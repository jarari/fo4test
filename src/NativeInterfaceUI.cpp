#include "NativeInterfaceUI.h"
#include "PipboyCursor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <utility>

#include "DX12SwapChain.h"
#include "ENBRenderDomain.h"
#include "Util.h"
#include "Upscaling.h"
#include <d3d11_1.h>

namespace WorldGuides
{
// Keep the raw world/first-person guides across Interface3D's offscreen
	// model and screen-mesh passes. FG owns a separate, earlier snapshot.
	class WorldGuideScope
	{
	public:
		explicit WorldGuideScope(bool enabled);
		~WorldGuideScope();
		WorldGuideScope(const WorldGuideScope&) = delete;
		WorldGuideScope& operator=(const WorldGuideScope&) = delete;

	private:
		bool captured = false;
	};

	void Release();

namespace
{
	winrt::com_ptr<ID3D11Texture2D> motionBackup, depthBackup;
	winrt::com_ptr<ID3D11Texture2D> motionSource, depthSource;
	winrt::com_ptr<ID3D11DeviceContext1> context;
	winrt::com_ptr<ID3DDeviceContextState> cleanState;
	bool active = false;

	struct ContextScope
	{
		winrt::com_ptr<ID3DDeviceContextState> previous;
		ContextScope() { context->SwapDeviceContextState(cleanState.get(), previous.put()); }
		~ContextScope() { context->SwapDeviceContextState(previous.get(), nullptr); }
	};

	void EnsureBackup(ID3D11Device* device, ID3D11Texture2D* source,
		winrt::com_ptr<ID3D11Texture2D>& backup)
	{
		D3D11_TEXTURE2D_DESC desc{}, previous{};
		source->GetDesc(&desc);
		if (backup) { backup->GetDesc(&previous); }
		if (backup && desc.Width == previous.Width && desc.Height == previous.Height &&
			desc.Format == previous.Format && desc.MipLevels == previous.MipLevels &&
			desc.ArraySize == previous.ArraySize && desc.SampleDesc.Count == previous.SampleDesc.Count &&
			desc.SampleDesc.Quality == previous.SampleDesc.Quality) {
			return;
		}
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		winrt::com_ptr<ID3D11Texture2D> next;
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, next.put()));
		backup = std::move(next);
	}
}

WorldGuideScope::WorldGuideScope(bool enabled)
{
	// A nested RenderAll remains inside the outer snapshot. In particular,
	// never replace the world backup with an intermediate model's guides.
	if (!enabled || active) { return; }
	try {
		auto* data = RE::BSGraphics::GetRendererData();
		auto* motion = reinterpret_cast<ID3D11Texture2D*>(data->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMotionVectors)].texture);
		auto* depth = reinterpret_cast<ID3D11Texture2D*>(data->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].texture);
		if (!motion || !depth) { return; }
		auto* device = reinterpret_cast<ID3D11Device*>(data->device);
		if (!context) {
			DX::ThrowIfFailed(reinterpret_cast<ID3D11DeviceContext*>(data->context)->QueryInterface(IID_PPV_ARGS(context.put())));
		}
		if (!cleanState) {
			winrt::com_ptr<ID3D11Device1> device1;
			DX::ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(device1.put())));
			const auto level = device->GetFeatureLevel();
			DX::ThrowIfFailed(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
				__uuidof(ID3D11Device), nullptr, cleanState.put()));
		}
		EnsureBackup(device, motion, motionBackup);
		EnsureBackup(device, depth, depthBackup);
		motionSource.copy_from(motion);
		depthSource.copy_from(depth);
		ContextScope scope;
		context->CopyResource(motionBackup.get(), motionSource.get());
		context->CopyResource(depthBackup.get(), depthSource.get());
		captured = active = true;
	} catch (const std::exception& e) {
		logger::error("[Interface3D motion] World guide preservation unavailable: {}", e.what());
	}
}

WorldGuideScope::~WorldGuideScope()
{
	if (!captured) { return; }
	{
		ContextScope scope;
		// Restore the exact resources captured before any native-depth slot
		// borrowing. Do not resolve the temporarily redirected engine slots.
		context->CopyResource(motionSource.get(), motionBackup.get());
		context->CopyResource(depthSource.get(), depthBackup.get());
	}
	motionSource = nullptr;
	depthSource = nullptr;
	active = false;
}

void Release()
{
	motionBackup = nullptr;
	depthBackup = nullptr;
	motionSource = nullptr;
	depthSource = nullptr;
	context = nullptr;
	cleanState = nullptr;
	active = false;
}

}

namespace
{
	bool enabled = false;
	bool pipboyAllocationReady = false;
	thread_local bool rendering = false;
	thread_local bool nativeScreenPass = false;
	thread_local int colorTarget = 0;
	thread_local int depthTarget = 0;
	bool modelHooksInstalled = false;
	thread_local uint32_t modelFrame = UINT32_MAX;
	thread_local std::array<RE::Interface3D::Renderer*, 64> renderedModels{};
	thread_local size_t renderedModelCount = 0;

	bool UsesNativeModelTargets(const RE::Interface3D::Renderer* renderer)
	{
		return renderer && renderer->postAA && renderer->screenAttachedElementRoot &&
			renderer->omsize.get() == RE::Interface3D::OffscreenMenuSize::kFullFrame;
	}

	void BeginNativeModelTargets(RE::Interface3D::Renderer* renderer);
	void EndNativeModelTargets(RE::Interface3D::Renderer* renderer = nullptr);
	bool HasReducedENBScene()
	{
		const auto& domain = ENBRenderDomain::Get();
		const auto* swap = DX12SwapChain::GetSingleton();
		return domain.Active() && (domain.Width() != swap->swapChainDesc.Width ||
			domain.Height() != swap->swapChainDesc.Height);
	}

	bool AlreadyRendered(RE::Interface3D::Renderer* a_renderer)
	{
		if (modelFrame != Util::State_GetSingleton()->frameCount) {
			return false;
		}
		for (size_t i = 0; i < renderedModelCount; ++i) {
			if (renderedModels[i] == a_renderer) {
				return true;
			}
		}
		return false;
	}

	// Patch RenderAll's calls, not the entry points: TF3DHUD's prepass detour
	// must run exactly once, including its CommitRenderState callback.
	struct ModelPrepasses
	{
		static void thunk(RE::Interface3D::Renderer* a_renderer)
		{
			if (a_renderer && a_renderer->postfx.get() == RE::Interface3D::PostEffect::kPipboy) {
			}
			if (!AlreadyRendered(a_renderer)) {
				const auto effect = a_renderer->postfx.get();
				const bool usesHUDGlass = effect == RE::Interface3D::PostEffect::kHUDGlass ||
					effect == RE::Interface3D::PostEffect::kHUDGlassWithMod ||
					(a_renderer->screenAttachedElementRoot &&
						a_renderer->screenMaterialName == "Materials\\Interface\\HUDGlassFlat.BGEM");
				if (nativeScreenPass && usesHUDGlass) {
					// HUDGlass and its shadow material do not produce a generic
					// coverage-alpha overlay. Supply the real destination BEFORE
					// prepasses/RenderMain, then keep the engine's completed RGB.
					if (DX12SwapChain::GetSingleton()->PrepareNativeUIForEngineComposition()) {
						static bool announced = false;
						if (!announced) {
							logger::info("[UI composite] HUDGlass uses engine frame composition; final alpha recomposition bypassed");
							announced = true;
						}
					}
				}
				BeginNativeModelTargets(a_renderer);
				func(a_renderer);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ModelMain
	{
		static void thunk(RE::Interface3D::Renderer* a_renderer, uint32_t a_target)
		{
			if (!AlreadyRendered(a_renderer)) {
				func(a_renderer, a_target);
			}
			EndNativeModelTargets(a_renderer);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void DirtyDepthBinding()
	{
		// Follow SetCurrentDepthStencilTarget/Flush's TLS lookup in both runtimes.
		// RendererData::shadowState is NOT a dereferenceable pointer in OG (0x1B70).
		static REL::Relocation<const uint32_t*> tlsIndex{ REL::ID{ 842564, 2787938 } };
		static REL::Relocation<std::byte**> defaultContext{ REL::ID{ 33539, 2704428 } };
		const auto* slots = reinterpret_cast<std::byte* const*>(__readgsqword(0x58));
		const auto* thread = slots[*tlsIndex];
		auto* context = thread ? *reinterpret_cast<std::byte* const*>(thread + 0xB20) : nullptr;
		if (!context) {
			context = *defaultContext;
		}
		if (context) {
			// Flush tests bit 0. The platform ID itself has not changed.
			*reinterpret_cast<uint32_t*>(context + 0x1B70) |= 1u;
		}
	}

	// Borrowed engine slot, restored before leaving RenderAll. The owned views
	// never replace engine ownership and are never passed to its destroy path.
	struct NativeDepth
	{
		winrt::com_ptr<ID3D11Texture2D> source;
		winrt::com_ptr<ID3D11Texture2D> texture;
		std::array<winrt::com_ptr<ID3D11DepthStencilView>, 16> views;
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> srvs;
		RE::BSGraphics::DepthStencilTarget target{};
		RE::BSGraphics::DepthStencilTarget original{};
		uint64_t generation = 0;
		uint32_t clearedFrame = UINT32_MAX;
		int slot = -1;

		void Restore()
		{
			if (slot >= 0) {
				RE::BSGraphics::GetRendererData()->depthStencilTargets[slot] = original;
				slot = -1;
				DirtyDepthBinding();
			}
		}

		void Bind(RE::BSGraphics::RenderTargetManager* a_manager, uint32_t a_logical = 1)
		{
			const auto offset = REX::FModule::IsRuntimeOG() ? 0xF54u : 0xF84u;
			const auto* ids = reinterpret_cast<const uint32_t*>(reinterpret_cast<const std::byte*>(a_manager) + offset);
			const auto index = ids[a_logical];
			auto* data = RE::BSGraphics::GetRendererData();
			if (index >= std::size(data->depthStencilTargets)) {
				return;
			}
			const auto& engine = data->depthStencilTargets[index];
			if (!engine.texture) {
				return;
			}
			auto* swap = DX12SwapChain::GetSingleton();
			auto* engineTexture = reinterpret_cast<ID3D11Texture2D*>(engine.texture);
			D3D11_TEXTURE2D_DESC desc{};
			engineTexture->GetDesc(&desc);
			if (desc.Width == swap->swapChainDesc.Width && desc.Height == swap->swapChainDesc.Height) {
				return;  // DLAA/native already has a matching engine depth target.
			}
			if (source.get() != engineTexture || generation != swap->NativeUIGeneration()) {
				// Build transactionally: an allocation failure leaves engine state intact.
				NativeDepth next;
				next.source.copy_from(engineTexture);
				desc.Width = swap->swapChainDesc.Width;
				desc.Height = swap->swapChainDesc.Height;
				desc.MiscFlags = 0;
				auto* device = reinterpret_cast<ID3D11Device*>(data->device);
				DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, next.texture.put()));
				next.target.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(next.texture.get());
				size_t viewIndex = 0;
				auto cloneViews = [&](const auto& a_source, auto& a_destination) {
					for (size_t i = 0; i < std::size(a_source); ++i, ++viewIndex) {
						if (a_source[i]) {
							D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
							reinterpret_cast<ID3D11DepthStencilView*>(a_source[i])->GetDesc(&viewDesc);
							DX::ThrowIfFailed(device->CreateDepthStencilView(next.texture.get(), &viewDesc, next.views[viewIndex].put()));
							a_destination[i] = reinterpret_cast<REX::W32::ID3D11DepthStencilView*>(next.views[viewIndex].get());
						}
					}
				};
				cloneViews(engine.dsView, next.target.dsView);
				cloneViews(engine.dsViewReadOnlyDepth, next.target.dsViewReadOnlyDepth);
				cloneViews(engine.dsViewReadOnlyStencil, next.target.dsViewReadOnlyStencil);
				cloneViews(engine.dsViewReadOnlyDepthStencil, next.target.dsViewReadOnlyDepthStencil);
				auto cloneSRV = [&](auto* a_source, auto*& a_destination, size_t a_index) {
					if (a_source) {
						D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
						reinterpret_cast<ID3D11ShaderResourceView*>(a_source)->GetDesc(&viewDesc);
						DX::ThrowIfFailed(device->CreateShaderResourceView(next.texture.get(), &viewDesc, next.srvs[a_index].put()));
						a_destination = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(next.srvs[a_index].get());
					}
				};
				cloneSRV(engine.srViewDepth, next.target.srViewDepth, 0);
				cloneSRV(engine.srViewStencil, next.target.srViewStencil, 1);
				next.generation = swap->NativeUIGeneration();
				*this = std::move(next);
				logger::info("[ENB UI] Native Interface3D depth{} {}x{}", a_logical, desc.Width, desc.Height);
			}
			const auto frame = Util::State_GetSingleton()->frameCount;
			if (clearedFrame != frame) {
				// Post-AA UI has its own depth/stencil; do not clear or overwrite world depth.
				auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
				for (size_t i = 0; i < 4; ++i) {
					if (views[i]) {
						D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
						views[i]->GetDesc(&viewDesc);
						const bool stencil = viewDesc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || viewDesc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
						context->ClearDepthStencilView(views[i].get(), D3D11_CLEAR_DEPTH | (stencil ? D3D11_CLEAR_STENCIL : 0), 1.0f, 0);
					}
				}
				clearedFrame = frame;
			}
			original = engine;
			slot = static_cast<int>(index);
			data->depthStencilTargets[index] = target;
			DirtyDepthBinding();
		}
	} nativeDepth;
	NativeDepth nativeMaskDepth;

	struct NativeModelColor
	{
		winrt::com_ptr<ID3D11Texture2D> texture, copyTexture;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11ShaderResourceView> srv, copySRV;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		RE::BSGraphics::RenderTarget target{}, original{};
		D3D11_TEXTURE2D_DESC description{};
		RE::BSGraphics::RenderTargetProperties properties{};
		uint32_t slot = UINT32_MAX;
		bool acquired = false;

		void Prepare(const RE::BSGraphics::RenderTarget& a_source, uint32_t a_width, uint32_t a_height)
		{
			D3D11_TEXTURE2D_DESC desc{};
			reinterpret_cast<ID3D11Texture2D*>(a_source.texture)->GetDesc(&desc);
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MiscFlags &= ~(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
			if (texture && std::memcmp(&desc, &description, sizeof(desc)) == 0 &&
				static_cast<bool>(copyTexture) == (a_source.copyTexture != nullptr)) {
				return;
			}
			NativeModelColor next;
			auto* device = reinterpret_cast<ID3D11Device*>(RE::BSGraphics::GetRendererData()->device);
			DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, next.texture.put()));
			if (a_source.rtView) {
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				reinterpret_cast<ID3D11RenderTargetView*>(a_source.rtView)->GetDesc(&view);
				DX::ThrowIfFailed(device->CreateRenderTargetView(next.texture.get(), &view, next.rtv.put()));
			}
			if (a_source.srView) {
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				reinterpret_cast<ID3D11ShaderResourceView*>(a_source.srView)->GetDesc(&view);
				DX::ThrowIfFailed(device->CreateShaderResourceView(next.texture.get(), &view, next.srv.put()));
			}
			if (a_source.uaView) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
				reinterpret_cast<ID3D11UnorderedAccessView*>(a_source.uaView)->GetDesc(&view);
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(next.texture.get(), &view, next.uav.put()));
			}
			if (a_source.copyTexture) {
				D3D11_TEXTURE2D_DESC copyDesc{};
				reinterpret_cast<ID3D11Texture2D*>(a_source.copyTexture)->GetDesc(&copyDesc);
				copyDesc.Width = a_width;
				copyDesc.Height = a_height;
				DX::ThrowIfFailed(device->CreateTexture2D(&copyDesc, nullptr, next.copyTexture.put()));
				if (a_source.copySRView) {
					D3D11_SHADER_RESOURCE_VIEW_DESC view{};
					reinterpret_cast<ID3D11ShaderResourceView*>(a_source.copySRView)->GetDesc(&view);
					DX::ThrowIfFailed(device->CreateShaderResourceView(next.copyTexture.get(), &view, next.copySRV.put()));
				}
			}
			next.target.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(next.texture.get());
			next.target.copyTexture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(next.copyTexture.get());
			next.target.rtView = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(next.rtv.get());
			next.target.srView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(next.srv.get());
			next.target.copySRView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(next.copySRV.get());
			next.target.uaView = reinterpret_cast<REX::W32::ID3D11UnorderedAccessView*>(next.uav.get());
			next.description = desc;
			// Preserve this pass's engine acquisition while replacing the cache.
			next.acquired = acquired;
			*this = std::move(next);
		}
	};

	// Logical IDs, not RendererData's allocation-order indices. These are the
	// deferred buffers and outputs selected from each renderer's configuration.
	// Never resize their world allocations or ENB's private resources globally.
	std::array<NativeModelColor, 100> modelColors;
	thread_local bool modelTargetsActive = false;
	thread_local RE::Interface3D::Renderer* modelTargetOwner = nullptr;
	thread_local bool maskDepthAcquired = false;
	using TargetLifetime = void (*)(RE::BSGraphics::RenderTargetManager*, int);

	void EndNativeModelTargets(RE::Interface3D::Renderer* a_renderer)
	{
		if (!modelTargetsActive || (a_renderer && a_renderer != modelTargetOwner)) { return; }
		nativeDepth.Restore();
		nativeMaskDepth.Restore();
		auto* manager = Util::RenderTargetManager_GetSingleton();
		auto* data = RE::BSGraphics::GetRendererData();
		if (maskDepthAcquired) {
			static REL::Relocation<TargetLifetime> releaseMaskDepth{ REL::ID{ 922599, 2277222 } };
			releaseMaskDepth(manager, 4);
			maskDepthAcquired = false;
		}
		static REL::Relocation<TargetLifetime> releaseModelTarget{ REL::ID{ 1374956, 2277220 } };
		for (size_t i = 0; i < modelColors.size(); ++i) {
			auto& color = modelColors[i];
			if (color.slot != UINT32_MAX) {
				data->renderTargets[color.slot] = color.original;
				manager->renderTargetData[static_cast<int>(i)] = color.properties;
				color.slot = UINT32_MAX;
			}
			if (color.acquired) {
				// Release only AFTER restoring engine ownership. The extra acquire
				// pins pooled RT37/52 across the prepass's own Acquire/Release pair
				// and keeps their native output available to RenderMain.
				releaseModelTarget(manager, static_cast<int>(i));
				color.acquired = false;
			}
		}
		modelTargetsActive = false;
		modelTargetOwner = nullptr;
		DirtyDepthBinding();
	}

	void BeginNativeModelTargets(RE::Interface3D::Renderer* a_renderer)
	{
		if (!nativeScreenPass || !HasReducedENBScene() ||
			modelTargetsActive || !UsesNativeModelTargets(a_renderer)) { return; }
		// Keep Workbench's entire model/ModMenu intermediate chain on the
		// existing engine allocations. ENB requires scene-sized deferred output.
		// RT62/63 and final UI composition retain their existing allocation policy.
		if (a_renderer->name == "WorkbenchItem3D") { return; }
		auto* swap = DX12SwapChain::GetSingleton();
		if (!swap->IsNativeUIActive()) { return; }
		auto* manager = Util::RenderTargetManager_GetSingleton();
		auto* data = RE::BSGraphics::GetRendererData();
		const auto offset = REX::FModule::IsRuntimeOG() ? 0xDC4u : 0xDF4u;
		const auto* ids = reinterpret_cast<const uint32_t*>(reinterpret_cast<const std::byte*>(manager) + offset);
		static REL::Relocation<TargetLifetime> acquireModelTarget{ REL::ID{ 1468639, 2277219 } };
		static REL::Relocation<TargetLifetime> releaseModelTarget{ REL::ID{ 1374956, 2277220 } };
		modelTargetsActive = true;
		modelTargetOwner = a_renderer;
		try {
			const auto fx = a_renderer->postfx.get();
			const bool offscreenModel = a_renderer->offscreen3DEnabled && (a_renderer->offscreenElement ||
				(a_renderer->highlightedElement && a_renderer->highlightOffscreen));
			const bool deferred = a_renderer->defRenderMainScreen ||
				(offscreenModel && (fx == RE::Interface3D::PostEffect::kHUDGlassWithMod ||
				fx == RE::Interface3D::PostEffect::kModMenu ||
				fx == RE::Interface3D::PostEffect::kModMenuHighlightAll ||
				fx == RE::Interface3D::PostEffect::kModMenuHighlightAllNoPulseOrScanLines));
			std::array<bool, 100> selected{};
			auto select = [&](int target) {
				// RT0 is the final UI destination; fixed-size Pipboy/text surfaces
				// have a separate allocation policy. Never borrow their slots here.
				if (target > 0 && target < 100 && target != 60 && target != 61 && target != 64) {
					selected[target] = true;
				}
			};
			if (deferred) {
				for (int target : {26, 27, 29, 30, 31, 32, 33, 34}) { select(target); }
			}
			// ModMenu consumes Albedo26/Mask15/Normals27/Image37 together.
			// Its RenderMask producer writes RT15 with depth4, independently of
			// the model's G-buffer/depth1. Keep both producer targets native.
			const bool modMenu = fx == RE::Interface3D::PostEffect::kModMenu ||
				fx == RE::Interface3D::PostEffect::kModMenuHighlightAll ||
				fx == RE::Interface3D::PostEffect::kModMenuHighlightAllNoPulseOrScanLines;
			if (modMenu) {
				select(15);
				static REL::Relocation<TargetLifetime> acquireMaskDepth{ REL::ID{ 1015879, 2277221 } };
				acquireMaskDepth(manager, 4);
				maskDepthAcquired = true;
				nativeMaskDepth.Bind(manager, 4);
			}
			if (offscreenModel) { select(52); }
			if (offscreenModel && (fx == RE::Interface3D::PostEffect::kHUDGlassWithMod ||
				fx == RE::Interface3D::PostEffect::kModMenu)) { select(37); }
			select(a_renderer->customRenderTarget >= 0 ? a_renderer->customRenderTarget : 63);
			select(a_renderer->customSwapTarget >= 0 ? a_renderer->customSwapTarget : 63);
			bool allocated = false;
			// Allocate the entire set before publishing any replacement. Optional
			// G-buffer outputs are absent when the engine disables their feature.
			for (size_t i = 0; i < modelColors.size(); ++i) {
				const auto logical = static_cast<int>(i);
				if (!selected[i]) { continue; }
				if (!manager->renderTargetData[logical].width || !manager->renderTargetData[logical].height) { continue; }
				acquireModelTarget(manager, logical);
				auto& color = modelColors[i];
				color.acquired = true;
				if (ids[logical] >= std::size(data->renderTargets) || !data->renderTargets[ids[logical]].texture) {
					throw std::runtime_error("Interface3D render target is unavailable");
				}
				const auto& source = data->renderTargets[ids[logical]];
				D3D11_TEXTURE2D_DESC desc{};
				reinterpret_cast<ID3D11Texture2D*>(source.texture)->GetDesc(&desc);
				if (desc.Width == swap->swapChainDesc.Width && desc.Height == swap->swapChainDesc.Height) {
					releaseModelTarget(manager, logical);
					color.acquired = false;
					continue;
				}
				allocated |= !color.texture || color.description.Width != swap->swapChainDesc.Width || color.description.Height != swap->swapChainDesc.Height;
				color.Prepare(source, swap->swapChainDesc.Width, swap->swapChainDesc.Height);
			}
			for (size_t i = 0; i < modelColors.size(); ++i) {
				auto& color = modelColors[i];
				if (!color.acquired) { continue; }
				const auto logical = static_cast<int>(i);
				color.slot = ids[logical];
				color.original = data->renderTargets[color.slot];
				color.properties = manager->renderTargetData[logical];
				data->renderTargets[color.slot] = color.target;
				manager->renderTargetData[logical].width = swap->swapChainDesc.Width;
				manager->renderTargetData[logical].height = swap->swapChainDesc.Height;
			}
			if (nativeDepth.slot < 0) { nativeDepth.Bind(manager); }
			DirtyDepthBinding();
			if (allocated) {
				logger::info("[ENB UI] Native model targets for {}: {}x{}", a_renderer->name.c_str(), swap->swapChainDesc.Width, swap->swapChainDesc.Height);
			}
		} catch (const std::exception& e) {
			EndNativeModelTargets();
			logger::error("[ENB UI] Native model targets unavailable: {}", e.what());
		}
	}

	// RT62/63 are native-sized even during pre-AA/model work. Their DSV must
	// follow each color binding, not just the post-AA RenderAll invocation.
	struct RenderScope
	{
		RenderScope()
		{
			rendering = true;
		}
		~RenderScope()
		{
			EndNativeModelTargets();
			nativeDepth.Restore();
			nativeMaskDepth.Restore();
			rendering = false;
		}
	};

	void UpdateDepthBinding(RE::BSGraphics::RenderTargetManager* a_manager)
	{
		const auto* swap = DX12SwapChain::GetSingleton();
		const bool wantNative = depthTarget == 1 && colorTarget >= 0 && colorTarget < 100 &&
			a_manager->renderTargetData[colorTarget].width == swap->swapChainDesc.Width &&
			a_manager->renderTargetData[colorTarget].height == swap->swapChainDesc.Height;
		// SetColor/SetDepth frequently repeat an unchanged binding. Do not undo
		// and redo the borrowed slot: that dirties OM state on EVERY such call,
		// defeating the engine's redundant-state suppression (and ENB's cache).
		// Resize cannot occur inside the synchronous RenderAll scope.
		if (wantNative && nativeDepth.slot >= 0) {
			return;
		}
		nativeDepth.Restore();
		const auto& domain = ENBRenderDomain::Get();
		if (wantNative && (domain.Width() != swap->swapChainDesc.Width || domain.Height() != swap->swapChainDesc.Height)) {
			try {
				nativeDepth.Bind(a_manager);
			} catch (const std::exception& e) {
				logger::error("[ENB UI] Native depth allocation failed: {}", e.what());
			}
		}
	}

	bool PromotePipboyExtent(uint32_t& width, uint32_t& height)
	{
		const auto displayHeight = DX12SwapChain::GetSingleton()->swapChainDesc.Height;
		if (!enabled || !pipboyAllocationReady || !ENBRenderDomain::Get().Active() ||
			!width || !height || height >= displayHeight) { return false; }
		// Keep the INI aspect and logical menu coordinates. Only the physical
		// color/depth allocation grows; never reduce a user's higher INI setting.
		const auto scaledWidth = (uint64_t{ width } * displayHeight + height - 1) / height;
		if (scaledWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			displayHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) { return false; }
		width = static_cast<uint32_t>(scaledWidth);
		height = displayHeight;
		return true;
	}

	struct CreateDepthTarget
	{
		static void thunk(RE::BSGraphics::RenderTargetManager* manager, int target,
			const void* properties, int persistency)
		{
			if (target != 3) { func(manager, target, properties, persistency); return; }
			// AE adds a field at +0x18. Preserve its full descriptor instead of
			// copying CommonLib's OG-sized DepthStencilTargetProperties.
			std::array<uint32_t, 7> copy{};
			std::memcpy(copy.data(), properties, REX::FModule::IsRuntimeOG() ? 0x18 : 0x1C);
			if (PromotePipboyExtent(copy[0], copy[1])) {
				logger::info("[ENB UI] Pipboy depth3 {}x{}", copy[0], copy[1]);
			}
			func(manager, target, copy.data(), persistency);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CreateTarget
	{
		static void thunk(RE::BSGraphics::RenderTargetManager* a_manager, int a_target,
			const RE::BSGraphics::RenderTargetProperties& a_properties, int a_persistency)
		{
			auto properties = a_properties;
			const auto* swap = DX12SwapChain::GetSingleton();
			// HUDGlass's input and mask output. Resize follows engine ownership.
			if (enabled && ENBRenderDomain::Get().Active() && (a_target == 62 || a_target == 63) &&
				properties.mipLevel < 0 && swap->swapChainDesc.Width && swap->swapChainDesc.Height) {
				properties.width = swap->swapChainDesc.Width;
				properties.height = swap->swapChainDesc.Height;
				logger::info("[ENB UI] Native custom RT{} {}x{} -> {}x{}", a_target,
					a_properties.width, a_properties.height, properties.width, properties.height);
			}
			if ((a_target == 60 || a_target == 61) && properties.mipLevel < 0 &&
				PromotePipboyExtent(properties.width, properties.height)) {
				logger::info("[ENB UI] Pipboy RT{} {}x{} -> {}x{}", a_target,
					a_properties.width, a_properties.height, properties.width, properties.height);
			}
			func(a_manager, a_target, properties, a_persistency);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetColor
	{
		static void thunk(RE::BSGraphics::RenderTargetManager* a_manager, int a_slot, int a_target, RE::BSGraphics::SetRenderTargetMode a_mode)
		{
			if (a_slot == 0) {
				colorTarget = a_target;
				if (rendering) {
					UpdateDepthBinding(a_manager);
				}
			}
			func(a_manager, a_slot, a_target, a_mode);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetDepth
	{
		static void thunk(RE::BSGraphics::RenderTargetManager* a_manager, int a_target,
			RE::BSGraphics::SetRenderTargetMode a_mode, int a_slice, bool a_readOnly)
		{
			depthTarget = a_target;
			if (rendering) {
				UpdateDepthBinding(a_manager);
			}
			func(a_manager, a_target, a_mode, a_slice, a_readOnly);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool InstallDepthHook()
	{
		const REL::ID id{ 704517, 2277189 };
		const auto entry = id.address();
		std::uintptr_t previous = 0;
		if (stl::get_jump_destination(entry, previous)) {
			// Preserve an existing mod's entry detour using the ordinary gateway.
			return stl::detour_thunk_gateway<SetDepth>(id, 7, "Interface3D native depth binding");
		}
		if (!stl::is_readable_memory(entry, 7)) {
			return false;
		}
		const auto* original = reinterpret_cast<const uint8_t*>(entry);
		int32_t displacement = 0;
		std::memcpy(&displacement, original + 3, sizeof(displacement));
		const auto source = static_cast<std::uintptr_t>(static_cast<intptr_t>(entry + 7) + displacement);
		const auto tlsIndex = REL::ID{ 842564, 2787938 }.address();
		if (original[0] != 0x44 || original[1] != 0x8B || original[2] != 0x15 || source != tlsIndex) {
			logger::error("[ENB UI] Unexpected depth entry; native depth hook not installed");
			return false;
		}
		// Original: mov r10d, [rip + _tls_index]. A gateway close enough to
		// the function for JMP rel32 need not be close enough to its data.
		// Expand the load without changing flags or any other register:
		// mov r10, imm64; mov r10d, [r10]; jmp absolute entry+7.
		std::array<uint8_t, 13> load{ 0x49, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0, 0x45, 0x8B, 0x12 };
		std::memcpy(load.data() + 2, &tlsIndex, sizeof(tlsIndex));
		auto* gateway = static_cast<std::byte*>(REL::GetTrampoline().allocate(load.size() + sizeof(REL::ASM::JMP14)));
		std::memcpy(gateway, load.data(), load.size());
		const REL::ASM::JMP14 continuation{ entry + 7 };
		std::memcpy(gateway + load.size(), &continuation, sizeof(continuation));
		::FlushInstructionCache(::GetCurrentProcess(), gateway, load.size() + sizeof(continuation));
		if (!stl::write_branch5(entry, reinterpret_cast<std::uintptr_t>(&SetDepth::thunk))) {
			return false;
		}
		SetDepth::func = REL::Relocation<decltype(SetDepth::thunk)>{ reinterpret_cast<std::uintptr_t>(gateway) };
		logger::info("[ENB UI] Installed depth gateway with absolute TLS load");
		return true;
	}

	struct RenderAll
	{
		static void thunk(uint32_t a_target, bool a_postAA)
		{
			// Capture before RenderScope can borrow the native depth slot; restore
			// after all models and their screen meshes, including early returns.
			WorldGuides::WorldGuideScope worldGuides(enabled && !a_postAA);
			// Composition is common to both proxy paths; native depth/viewport
			// routing remains ENB-only. Restore the pass flag across nested calls.
			struct ScreenPassScope
			{
				bool previous = nativeScreenPass;
				explicit ScreenPassScope(bool a_screenPass) { nativeScreenPass = a_screenPass; }
				~ScreenPassScope() { nativeScreenPass = previous; }
			} screenPass(enabled && !rendering && a_postAA && a_target == 0);
			if (!enabled || !ENBRenderDomain::Get().Active() || rendering) {
				func(a_target, a_postAA);
				return;
			}
			PipboyCursor::RefreshViewport();
			if (a_postAA && a_target == 0) {
				DX12SwapChain::GetSingleton()->BeginNativeUI();
			}
			RenderScope scope;
			// Includes MainMenu/FlatScreenModel, which render BEFORE ScreenSpace_RenderMenus.
			// Leave native RT0 active for subsequent 2D UI and the D3D12 present composite.
			func(a_target, a_postAA);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

bool NativeInterfaceUI::IsRendering()
{
	return rendering;
}

void NativeInterfaceUI::RenderModelsBeforeUpscale(uint32_t a_target)
{
	auto* upscaling = Upscaling::GetSingleton();
	if (!enabled || !modelHooksInstalled || rendering || a_target != 0 ||
		!ENBRenderDomain::Get().Active() || !upscaling->IsD3D12DLSSActive() ||
		upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDLSS) {
		return;
	}
	static REL::Relocation<const bool*> disabled{ REL::ID{ 789743, 2696451, 4803742 } };
	static REL::Relocation<const bool*> preAAEnabled{ REL::ID{ 339016, 2696449, 4803740 } };
	if (*disabled || !*preAAEnabled) {
		return;
	}
	const auto frame = Util::State_GetSingleton()->frameCount;
	if (modelFrame != frame) {
		modelFrame = frame;
		renderedModelCount = 0;
	}
	static REL::Relocation<RE::BSTArray<RE::Interface3D::Renderer*>*> renderers{ REL::ID{ 996993, 2696455, 4803746 } };
	static REL::Relocation<RE::BSReadWriteLock*> lock{ REL::ID{ 778095, 2696454, 4803745 } };
	static REL::Relocation<bool*> shaderPostAA{ REL::ID{ 801215, 2712496 } };
	static REL::Relocation<uint32_t*> displayTarget{ REL::ID{ 113725, 2712501 } };
	using HasMenus = bool (*)(RE::UI*, const RE::BSFixedString&);
	static REL::Relocation<HasMenus> hasMenus{ REL::ID{ 1574554, 2284758 } };

	RE::BSAutoReadLock listLock(*lock);
	WorldGuides::WorldGuideScope worldGuides(true);
	RenderScope scope;
	struct ShaderScope
	{
		bool previousPostAA = *shaderPostAA;
		uint32_t previousTarget = *displayTarget;
		ShaderScope() { *shaderPostAA = true; }
		~ShaderScope() { *shaderPostAA = previousPostAA; *displayTarget = previousTarget; }
	} shaderScope;
	for (auto* renderer : *renderers) {
		if (renderedModelCount == renderedModels.size()) {
			break;
		}
		if (!renderer || !renderer->enabled || !renderer->postAA || AlreadyRendered(renderer) ||
			(HasReducedENBScene() && UsesNativeModelTargets(renderer)) ||
			!renderer->screenAttachedElementRoot ||
			renderer->omsize.get() != RE::Interface3D::OffscreenMenuSize::kFullFrame) {
			continue;
		}
		const auto fx = renderer->postfx.get();
		const bool hasModel = renderer->defRenderMainScreen ||
			(renderer->offscreen3DEnabled && (renderer->offscreenElement ||
				(renderer->highlightedElement && renderer->highlightOffscreen)));
		if (!hasModel || fx == RE::Interface3D::PostEffect::kPipboy ||
			fx == RE::Interface3D::PostEffect::kHUDGlass ||
			fx == RE::Interface3D::PostEffect::kHUDGlassWithMod ||
			(RE::UI::GetSingleton() && hasMenus(RE::UI::GetSingleton(), renderer->name))) {
			continue;
		}
		// RT63 is shared with HUDGlass. Produce and consume this renderer's
		// output consecutively, with the native-color depth hook active for
		// every intermediate bind. Never cache an SRV for later composition.
		ModelPrepasses::thunk(renderer);
		ModelMain::thunk(renderer, a_target);
		renderedModels[renderedModelCount++] = renderer;
		RE::BSAutoWriteLock quadsLock(renderer->cachedQuadsLock);
		renderer->colorFXInfos.clear();
		renderer->backgroundFXInfos.clear();
	}
}

void NativeInterfaceUI::ReleaseResources()
{
	EndNativeModelTargets();
	modelColors = {};
	WorldGuides::Release();
	// Called by the interop resize transaction, after outstanding GPU work drains.
	// Do not retain a former device's depth allocation across a resize/recreation.
	nativeDepth.Restore();
	nativeDepth = {};
	nativeMaskDepth.Restore();
	nativeMaskDepth = {};
	modelFrame = UINT32_MAX;
	renderedModelCount = 0;
}

void NativeInterfaceUI::InstallHooks(bool a_nativeDomains)
{
	PipboyCursor::InstallHooks(a_nativeDomains);
	const auto isOG = REX::FModule::IsRuntimeOG();
	bool nativeHooksReady = true;
	if (a_nativeDomains) {
		const auto create = stl::detour_thunk_gateway<CreateTarget>(REL::ID{ 43433, 2277176 }, isOG ? 5 : 6, "Interface3D native target allocation");
		const auto color = stl::detour_thunk_gateway<SetColor>(REL::ID{ 1502425, 2277188 }, isOG ? 6 : 5, "Interface3D color target tracking");
		const auto depth = InstallDepthHook();
		const auto pipboyDepth = stl::detour_thunk_gateway<CreateDepthTarget>(REL::ID{ 1159619, 2277177 },
			isOG ? 5 : 6, "Interface3D Pipboy depth allocation");
		pipboyAllocationReady = create && pipboyDepth;
		nativeHooksReady = create && color && depth;
	}
	// Hook the worker, not RenderPostAA's MOV DL,1 / relative tail jump.
	const auto render = stl::detour_thunk_gateway<RenderAll>(REL::ID{ 1030129, 2222565 }, 8, "Interface3D native post-AA UI");
	enabled = nativeHooksReady && render;
	const auto renderAll = REL::ID{ 1030129, 2222565 }.address();
	const auto prepassCall = renderAll + (isOG ? 0x92 : 0x145);
	const auto mainCall = renderAll + (isOG ? 0x9D : 0x150);
	if (enabled && stl::is_readable_memory(prepassCall, 5) && stl::is_readable_memory(mainCall, 5) &&
		*reinterpret_cast<const uint8_t*>(prepassCall) == 0xE8 &&
		*reinterpret_cast<const uint8_t*>(mainCall) == 0xE8) {
		stl::write_thunk_call<ModelPrepasses>(prepassCall);
		stl::write_thunk_call<ModelMain>(mainCall);
		modelHooksInstalled = true;
	} else {
		logger::error("[ENB UI] Interface3D model call sites unavailable; model routing disabled");
	}
	if (!enabled) {
		logger::error("[ENB UI] Incomplete Interface3D hooks; native custom path disabled");
	}
}

namespace
{
	// The Pip-Boy cursor viewport rect is a raw .rdata constant with no stable
	// address-library id across runtimes, so locate it structurally instead of by
	// id: the cursor movie's root-path literal sits immediately before it, and the
	// rect itself is the unique { 0, 0, 1920, 1080 } quad that follows. Verified on
	// 1.10.984 at .rdata 0x23A9E40, directly after "root1.Cursor_mc" at 0x23A9E30.
	std::int32_t* FindPipboyCursorViewportRect()
	{
		static constexpr char kCursorRoot[]{ "root1.Cursor_mc" };
		static constexpr std::int32_t kStockRect[]{ 0, 0, 1920, 1080 };
		const auto* base = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr));
		if (!base) {
			return nullptr;
		}
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto* section = IMAGE_FIRST_SECTION(nt);
		for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
			if (std::memcmp(section->Name, ".rdata", 6) != 0) {
				continue;
			}
			const auto* begin = base + section->VirtualAddress;
			const auto* end = begin + section->Misc.VirtualSize;
			for (const auto* at = begin;;) {
				at = std::search(at, end, std::begin(kCursorRoot), std::end(kCursorRoot));
				if (at == end) {
					break;
				}
				const auto* probe = at + sizeof(kCursorRoot);
				for (; probe + sizeof(kStockRect) <= end && probe < at + 0x40; probe += sizeof(std::int32_t)) {
					if (std::memcmp(probe, kStockRect, sizeof(kStockRect)) == 0) {
						return const_cast<std::int32_t*>(reinterpret_cast<const std::int32_t*>(probe));
					}
				}
				at += sizeof(kCursorRoot);
			}
		}
		return nullptr;
	}
}

void NativeInterfaceUI::ScaleLegacyNGPipboyLogicalSpace(uint32_t a_displayHeight)
{
	// PromotePipboyExtent grows only the PHYSICAL Pip-Boy colour/depth allocation to
	// the display height. Three LOGICAL quantities describe that same surface, and if
	// they do not follow the identical growth they end up describing a small top-left
	// corner of it:
	//   * uPipboyTarget{Width,Height} - the offscreen buffer size the engine reports
	//     through Interface3D::Renderer::Offscreen_GetRenderTarget{Width,Height}.
	//   * uPipboyConstraint* - where Pip-Boy content is placed inside that buffer.
	//   * CursorMenu's hard-coded { 0, 0, 1920, 1080 } viewport rect. CursorMenu
	//     special-cases the "PipboyMenu" renderer and hand-builds its viewport rather
	//     than going through Interface3D::Renderer::SetViewport like every other
	//     custom renderer, and Scaleform clips the draw to min(rect, buffer). With a
	//     promoted target that stranded the mouse cursor sprite inside a
	//     uPipboyTarget-sized top-left window of the map, and no INI setting can
	//     reach the 1920x1080 constant.
	// Scaling all three by one factor leaves the Pip-Boy visually identical while the
	// cursor both covers and correctly addresses the whole surface.
	static bool scaled = false;
	if (!REX::FModule::IsRuntimeNG() || scaled || !a_displayHeight || !ENBRenderDomain::Get().Active()) {
		return;
	}
	// All or nothing: scaling the buffer without the rect would move the clip rather
	// than remove it, so bail out entirely if the rect cannot be located.
	auto* cursorRect = FindPipboyCursorViewportRect();
	if (!cursorRect) {
		logger::warn("[ENB UI] Pip-Boy cursor viewport rect not found; leaving the Pip-Boy logical space unscaled");
		return;
	}
	auto* targetWidth = RE::GetINISetting("uPipboyTargetWidth:Display");
	auto* targetHeight = RE::GetINISetting("uPipboyTargetHeight:Display");
	if (!targetWidth || !targetHeight) {
		return;
	}
	const auto logicalWidth = targetWidth->GetUInt();
	const auto logicalHeight = targetHeight->GetUInt();
	if (!logicalWidth || !logicalHeight || logicalHeight >= a_displayHeight) {
		return;  // already display-sized: nothing is promoted, so nothing to scale
	}
	const auto scale = static_cast<double>(a_displayHeight) / static_cast<double>(logicalHeight);
	const auto scaleValue = [scale](uint32_t a_value) {
		return static_cast<uint32_t>(std::lround(static_cast<double>(a_value) * scale));
	};

	// Matches PromotePipboyExtent's own aspect-preserving growth, so the engine now
	// allocates the promoted extent directly and the promotion becomes a no-op.
	targetWidth->SetUInt(scaleValue(logicalWidth));
	targetHeight->SetUInt(a_displayHeight);

	static constexpr const char* constraints[]{
		"uPipboyConstraintTLX:Pipboy",
		"uPipboyConstraintTLY:Pipboy",
		"uPipboyConstraintWidth:Pipboy",
		"uPipboyConstraintHeight:Pipboy",
		"uPipboyConstraintTLX_PowerArmor:Pipboy",
		"uPipboyConstraintTLY_PowerArmor:Pipboy",
		"uPipboyConstraintWidth_PowerArmor:Pipboy",
		"uPipboyConstraintHeight_PowerArmor:Pipboy"
	};
	for (const auto* name : constraints) {
		if (auto* setting = RE::GetINISetting(name)) {
			setting->SetUInt(scaleValue(setting->GetUInt()));
		}
	}

	// { left, top, width, height }; only the extent grows, the origin stays 0,0.
	const auto rectWidth = static_cast<std::int32_t>(scaleValue(1920));
	const auto rectHeight = static_cast<std::int32_t>(scaleValue(1080));
	REL::WriteSafeData(reinterpret_cast<std::uintptr_t>(cursorRect + 2), rectWidth);
	REL::WriteSafeData(reinterpret_cast<std::uintptr_t>(cursorRect + 3), rectHeight);

	scaled = true;
	logger::info("[ENB UI] Scaled Pip-Boy logical space x{:.4f}: target {}x{} -> {}x{}, cursor rect 1920x1080 -> {}x{}",
		scale, logicalWidth, logicalHeight, targetWidth->GetUInt(), targetHeight->GetUInt(),
		rectWidth, rectHeight);
}
