#include "NativeInterfaceUI.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <intrin.h>
#include <utility>

#include "DX12SwapChain.h"
#include "ENBRenderDomain.h"
#include "Util.h"
#include "RenderProfiling.h"
#include "Upscaling.h"

namespace
{
	bool enabled = false;
	thread_local bool rendering = false;
	thread_local int colorTarget = 0;
	thread_local int depthTarget = 0;
	bool modelHooksInstalled = false;
	thread_local uint32_t modelFrame = UINT32_MAX;
	thread_local std::array<RE::Interface3D::Renderer*, 64> renderedModels{};
	thread_local size_t renderedModelCount = 0;

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
			if (!AlreadyRendered(a_renderer)) {
				const ScopedRenderCPUProfile timing("Interface3D-prepasses", a_renderer->name.c_str());
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
				const ScopedRenderCPUProfile timing("Interface3D-main", a_renderer->name.c_str());
				func(a_renderer, a_target);
			}
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

		void Bind(RE::BSGraphics::RenderTargetManager* a_manager)
		{
			const auto offset = REX::FModule::IsRuntimeOG() ? 0xF54u : 0xF84u;
			const auto* ids = reinterpret_cast<const uint32_t*>(reinterpret_cast<const std::byte*>(a_manager) + offset);
			const auto index = ids[1];
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
				logger::info("[ENB UI] Native Interface3D depth/stencil {}x{}", desc.Width, desc.Height);
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

	// RT62/63 are native-sized even during pre-AA/model work. Their DSV must
	// follow each color binding, not just the post-AA RenderAll invocation.
	struct RenderScope
	{
		RenderScope() { rendering = true; }
		~RenderScope() { nativeDepth.Restore(); rendering = false; }
	};

	void TraceModelOutput(RE::Interface3D::Renderer* a_renderer, uint32_t a_target)
	{
		if (!Upscaling::GetSingleton()->settings.enbGPUTiming ||
			Util::State_GetSingleton()->frameCount % 120 != 0 || a_target >= 100) {
			return;
		}
		auto* manager = Util::RenderTargetManager_GetSingleton();
		const auto mappingOffset = REX::FModule::IsRuntimeOG() ? 0xDC4u : 0xDF4u;
		const auto* ids = reinterpret_cast<const uint32_t*>(reinterpret_cast<const std::byte*>(manager) + mappingOffset);
		auto* data = RE::BSGraphics::GetRendererData();
		if (ids[a_target] >= std::size(data->renderTargets)) {
			return;
		}
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
		winrt::com_ptr<ID3D11RenderTargetView> color;
		winrt::com_ptr<ID3D11DepthStencilView> depth;
		context->OMGetRenderTargets(1, color.put(), depth.put());
		auto extent = [](ID3D11View* a_view) {
			D3D11_TEXTURE2D_DESC desc{};
			if (a_view) {
				winrt::com_ptr<ID3D11Resource> resource;
				a_view->GetResource(resource.put());
				if (auto texture = resource.try_as<ID3D11Texture2D>()) {
					texture->GetDesc(&desc);
				}
			}
			return desc;
		};
		const auto c = extent(color.get());
		const auto d = extent(depth.get());
		const auto& expected = data->renderTargets[ids[a_target]];
		const bool targetMatches = color.get() == reinterpret_cast<ID3D11RenderTargetView*>(expected.rtView);
		const bool depthMatches = !depth || (c.Width == d.Width && c.Height == d.Height &&
			c.SampleDesc.Count == d.SampleDesc.Count);
		logger::info("[Interface3D model output] frame={} renderer={} L{}->P{} bound-match={} color={}x{} depth={}x{} compatible={} native-depth={}",
			Util::State_GetSingleton()->frameCount, a_renderer->name.c_str(), a_target, ids[a_target], targetMatches,
			c.Width, c.Height, d.Width, d.Height, depthMatches, nativeDepth.slot >= 0);
	}

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

	struct CreateTarget
	{
		static void thunk(RE::BSGraphics::RenderTargetManager* a_manager, int a_target,
			const RE::BSGraphics::RenderTargetProperties& a_properties, int a_persistency)
		{
			auto properties = a_properties;
			const auto* swap = DX12SwapChain::GetSingleton();
			// HUDGlass's input and mask output. Keep fixed-size Pipboy/text targets
			// and all world/ENB targets unchanged. Resize follows normal engine ownership.
			if (enabled && ENBRenderDomain::Get().Active() && (a_target == 62 || a_target == 63) &&
				properties.mipLevel < 0 && swap->swapChainDesc.Width && swap->swapChainDesc.Height) {
				properties.width = swap->swapChainDesc.Width;
				properties.height = swap->swapChainDesc.Height;
				logger::info("[ENB UI] Native custom RT{} {}x{} -> {}x{}", a_target,
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
			if (!enabled || !ENBRenderDomain::Get().Active() || rendering) {
				func(a_target, a_postAA);
				return;
			}
			if (a_postAA && a_target == 0) {
				DX12SwapChain::GetSingleton()->BeginNativeUI();
			}
			RenderScope scope;
			// Includes MainMenu/FlatScreenModel, which render BEFORE ScreenSpace_RenderMenus.
			// Leave native RT0 active for subsequent 2D UI and the D3D12 present composite.
			const ScopedRenderCPUProfile timing("Interface3D-native (inclusive)", a_postAA ? "post-AA" : "pre-AA");
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
	static REL::Relocation<const bool*> disabled{ REL::ID{ 789743, 4803742 } };
	static REL::Relocation<const bool*> preAAEnabled{ REL::ID{ 339016, 4803740 } };
	if (*disabled || !*preAAEnabled) {
		return;
	}
	const auto frame = Util::State_GetSingleton()->frameCount;
	if (modelFrame != frame) {
		modelFrame = frame;
		renderedModelCount = 0;
	}
	static REL::Relocation<RE::BSTArray<RE::Interface3D::Renderer*>*> renderers{ REL::ID{ 996993, 4803746 } };
	static REL::Relocation<RE::BSReadWriteLock*> lock{ REL::ID{ 778095, 4803745 } };
	static REL::Relocation<bool*> shaderPostAA{ REL::ID{ 801215, 2712496 } };
	static REL::Relocation<uint32_t*> displayTarget{ REL::ID{ 113725, 2712501 } };
	using HasMenus = bool (*)(RE::UI*, const RE::BSFixedString&);
	static REL::Relocation<HasMenus> hasMenus{ REL::ID{ 1574554, 2284758 } };

	RE::BSAutoReadLock listLock(*lock);
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
		{
			const ScopedRenderCPUProfile timing("Interface3D-prepasses-before-upscale", renderer->name.c_str());
			ModelPrepasses::func(renderer);
		}
		TraceModelOutput(renderer, *displayTarget);
		{
			const ScopedRenderCPUProfile timing("Interface3D-main-before-upscale", renderer->name.c_str());
			ModelMain::func(renderer, a_target);
		}
		renderedModels[renderedModelCount++] = renderer;
		RE::BSAutoWriteLock quadsLock(renderer->cachedQuadsLock);
		renderer->colorFXInfos.clear();
		renderer->backgroundFXInfos.clear();
	}
}

void NativeInterfaceUI::ReleaseResources()
{
	// Called by the interop resize transaction, after outstanding GPU work drains.
	// Do not retain a former device's depth allocation across a resize/recreation.
	nativeDepth.Restore();
	nativeDepth = {};
	modelFrame = UINT32_MAX;
	renderedModelCount = 0;
}

void NativeInterfaceUI::InstallHooks()
{
	const auto isOG = REX::FModule::IsRuntimeOG();
	const auto create = stl::detour_thunk_gateway<CreateTarget>(REL::ID{ 43433, 2277176 }, isOG ? 5 : 6, "Interface3D native target allocation");
	const auto color = stl::detour_thunk_gateway<SetColor>(REL::ID{ 1502425, 2277188 }, isOG ? 6 : 5, "Interface3D color target tracking");
	const auto depth = InstallDepthHook();
	// Hook the worker, not RenderPostAA's MOV DL,1 / relative tail jump.
	const auto render = stl::detour_thunk_gateway<RenderAll>(REL::ID{ 1030129, 2222565 }, 8, "Interface3D native post-AA UI");
	enabled = create && color && depth && render;
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
