#include "Upscaling.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <SimpleIni.h>

#include "DX12SwapChain.h"

extern bool enbLoaded;

/** @brief Hook for updating jitter, dynamic resolution, and resources */
struct BSGraphics_State_UpdateDynamicResolution
{
	static void thunk(RE::BSGraphics::RenderTargetManager* This,
		RE::NiPoint3* a2,
		RE::NiPoint3* a3,
		RE::NiPoint3* a4,
		RE::NiPoint3* a5)
	{
		func(This, a2, a3, a4, a5);
		Upscaling::GetSingleton()->UpdateUpscaling();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook to disable TAA when alternative scaling method is active */
struct ImageSpaceEffectTemporalAA_IsActive
{
	static bool thunk(struct ImageSpaceEffectTemporalAA* This)
	{
		return Upscaling::GetSingleton()->upscaleMethod == Upscaling::UpscaleMethod::kDisabled && func(This);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Keep workbench item 3D in the post-upscale Interface3D/UI phase */
struct Interface3D_Renderer_Create
{
	static RE::Interface3D::Renderer* thunk(
		const RE::BSFixedString& a_name,
		RE::UI_DEPTH_PRIORITY a_depth,
		float a_fov,
		bool a_alwaysRenderWhenEnabled)
	{
		auto renderer = func(a_name, a_depth, a_fov, a_alwaysRenderWhenEnabled);
		if (renderer &&
			Upscaling::GetSingleton()->upscaleMethod != Upscaling::UpscaleMethod::kDisabled &&
			a_name == "WorkbenchItem3D") {
			renderer->postAA = true;
		}
		return renderer;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

float originalDynamicHeightRatio = 1.0f;
float originalDynamicWidthRatio = 1.0f;

namespace
{
	constexpr uint32_t kDLSSGResumeStableFrames = 2;
	constexpr std::ptrdiff_t kServingThreadStateOffset = 0x68;
	constexpr auto kLoadingScreenPumpInterval = std::chrono::milliseconds(500);

	float* GetGlobalDynamicWidthRatio()
	{
		static REL::Relocation<float*> ratio{
			REL::ID {
				1361955,
				2666748
			}
		};
		return ratio.get();
	}

	bool GetIniBoolAnySection(CSimpleIniA& a_ini, const char* a_key, bool a_default)
	{
		CSimpleIniA::TNamesDepend sections;
		a_ini.GetAllSections(sections);
		for (const auto& section : sections) {
			if (a_ini.GetBoolValue(section.pItem, a_key, a_default) != a_default) {
				return !a_default;
			}
		}

		return a_ini.GetBoolValue(nullptr, a_key, a_default);
	}

	bool ShouldInstallHighFPSPhysicsFixLoadingScreenCompat()
	{
		const bool moduleLoaded = GetModuleHandleW(L"HighFPSPhysicsFix.dll") != nullptr;
		const bool dllExists = GetFileAttributesW(L"Data\\F4SE\\Plugins\\HighFPSPhysicsFix.dll") != INVALID_FILE_ATTRIBUTES;
		if (!moduleLoaded && !dllExists) {
			return false;
		}

		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile("Data\\F4SE\\Plugins\\HighFPSPhysicsFix.ini") < 0) {
			logger::warn("[Upscaling] HighFPSPhysicsFix.dll detected, but HighFPSPhysicsFix.ini could not be read");
			return false;
		}

		const bool enabled = GetIniBoolAnySection(ini, "DisableAnimationOnLoadingScreens", false);
		if (enabled) {
			logger::info("[Upscaling] HighFPSPhysicsFix loading screen animation patch detected; installing safe loading-screen detour");
		}
		return enabled;
	}

	struct JobListManager_ServingThread_DisplayLoadingScreen
	{
		static void thunk(void* a_this)
		{
			if (!a_this) {
				return;
			}

			// HFPF's inner patch turns the original function into a one-shot
			// loading-screen pump. Keep that first pump so Scaleform/UI render
			// state is initialized, then preserve the vanilla wait contract.
			func(a_this);

			auto* state = reinterpret_cast<volatile std::uint32_t*>(
				reinterpret_cast<std::byte*>(a_this) + kServingThreadStateOffset);
			auto nextPump = std::chrono::steady_clock::now() + kLoadingScreenPumpInterval;
			while (*state == 2) {
				const auto now = std::chrono::steady_clock::now();
				if (now >= nextPump) {
					func(a_this);
					nextPump = now + kLoadingScreenPumpInterval;
				} else if (nextPump - now > std::chrono::milliseconds(1)) {
					Sleep(1);
				} else {
					SwitchToThread();
				}
			}
		}

		static inline void (*func)(void*);
	};

	float* GetGlobalDynamicHeightRatio()
	{
		static REL::Relocation<float*> ratio{
			REL::ID {
				681244,
				2666749
			}
		};
		return ratio.get();
	}

	void SetDynamicResolutionRatio(RE::BSGraphics::RenderTargetManager* a_renderTargetManager, float a_widthRatio, float a_heightRatio)
	{
		a_renderTargetManager->dynamicWidthRatio = a_widthRatio;
		a_renderTargetManager->dynamicHeightRatio = a_heightRatio;
		a_renderTargetManager->isDynamicResolutionCurrentlyActivated = a_widthRatio != 1.0f || a_heightRatio != 1.0f;

		*GetGlobalDynamicWidthRatio() = a_widthRatio;
		*GetGlobalDynamicHeightRatio() = a_heightRatio;
	}

	float GetUpscaleRatioFromQualityMode(uint a_qualityMode)
	{
		switch (a_qualityMode) {
		case 0:
			return 1.0f;
		case 1:
			return 1.5f;
		case 2:
			return 1.7f;
		case 3:
			return 2.0f;
		case 4:
			return 3.0f;
		default:
			return 1.5f;
		}
	}

	uint GetEffectiveQualityMode(Upscaling::UpscaleMethod a_upscaleMethod, uint a_qualityMode)
	{
		// ENB does not support this plugin's render-resolution scaling, but native AA
		// still runs at 1.0 scale and can feed frame generation.
		if (enbLoaded && a_qualityMode != 0 &&
			(a_upscaleMethod == Upscaling::UpscaleMethod::kFSR || a_upscaleMethod == Upscaling::UpscaleMethod::kDLSS)) {
			return 0;
		}

		return a_qualityMode;
	}

	float Halton(uint32_t a_index, uint32_t a_base)
	{
		float result = 0.0f;
		float fraction = 1.0f;
		while (a_index > 0) {
			fraction /= static_cast<float>(a_base);
			result += fraction * static_cast<float>(a_index % a_base);
			a_index /= a_base;
		}
		return result;
	}

	uint32_t GetJitterPhaseCount(uint32_t a_renderWidth, uint32_t a_displayWidth)
	{
		if (a_renderWidth == 0 || a_displayWidth == 0) {
			return 1;
		}

		const auto scale = static_cast<float>(a_displayWidth) / static_cast<float>(a_renderWidth);
		return std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(8.0f * scale * scale)));
	}

	void GetJitterOffset(float* a_outX, float* a_outY, uint32_t a_frameCount, uint32_t a_phaseCount)
	{
		if (!a_outX || !a_outY) {
			return;
		}

		a_phaseCount = std::max<uint32_t>(1, a_phaseCount);
		const auto phase = (a_frameCount % a_phaseCount) + 1;
		*a_outX = Halton(phase, 2) - 0.5f;
		*a_outY = Halton(phase, 3) - 0.5f;
	}

	bool SharedTextureMatches(const std::unique_ptr<Texture2D>& a_texture, const D3D11_TEXTURE2D_DESC& a_desc)
	{
		if (!a_texture || !a_texture->resource) {
			return false;
		}

		D3D11_TEXTURE2D_DESC currentDesc{};
		a_texture->resource->GetDesc(&currentDesc);
		return currentDesc.Width == a_desc.Width &&
			currentDesc.Height == a_desc.Height &&
			currentDesc.Format == a_desc.Format &&
			currentDesc.BindFlags == a_desc.BindFlags &&
			currentDesc.MiscFlags == a_desc.MiscFlags;
	}

	void EnsureSharedD3D12Texture(
		D3D11_TEXTURE2D_DESC a_desc,
		std::unique_ptr<Texture2D>& a_texture,
		winrt::com_ptr<ID3D12Resource>& a_d3d12Resource,
		bool a_createUAV)
	{
		a_desc.Usage = D3D11_USAGE_DEFAULT;
		a_desc.CPUAccessFlags = 0;
		a_desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

		if (SharedTextureMatches(a_texture, a_desc) && a_d3d12Resource) {
			return;
		}

		a_d3d12Resource = nullptr;
		a_texture = std::make_unique<Texture2D>(a_desc);

		if (a_createUAV) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = a_desc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			a_texture->CreateUAV(uavDesc);
		}

		winrt::com_ptr<IDXGIResource1> dxgiResource;
		DX::ThrowIfFailed(a_texture->resource->QueryInterface(IID_PPV_ARGS(dxgiResource.put())));

		HANDLE sharedHandle = nullptr;
		DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle));
		DX::ThrowIfFailed(DX12SwapChain::GetSingleton()->GetD3D12Device()->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(a_d3d12Resource.put())));
		CloseHandle(sharedHandle);
	}

	void ClearDLSSGComputeBindings(ID3D11DeviceContext* a_context)
	{
		ID3D11Buffer* nullBuffer = nullptr;
		a_context->CSSetConstantBuffers(0, 1, &nullBuffer);

		ID3D11ShaderResourceView* views[1] = { nullptr };
		a_context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		a_context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		a_context->CSSetShader(shader, nullptr, 0);
	}
}

/** @brief Hook to fix outline thickness in VATs shader*/
struct ImageSpaceEffectVatsTarget_UpdateParams_SetPixelConstant
{
	static void thunk(struct ImageSpaceShaderParam* This, int row, float x, float y, float z, float w)
	{
		func(This, row, x * originalDynamicHeightRatio, y * originalDynamicWidthRatio, z, w);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook to fix dynamic resolution and jitter in post processing shaders */
struct DrawWorld_Imagespace_RenderEffectRange
{
	static void thunk(RE::BSGraphics::RenderTargetManager* This, uint a2, uint a3, uint a4, uint a5)
	{
		auto upscaling = Upscaling::GetSingleton();

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		static auto gameViewport = Util::State_GetSingleton();

		bool requiresOverride = renderTargetManager->dynamicHeightRatio != 1.0 || renderTargetManager->dynamicWidthRatio != 1.0;

		auto originalOffsetX = gameViewport->offsetX;
		auto originalOffsetY = gameViewport->offsetY;

		// Disable removal of jitter in some passes
		if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDisabled){
			gameViewport->offsetX = originalOffsetX;
			gameViewport->offsetY = originalOffsetY;
		}

		originalDynamicHeightRatio = renderTargetManager->dynamicHeightRatio;
		originalDynamicWidthRatio = renderTargetManager->dynamicWidthRatio;

		if (requiresOverride) {

			// HDR shaders
			func(This, 0, 3, 1, 1);
			upscaling->OverrideRenderTargets({1, 4, 29, 16});
			upscaling->OverrideDepth(true);
			SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);

			// LDR shaders
			func(This, 4, 13, 1, 1);
			upscaling->ResetDepth();
			upscaling->ResetRenderTargets({ 1, 2, 4 });

			SetDynamicResolutionRatio(renderTargetManager, originalDynamicWidthRatio, originalDynamicHeightRatio);
		} else {
			func(This, a2, a3, a4, a5);
		}

		gameViewport->offsetX = originalOffsetX;
		gameViewport->offsetY = originalOffsetY;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook to add alternative scaling method */
struct DrawWorld_Imagespace_LateRenderEffectRange
{
	static void thunk(RE::BSGraphics::RenderTargetManager* This, uint a2, uint a3, uint a4, uint a5)
	{
		auto upscaling = Upscaling::GetSingleton();

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();

		originalDynamicHeightRatio = renderTargetManager->dynamicHeightRatio;
		originalDynamicWidthRatio = renderTargetManager->dynamicWidthRatio;

		if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDisabled &&
			(renderTargetManager->dynamicHeightRatio != 1.0f || renderTargetManager->dynamicWidthRatio != 1.0f)) {
			upscaling->OverrideRenderTargets({ static_cast<int>(a4) });
			upscaling->OverrideDepth(true);
			SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);
			func(This, a2, a3, a4, a5);
			upscaling->ResetDepth();
			upscaling->ResetRenderTargets({ static_cast<int>(a5) });
			SetDynamicResolutionRatio(renderTargetManager, originalDynamicWidthRatio, originalDynamicHeightRatio);
			upscaling->Upscale(static_cast<int>(a5));
			return;
		}

		if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDisabled) {
			func(This, a2, a3, a4, a5);
			upscaling->Upscale(static_cast<int>(a5));
			return;
		}

		func(This, a2, a3, a4, a5);
		upscaling->CaptureDLSSGInputs(static_cast<int>(a5));
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook to add alternative scaling method */
struct DrawWorld_Imagespace_SetUseDynamicResolutionViewportAsDefaultViewport
{
	static void thunk(RE::BSGraphics::RenderTargetManager* This, bool a_true)
	{
		func(This, a_true);

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();

		originalDynamicHeightRatio = renderTargetManager->dynamicHeightRatio;
		originalDynamicWidthRatio = renderTargetManager->dynamicWidthRatio;

		SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);

		func(This, false);

		using SetCurrentViewportDefault_t = void (*)(RE::BSGraphics::RenderTargetManager*);
		static REL::Relocation<SetCurrentViewportDefault_t> setCurrentViewportDefault{ REL::ID{ 158420, 2277192 } };
		setCurrentViewportDefault(This);

		static auto rendererData = RE::BSGraphics::GetRendererData();
		static auto gameViewport = Util::State_GetSingleton();
		auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

		gameViewport->frameBufferViewport = {
			0.0f,
			static_cast<float>(gameViewport->screenWidth),
			0.0f,
			static_cast<float>(gameViewport->screenHeight)
		};

		D3D11_VIEWPORT viewport{};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = static_cast<float>(gameViewport->screenWidth);
		viewport.Height = static_cast<float>(gameViewport->screenHeight);
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for deferred pre-pass rendering with sampler state override */
struct DrawWorld_Render_PreUI_DeferredPrePass
{
	static void thunk(struct DrawWorld* This)
	{
		auto upscaling = Upscaling::GetSingleton();
		upscaling->OverrideSamplerStates();
		func(This);
		upscaling->ResetSamplerStates();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for forward rendering pass with sampler state override and reactive mask generation */
struct DrawWorld_Render_PreUI_Forward
{
	static void thunk(struct DrawWorld* This)
	{
		auto upscaling = Upscaling::GetSingleton();

		upscaling->OverrideSamplerStates();
		func(This);
		upscaling->ResetSamplerStates();

		auto fidelityFX = FidelityFX::GetSingleton();

		if (upscaling->upscaleMethod == Upscaling::UpscaleMethod::kFSR)
			fidelityFX->GenerateReactiveMask();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

bool frameGenerationReticleFix = false;

/** @brief Hook forward rendering to capture frame-generation motion/depth inputs */
struct DrawWorld_FrameGenerationForward
{
	static void thunk(void* This)
	{
		func(This);

		if (!frameGenerationReticleFix) {
			Upscaling::GetSingleton()->CopyFrameGenerationBuffers();
		}

		frameGenerationReticleFix = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook reticle rendering to mask reticles out of frame-generation motion/depth inputs */
struct DrawWorld_FrameGenerationReticle
{
	static void thunk(void* This)
	{
		auto upscaling = Upscaling::GetSingleton();
		upscaling->PreFrameGenerationAlpha();
		func(This);
		frameGenerationReticleFix = upscaling->PostFrameGenerationAlpha();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for HBAO to fix dynamic resolution */
struct DrawWorld_Render_PreUI_NVHBAO
{
	static void thunk(struct DrawWorld* This)
	{
		auto upscaling = Upscaling::GetSingleton();

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		bool requiresOverride = renderTargetManager->dynamicHeightRatio != 1.0 || renderTargetManager->dynamicWidthRatio != 1.0;

		originalDynamicHeightRatio = renderTargetManager->dynamicHeightRatio;
		originalDynamicWidthRatio = renderTargetManager->dynamicWidthRatio;

		if (requiresOverride) {
			upscaling->OverrideDepth(true);
			upscaling->OverrideRenderTargets({20});
			SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);
		}

		func(This);

		if (requiresOverride) {
			upscaling->ResetDepth();
			upscaling->ResetRenderTargets({25});
			SetDynamicResolutionRatio(renderTargetManager, originalDynamicWidthRatio, originalDynamicHeightRatio);
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for BSDFComposite with render target and depth override */
struct DrawWorld_DeferredComposite_RenderPassImmediately
{
	static void thunk(RE::BSRenderPass* This, uint a2, bool a3)
	{
		auto upscaling = Upscaling::GetSingleton();

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		bool requiresOverride = renderTargetManager->dynamicHeightRatio != 1.0 || renderTargetManager->dynamicWidthRatio != 1.0;

		originalDynamicHeightRatio = renderTargetManager->dynamicHeightRatio;
		originalDynamicWidthRatio = renderTargetManager->dynamicWidthRatio;

		if (requiresOverride) {
			upscaling->OverrideRenderTargets({20, 25, 57, 24, 23, 58, 59, 3, 9, 60, 61, 28});
			upscaling->OverrideDepth(true);
			SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);
		}

		func(This, a2, a3);

		if (requiresOverride) {
			upscaling->ResetRenderTargets({4});
			upscaling->ResetDepth();
			SetDynamicResolutionRatio(renderTargetManager, originalDynamicWidthRatio, originalDynamicHeightRatio);
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for BSImagespaceShaderLensFlare with depth override */
struct BSImagespaceShaderLensFlare_RenderLensFlare
{
	static void thunk(RE::NiCamera* a_camera)
	{
		auto upscaling = Upscaling::GetSingleton();

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		bool requiresOverride = renderTargetManager->dynamicHeightRatio != 1.0 || renderTargetManager->dynamicWidthRatio != 1.0;

		if (requiresOverride)
			upscaling->OverrideDepth(true);

		func(a_camera);

		if (requiresOverride)
			upscaling->ResetDepth();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for BSImagespaceShaderSSLRRaytracing with replaced shader */
struct BSImagespaceShaderSSLRRaytracing_SetupTechnique_BeginTechnique
{
	static void thunk(RE::BSShader* This, uint a2, uint a3, uint a4, uint a5)
	{
		func(This, a2, a3, a4, a5);
		Upscaling::GetSingleton()->PatchSSRShader();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for forward alpha rendering with opaque texture copy for reactive mask */
struct ForwardAlphaImpl_FinishAccumulating_Standard_PostResolveDepth
{
	static void thunk(RE::BSShaderAccumulator* This)
	{
		func(This);
		auto upscaling = Upscaling::GetSingleton();
		auto fidelityFX = FidelityFX::GetSingleton();

		if (upscaling->upscaleMethod == Upscaling::UpscaleMethod::kFSR ||
			upscaling->upscaleMethod == Upscaling::UpscaleMethod::kDLSS)
			fidelityFX->CopyOpaqueTexture();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook LoadingMenu to fix jitter scale */
struct LoadingMenu_Render_UpdateTemporalData
{
	static void thunk(RE::BSGraphics::State* This)
	{
		func(This);

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook to restore dynamic resolution settings */
struct DrawWorld_Imagespace
{
	static void thunk(struct DrawWorld* This)
	{
		func(This);

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();

		SetDynamicResolutionRatio(renderTargetManager, originalDynamicWidthRatio, originalDynamicHeightRatio);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void Upscaling::InstallHooks()
{
	// Disable TAA shader if using alternative scaling method
	stl::write_vfunc<0x8, ImageSpaceEffectTemporalAA_IsActive>(RE::VTABLE::ImageSpaceEffectTemporalAA[0]);
	stl::detour_thunk<Interface3D_Renderer_Create>(REL::ID{ 88488, 2222519 });

	const auto isOG = REX::FModule::IsRuntimeOG();

	// Control jitters, dynamic resolution, sampler states, and render targets
	stl::write_thunk_call<BSGraphics_State_UpdateDynamicResolution>(
		REL::ID{ 984743, 2318321 }.address() + (isOG ? 0x14B : 0x29F));

	// Add alternative scaling method
	stl::write_thunk_call<DrawWorld_Imagespace_SetUseDynamicResolutionViewportAsDefaultViewport>(
		REL::ID{ 587723, 2318322 }.address() + (isOG ? 0xE1 : 0xC5));

	// Control sampler states for mipmap bias
	stl::write_thunk_call<DrawWorld_Render_PreUI_DeferredPrePass>(
		REL::ID{ 984743, 2318321 }.address() + (isOG ? 0x17F : 0x2E3));
	stl::write_thunk_call<DrawWorld_Render_PreUI_Forward>(
		REL::ID{ 984743, 2318321 }.address() + (isOG ? 0x1C9 : 0x3A6));

	// Copy opaque texture for FSR reactive mask
	stl::write_thunk_call<ForwardAlphaImpl_FinishAccumulating_Standard_PostResolveDepth>(
		REL::ID{ 338205, 2318315 }.address() + (isOG ? 0x1DC : 0x4C6));

	// Capture reticle-safe motion vectors and depth for frame generation.
	stl::detour_thunk<DrawWorld_FrameGenerationForward>(REL::ID{ 656535, 2318315 });
	stl::write_thunk_call<DrawWorld_FrameGenerationReticle>(
		REL::ID{ 338205, 2318315 }.address() + (isOG ? 0x253 : 0x53D));

	// This hook also owns native-AA evaluation and frame-generation input capture.
	// Keep it installed with ENB; the dynamic-resolution branches stay inactive at 1.0 scale.
	stl::write_thunk_call<DrawWorld_Imagespace_LateRenderEffectRange>(
		REL::ID{ 587723, 2318322 }.address() + (isOG ? 0xD3 : 0xB7));

	if (ShouldInstallHighFPSPhysicsFixLoadingScreenCompat()) {
		stl::detour_thunk<JobListManager_ServingThread_DisplayLoadingScreen>(REL::ID{ 132841, 2227631 });
	}

	// These hooks are not needed when using ENB because dynamic resolution is not supported
	if (!enbLoaded) {
		// Fix dynamic resolution for BSDFComposite
		stl::write_thunk_call<DrawWorld_DeferredComposite_RenderPassImmediately>(
			REL::ID{ 728427, 2318313 }.address() + (isOG ? 0x8DC : 0x915));

		// Fix dynamic resolution for Lens Flare visibility
		stl::detour_thunk<BSImagespaceShaderLensFlare_RenderLensFlare>(REL::ID{ 676108, 2317547 });

		// Fix dynamic resolution for Screenspace Reflections
		stl::write_thunk_call<BSImagespaceShaderSSLRRaytracing_SetupTechnique_BeginTechnique>(
			REL::ID{ 779077, 2317302 }.address() + 0x1C);

		// Fix dynamic resolution for post processing
		stl::write_thunk_call<DrawWorld_Imagespace_RenderEffectRange>(
			REL::ID{ 587723, 2318322 }.address() + (isOG ? 0x9F : 0x83));

		// Fix dynamic resolution for HBAO
		if (isOG) {
			stl::write_thunk_call<DrawWorld_Render_PreUI_NVHBAO>(REL::ID{ 984743 }.address() + 0x1BA);
		}
		
		// Fix VATs line thickness
		stl::write_thunk_call<ImageSpaceEffectVatsTarget_UpdateParams_SetPixelConstant>(
			REL::ID{ 1042583, 2317983 }.address() + (isOG ? 0xBB : 0x110));

		// Fix jitter in LoadingMenu
		stl::write_thunk_call<LoadingMenu_Render_UpdateTemporalData>(
			REL::ID{ 135719, 2249225 }.address() + (isOG ? 0x2BD : 0x275));

		// Fix dynamic resolution after upscaling
		stl::detour_thunk<DrawWorld_Imagespace>(REL::ID{ 587723, 2318322 });
	}
}

struct SamplerStates
{
	ID3D11SamplerState* a[320];

	static SamplerStates* GetSingleton()
	{
		static auto samplerStates = reinterpret_cast<SamplerStates*>(REL::ID{ 44312, 2704455 }.address());
		return samplerStates;
	}
};

void Upscaling::LoadSettings()
{
	const auto previousUpscaleMethodPreference = static_cast<UpscaleMethod>(settings.upscaleMethodPreference);

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile("Data\\MCM\\Settings\\Upscaling.ini");
	
	settings.upscaleMethodPreference = static_cast<uint>(ini.GetLongValue("Settings", "iUpscaleMethodPreference", 2));
	settings.qualityMode = static_cast<uint>(ini.GetLongValue("Settings", "iQualityMode", 1));
	settings.frameGenerationMode = static_cast<uint>(ini.GetLongValue("Settings", "iFrameGenerationMode", 0));
	settings.dlssgGeneratedFrames = static_cast<uint>(ini.GetLongValue("Settings", "iDLSSGGeneratedFrames", 0));
	settings.dynamicMFGEnabled = static_cast<uint>(ini.GetLongValue("Settings", "bDynamicMFGEnabled", 0));
	settings.dynamicMFGTargetFPS = static_cast<uint>(ini.GetLongValue("Settings", "iDynamicMFGTargetFPS", 300));
	settings.reflexMode = static_cast<uint>(ini.GetLongValue("Settings", "iReflexMode", 1));
	settings.dlssModelPreset = static_cast<uint>(std::clamp<long>(ini.GetLongValue("Settings", "iDLSSModelPreset", 0), 0, 4));
	settings.osdMode = static_cast<uint>(std::clamp<long>(ini.GetLongValue("Settings", "iOnScreenDisplay", 0), 0, 2));
	settings.taggedTextureDebug = static_cast<uint>(ini.GetLongValue("Settings", "bTaggedTextureDebug", 0) == 1);
	const auto legacySharpness = ini.GetDoubleValue("Settings", "fRCASSharpness", 0.2);
	settings.sharpness = std::clamp(static_cast<float>(ini.GetDoubleValue("Settings", "fSharpness", legacySharpness)), 0.0f, 1.0f);

	auto streamline = Streamline::GetSingleton();
	const auto currentUpscaleMethodPreference = static_cast<UpscaleMethod>(settings.upscaleMethodPreference);
	const auto switchedBetweenD3D12Upscalers =
		(previousUpscaleMethodPreference == UpscaleMethod::kFSR && currentUpscaleMethodPreference == UpscaleMethod::kDLSS) ||
		(previousUpscaleMethodPreference == UpscaleMethod::kDLSS && currentUpscaleMethodPreference == UpscaleMethod::kFSR);
	const auto switchedToD3D12Upscaler =
		previousUpscaleMethodPreference != currentUpscaleMethodPreference &&
		(currentUpscaleMethodPreference == UpscaleMethod::kFSR || currentUpscaleMethodPreference == UpscaleMethod::kDLSS);

	if (switchedToD3D12Upscaler) {
		if (!DX12SwapChain::GetSingleton()->IsReady()) {
			logger::warn("[Upscaling] Switching to FSR or DLSS requires the D3D12 proxy swapchain, but it is unavailable. Restart the game and check earlier DX12SwapChain startup logs.");
		} else if (currentUpscaleMethodPreference == UpscaleMethod::kDLSS && !streamline->featureDLSS) {
			logger::warn("[Upscaling] DLSS was selected but Streamline DLSS is unavailable; FSR will remain active.");
		} else if (switchedBetweenD3D12Upscalers) {
			logger::info("[Upscaling] Runtime upscaler switch requested: {} -> {}",
				previousUpscaleMethodPreference == UpscaleMethod::kFSR ? "FSR" : "DLSS",
				currentUpscaleMethodPreference == UpscaleMethod::kFSR ? "FSR" : "DLSS");
		}
	}

	if (settings.frameGenerationMode > 0 && streamline->initialized && !streamline->UsesD3D12()) {
		logger::warn("[Upscaling] DLSS-G requires the D3D12 proxy swapchain to be selected at startup. Keep iFrameGenerationMode enabled and restart the game.");
	}
}

void Upscaling::OnDataLoaded()
{
	RE::UI::GetSingleton()->RegisterSink<RE::MenuOpenCloseEvent>(this);
	LoadSettings();
	UpdateGameSettings();
}

RE::BSEventNotifyControl Upscaling::ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	auto singleton = GetSingleton();

	// Reload settings when closing MCM menu
	if (a_event.menuName == "PauseMenu") {
		singleton->pauseMenuOpen = a_event.opening;
		if (!a_event.opening) {
			singleton->LoadSettings();
		}
	}
	if (a_event.menuName == "ScopeMenu") {
		singleton->scopeMenuOpen = a_event.opening;
	}

	return RE::BSEventNotifyControl::kContinue;
}

void Upscaling::UpdateRenderTarget(int index, float a_currentWidthRatio, float a_currentHeightRatio)
{
	// Get the game's renderer and save the original render target
	static auto rendererData = RE::BSGraphics::GetRendererData();
	originalRenderTargets[index] = rendererData->renderTargets[index];

	auto& originalRenderTarget = originalRenderTargets[index];
	auto& proxyRenderTarget = proxyRenderTargets[index];

	// Clean up existing proxy render target resources
	// We manually Release() these because they're game engine structures, not our smart pointers
	if (proxyRenderTarget.uaView)
		proxyRenderTarget.uaView->Release();
	proxyRenderTarget.uaView = nullptr;

	if (proxyRenderTarget.srView)
		proxyRenderTarget.srView->Release();
	proxyRenderTarget.srView = nullptr;

	if (proxyRenderTarget.rtView)
		proxyRenderTarget.rtView->Release();
	proxyRenderTarget.rtView = nullptr;

	if (proxyRenderTarget.texture)
		proxyRenderTarget.texture->Release();
	proxyRenderTarget.texture = nullptr;

	// Do not need to replace render targets at native resolution
	if (a_currentWidthRatio == 1.0f && a_currentHeightRatio == 1.0f)
		return;

	D3D11_TEXTURE2D_DESC textureDesc{};
	if (originalRenderTarget.texture)
		reinterpret_cast<ID3D11Texture2D*>(originalRenderTarget.texture)->GetDesc(&textureDesc);

	D3D11_RENDER_TARGET_VIEW_DESC rtViewDesc{};
	if (originalRenderTarget.rtView)
		reinterpret_cast<ID3D11RenderTargetView*>(originalRenderTarget.rtView)->GetDesc(&rtViewDesc);

	D3D11_SHADER_RESOURCE_VIEW_DESC srViewDesc{};
	if (originalRenderTarget.srView)
		reinterpret_cast<ID3D11ShaderResourceView*>(originalRenderTarget.srView)->GetDesc(&srViewDesc);

	D3D11_UNORDERED_ACCESS_VIEW_DESC uaViewDesc;
	if (originalRenderTarget.uaView)
		reinterpret_cast<ID3D11UnorderedAccessView*>(originalRenderTarget.uaView)->GetDesc(&uaViewDesc);

	// Scale texture dimensions (e.g., 1920x1080 @ 0.67 = 1280x720)
	textureDesc.Width = static_cast<uint>(static_cast<float>(textureDesc.Width) * a_currentWidthRatio);
	textureDesc.Height = static_cast<uint>(static_cast<float>(textureDesc.Height) * a_currentHeightRatio);

	auto device = reinterpret_cast<ID3D11Device*>(rendererData->device);

	if (originalRenderTarget.texture)
		DX::ThrowIfFailed(device->CreateTexture2D(&textureDesc, nullptr, reinterpret_cast<ID3D11Texture2D**>(&proxyRenderTarget.texture)));

	if (auto texture = reinterpret_cast<ID3D11Texture2D*>(proxyRenderTarget.texture)) {
		if (originalRenderTarget.rtView)
			DX::ThrowIfFailed(device->CreateRenderTargetView(texture, &rtViewDesc, reinterpret_cast<ID3D11RenderTargetView**>(&proxyRenderTarget.rtView)));

		if (originalRenderTarget.srView)
			DX::ThrowIfFailed(device->CreateShaderResourceView(texture, &srViewDesc, reinterpret_cast<ID3D11ShaderResourceView**>(&proxyRenderTarget.srView)));

		if (originalRenderTarget.uaView)
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texture, &uaViewDesc, reinterpret_cast<ID3D11UnorderedAccessView**>(&proxyRenderTarget.uaView)));
	}

#ifndef NDEBUG
	if (auto texture = reinterpret_cast<ID3D11Texture2D*>(proxyRenderTarget.texture)) {
		auto name = std::format("RT PROXY {}", index);
		texture->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
	}

	if (auto rtView = reinterpret_cast<ID3D11RenderTargetView*>(proxyRenderTarget.rtView)) {
		auto name = std::format("RTV PROXY {}", index);
		rtView->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
	}

	if (auto srView = reinterpret_cast<ID3D11ShaderResourceView*>(proxyRenderTarget.srView)) {
		auto name = std::format("SRV PROXY {}", index);
		srView->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
	}

	if (auto uaView = reinterpret_cast<ID3D11UnorderedAccessView*>(proxyRenderTarget.uaView)) {
		auto name = std::format("UAV PROXY {}", index);
		uaView->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
	}
#endif
}

void Upscaling::OverrideRenderTarget(int index, bool a_doCopy)
{
	if (!originalRenderTargets[index].texture || !proxyRenderTargets[index].texture)
		return;

	static auto rendererData = RE::BSGraphics::GetRendererData();

	// Replace the game's render target with our scaled proxy version
	rendererData->renderTargets[index] = proxyRenderTargets[index];

	// Optionally perform expensive copy operation
	if (a_doCopy) {
		// Get dimensions of both textures
		D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
		reinterpret_cast<ID3D11Texture2D*>(originalRenderTargets[index].texture)->GetDesc(&srcDesc);
		reinterpret_cast<ID3D11Texture2D*>(proxyRenderTargets[index].texture)->GetDesc(&dstDesc);

		D3D11_BOX srcBox;
		srcBox.left = 0;
		srcBox.top = 0;
		srcBox.front = 0;
		srcBox.right = dstDesc.Width;
		srcBox.bottom = dstDesc.Height;
		srcBox.back = 1;

		auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
		context->CopySubresourceRegion(reinterpret_cast<ID3D11Texture2D*>(proxyRenderTargets[index].texture), 0, 0, 0, 0, reinterpret_cast<ID3D11Texture2D*>(originalRenderTargets[index].texture), 0, &srcBox);
	}
}

void Upscaling::ResetRenderTarget(int index, bool a_doCopy)
{
	if (!originalRenderTargets[index].texture || !proxyRenderTargets[index].texture)
		return;

	static auto rendererData = RE::BSGraphics::GetRendererData();

	// Optionally perform expensive copy operation before swapping back
	if (a_doCopy) {
		D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
		reinterpret_cast<ID3D11Texture2D*>(proxyRenderTargets[index].texture)->GetDesc(&srcDesc);
		reinterpret_cast<ID3D11Texture2D*>(originalRenderTargets[index].texture)->GetDesc(&dstDesc);

		D3D11_BOX srcBox;
		srcBox.left = 0;
		srcBox.top = 0;
		srcBox.front = 0;
		srcBox.right = srcDesc.Width;
		srcBox.bottom = srcDesc.Height;
		srcBox.back = 1;

		auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
		context->CopySubresourceRegion(reinterpret_cast<ID3D11Texture2D*>(originalRenderTargets[index].texture), 0, 0, 0, 0, reinterpret_cast<ID3D11Texture2D*>(proxyRenderTargets[index].texture), 0, &srcBox);
	}

	// Restore the original render target
	rendererData->renderTargets[index] = originalRenderTargets[index];
}

void Upscaling::UpdateRenderTargets(float a_currentWidthRatio, float a_currentHeightRatio)
{
	static auto previousWidthRatio = 0.0f;
	static auto previousHeightRatio = 0.0f;
	static bool previousNeedsUpscalingTexture = false;
	const bool needsUpscalingTexture = upscaleMethodNoMenu != UpscaleMethod::kDisabled;
	const bool suspendedAtNativeForMenu =
		a_currentWidthRatio == 1.0f &&
		a_currentHeightRatio == 1.0f &&
		upscaleMethod == UpscaleMethod::kDisabled &&
		needsUpscalingTexture;

	const bool nativeIdle =
		a_currentWidthRatio == 1.0f &&
		a_currentHeightRatio == 1.0f &&
		!needsUpscalingTexture &&
		!upscalingTexture &&
		!depthOverrideTexture &&
		!dlssOutputTexture &&
		!dilatedMotionVectorTexture;
	if (nativeIdle && !previousNeedsUpscalingTexture) {
		return;
	}

	if (suspendedAtNativeForMenu) {
		return;
	}

	// Check for resolution update
	if (previousWidthRatio == a_currentWidthRatio &&
		previousHeightRatio == a_currentHeightRatio &&
		previousNeedsUpscalingTexture == needsUpscalingTexture) {
		return;
	}

	previousWidthRatio = a_currentWidthRatio;
	previousHeightRatio = a_currentHeightRatio;
	previousNeedsUpscalingTexture = needsUpscalingTexture;

	// Recreate render targets with new dimensions
	for (int i = 0; i < ARRAYSIZE(renderTargetsPatch); i++)
		UpdateRenderTarget(renderTargetsPatch[i], a_currentWidthRatio, a_currentHeightRatio);

	// Reset render-size dependent local textures. Shared D3D11/D3D12 bridge
	// textures are descriptor-checked at capture time and can be reused when
	// menu transitions return to the same dimensions.
	upscalingTexture = nullptr;
	dlssOutputTexture = nullptr;
	dilatedMotionVectorTexture = nullptr;
	depthOverrideTexture = nullptr;
	dlssgHUDLessTexture = nullptr;
	dlssTransparencyMaskReady = false;
	for (std::size_t i = 0; i < dlssgInputsReady.size(); ++i) {
		dlssgInputsReady[i] = false;
		fsrFrameGenerationInputsReady[i] = false;
		fsrD3D12InputsReady[i] = false;
		dlssD3D12InputsReady[i] = false;
		dlssD3D12Sharpened[i] = false;
		dlssD3D12TransparencyMaskReady[i] = false;
		dlssgInputFrameTokenIndices[i] = std::numeric_limits<uint32_t>::max();
		dlssgInputRenderSizes[i] = { 0.0f, 0.0f };
		dlssgInputDisplaySizes[i] = { 0.0f, 0.0f };
		fsrFrameGenerationColorFormats[i] = DXGI_FORMAT_UNKNOWN;
		fsrFrameGenerationFrameIDs[i] = 0;
		fsrInputJitters[i] = { 0.0f, 0.0f };
		fsrInputRenderSizes[i] = { 0.0f, 0.0f };
		fsrInputDisplaySizes[i] = { 0.0f, 0.0f };
	}

	// Match the final frame buffer texture. kMain is still a dynamic-resolution
	// scene target at this point in Fallout 4 and leaves the rest of the frame black.
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto frameBufferSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[(uint)Util::RenderTarget::kFrameBuffer].srView);

	ID3D11Resource* frameBufferResource;
	frameBufferSRV->GetResource(&frameBufferResource);

	D3D11_TEXTURE2D_DESC texDesc{};
	static_cast<ID3D11Texture2D*>(frameBufferResource)->GetDesc(&texDesc);

	frameBufferResource->Release();

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = {
			.MostDetailedMip = 0,
			.MipLevels = 1 }
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = {.MipSlice = 0 }
	};

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	// Intermediate upscaling texture (stores DLSS/FSR output)
	upscalingTexture = std::make_unique<Texture2D>(texDesc);
	upscalingTexture->CreateSRV(srvDesc);
	upscalingTexture->CreateUAV(uavDesc);

	// Do not need to replace render targets at native resolution
	if (a_currentWidthRatio == 1.0f && a_currentHeightRatio == 1.0f)
		return;

	// Dynamic resolution depth texture (R32 float)
	texDesc.Width = static_cast<uint>(static_cast<float>(texDesc.Width) * a_currentWidthRatio);
	texDesc.Height = static_cast<uint>(static_cast<float>(texDesc.Height) * a_currentHeightRatio);

	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	depthOverrideTexture = std::make_unique<Texture2D>(texDesc);
	depthOverrideTexture->CreateSRV(srvDesc);
	depthOverrideTexture->CreateUAV(uavDesc);
}

void Upscaling::OverrideRenderTargets(std::initializer_list<int> a_indicesToCopy)
{
	// Replace all patched render targets with their scaled proxy versions
	for (int i = 0; i < ARRAYSIZE(renderTargetsPatch); i++) {
		int targetIndex = renderTargetsPatch[i];
		bool shouldCopy = std::find(a_indicesToCopy.begin(), a_indicesToCopy.end(), targetIndex) != a_indicesToCopy.end();
		OverrideRenderTarget(targetIndex, shouldCopy);
	}

	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();

	// Update render target metadata to match the scaled resolution.
	// The proxy texture has scaled dimensions, and code paths with dynamic
	// viewport disabled need these dimensions to avoid rendering a native-size
	// viewport into the top-left of the proxy.
	for (int i = 0; i < 100; i++) {
		originalRenderTargetData[i] = renderTargetManager->renderTargetData[i];
		renderTargetManager->renderTargetData[i].width = static_cast<uint>(static_cast<float>(renderTargetManager->renderTargetData[i].width) * renderTargetManager->dynamicWidthRatio);
		renderTargetManager->renderTargetData[i].height = static_cast<uint>(static_cast<float>(renderTargetManager->renderTargetData[i].height) * renderTargetManager->dynamicHeightRatio);
	}

	// Check and override pixel shader SRVs that reference original render targets
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	// Get currently bound pixel shader SRVs (first 16 slots)
	ID3D11ShaderResourceView* boundSRVs[16] = {};
	context->PSGetShaderResources(0, 16, boundSRVs);

	// Scan through bound SRVs and replace any that match original render targets
	for (int srvSlot = 0; srvSlot < 16; srvSlot++) {
		if (!boundSRVs[srvSlot])
			continue;

		// Check if this SRV matches any original render target
		for (int rtIndex = 0; rtIndex < ARRAYSIZE(renderTargetsPatch); rtIndex++) {
			int targetIndex = renderTargetsPatch[rtIndex];
			auto& originalRT = originalRenderTargets[targetIndex];
			auto& proxyRT = proxyRenderTargets[targetIndex];

			// If the bound SRV matches an original render target SRV and we have a proxy
			if (boundSRVs[srvSlot] == reinterpret_cast<ID3D11ShaderResourceView*>(originalRT.srView) && proxyRT.srView) {
				// Replace with the proxy SRV
				auto proxySRV = reinterpret_cast<ID3D11ShaderResourceView*>(proxyRT.srView);
				context->PSSetShaderResources(srvSlot, 1, &proxySRV);
				break;
			}
		}

		// Release the reference from PSGetShaderResources
		boundSRVs[srvSlot]->Release();
	}

	// Temporarily disable dynamic resolution
	DrawWorld_Imagespace_SetUseDynamicResolutionViewportAsDefaultViewport::func(renderTargetManager, false);
}

void Upscaling::ResetRenderTargets(std::initializer_list<int> a_indicesToCopy)
{
	// Restore all original full-resolution render targets
	for (int i = 0; i < ARRAYSIZE(renderTargetsPatch); i++) {
		int targetIndex = renderTargetsPatch[i];
		// If indices array is empty, copy all. Otherwise, only copy if in the array
		bool shouldCopy = a_indicesToCopy.size() == 0 ||
			std::find(a_indicesToCopy.begin(), a_indicesToCopy.end(), targetIndex) != a_indicesToCopy.end();
		ResetRenderTarget(targetIndex, shouldCopy);
	}

	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();

	// Restore original render target metadata (full-resolution dimensions)
	for (int i = 0; i < 100; i++) {
		renderTargetManager->renderTargetData[i] = originalRenderTargetData[i];
	}

	// Check and restore pixel shader SRVs that reference proxy render targets
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	// Get currently bound pixel shader SRVs (first 16 slots)
	ID3D11ShaderResourceView* boundSRVs[16] = {};
	context->PSGetShaderResources(0, 16, boundSRVs);

	// Scan through bound SRVs and replace any that match proxy render targets
	for (int srvSlot = 0; srvSlot < 16; srvSlot++) {
		if (!boundSRVs[srvSlot])
			continue;

		// Check if this SRV matches any proxy render target
		for (int rtIndex = 0; rtIndex < ARRAYSIZE(renderTargetsPatch); rtIndex++) {
			int targetIndex = renderTargetsPatch[rtIndex];
			auto& originalRT = originalRenderTargets[targetIndex];
			auto& proxyRT = proxyRenderTargets[targetIndex];

			// If the bound SRV matches a proxy render target SRV and we have an original
			if (boundSRVs[srvSlot] == reinterpret_cast<ID3D11ShaderResourceView*>(proxyRT.srView) && originalRT.srView) {
				// Replace with the original SRV
				auto originalSRV = reinterpret_cast<ID3D11ShaderResourceView*>(originalRT.srView);
				context->PSSetShaderResources(srvSlot, 1, &originalSRV);
				break;
			}
		}

		// Release the reference from PSGetShaderResources
		boundSRVs[srvSlot]->Release();
	}

	// Enable dynamic resolution again
	DrawWorld_Imagespace_SetUseDynamicResolutionViewportAsDefaultViewport::func(renderTargetManager, true);
}

void Upscaling::OverrideDepth(bool a_doCopy)
{
	static auto rendererData = RE::BSGraphics::GetRendererData();

	// Save the original depth SRV (with dynamic resolution)
	originalDepthView = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth);

	// Optionally perform expensive copy operation
	if (a_doCopy) {
		static auto gameViewport = Util::State_GetSingleton();

		// Only copy depth once per frame
		static auto previousFrame = gameViewport->frameCount;
		if (previousFrame != gameViewport->frameCount)
			CopyDepth();
		previousFrame = gameViewport->frameCount;
	}

	// Replace with our dynamic resolution depth texture for post-processing effects
	rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(depthOverrideTexture->srv.get());
}

void Upscaling::ResetDepth()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();

	// Restore the original depth SRV with dynamic resolution
	rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(originalDepthView);
}

void Upscaling::UpdateSamplerStates(float a_currentMipBias)
{
	static float previousMipBias = 1.0f;

	if (upscaleMethodNoMenu == UpscaleMethod::kDisabled) {
		previousMipBias = std::numeric_limits<float>::quiet_NaN();
		return;
	}

	static auto samplerStates = SamplerStates::GetSingleton();
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto device = reinterpret_cast<ID3D11Device*>(rendererData->device);

	// Check if mip bias has changed - only recreate sampler states if needed
	if (previousMipBias == a_currentMipBias)
		return;

	previousMipBias = a_currentMipBias;

	// Store original sampler states from the game. These are restored after the
	// scoped render passes that use the biased states.
	for (int a = 0; a < 320; a++)
		originalSamplerStates[a] = samplerStates->a[a];

	// Create new sampler states with negative LOD bias
	for (int a = 0; a < 320; a++) {
		// Release existing biased sampler state
		if (biasedSamplerStates[a]){
			biasedSamplerStates[a]->Release();
			biasedSamplerStates[a] = nullptr;
		}

		// Create modified version with LOD bias applied
		if (auto samplerState = originalSamplerStates[a]) {
			D3D11_SAMPLER_DESC samplerDesc;
			samplerState->GetDesc(&samplerDesc);

			// Only modify 16x anisotropic samplers (the high-quality ones)
			if (samplerDesc.Filter == D3D11_FILTER_ANISOTROPIC) {
				samplerDesc.MaxAnisotropy = 8; // Reduced from 16x to 8x for performance
				samplerDesc.MipLODBias = a_currentMipBias;
			}

			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &biasedSamplerStates[a]));
		}
	}
}

void Upscaling::OverrideSamplerStates()
{
	if (upscaleMethod == UpscaleMethod::kDisabled)
		return;

	static auto samplerStates = SamplerStates::GetSingleton();
	for (int a = 0; a < 320; a++)
		samplerStates->a[a] = biasedSamplerStates[a];
}

void Upscaling::ResetSamplerStates()
{
	if (upscaleMethod == UpscaleMethod::kDisabled)
		return;

	static auto samplerStates = SamplerStates::GetSingleton();
	for (int a = 0; a < 320; a++)
		samplerStates->a[a] = originalSamplerStates[a];
}

void Upscaling::CopyDepth()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	// Unbind all render targets before we start manipulating textures
	// This ensures we don't have any resource hazards during the copy
	context->OMSetRenderTargets(0, nullptr, nullptr);

	static auto gameViewport = Util::State_GetSingleton();
	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();

	// Calculate both display (screen) and render (scaled) resolutions
	auto screenSize = float2(float(gameViewport->screenWidth), float(gameViewport->screenHeight));
	auto renderSize = float2(screenSize.x * renderTargetManager->dynamicWidthRatio, screenSize.y * renderTargetManager->dynamicHeightRatio);

	// Get the scaled depth buffer as input
	auto depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth);

	// Get the dynamic resolution depth output UAV
	auto depthUAV = depthOverrideTexture->uav.get();

	// Also update the linearized depth used by other effects
	auto linearDepthUAV = reinterpret_cast<ID3D11UnorderedAccessView*>(rendererData->renderTargets[(uint)Util::RenderTarget::kMainDepthMips].uaView);

	{
		UpdateAndBindUpscalingCB(context, screenSize, renderSize);

		{
			// Bind scaled depth as input (SRV)
			ID3D11ShaderResourceView* views[] = { depthSRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			// Bind full-resolution depth outputs (UAV)
			ID3D11UnorderedAccessView* uavs[] = { linearDepthUAV };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			// Run depth upscaling compute shader
			context->CSSetShader(GetOverrideLinearDepthCS(), nullptr, 0);

			// Dispatch with 8x8 thread groups covering the full screen resolution
			uint dispatchX = (uint)std::ceil(screenSize.x / 8.0f);
			uint dispatchY = (uint)std::ceil(screenSize.y / 8.0f);
			context->Dispatch(dispatchX, dispatchY, 1);
		}

		{
			// Bind scaled depth as input (SRV)
			ID3D11ShaderResourceView* views[] = { depthSRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			// Bind full-resolution depth outputs (UAV)
			ID3D11UnorderedAccessView* uavs[] = { depthUAV };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			// Run depth upscaling compute shader
			context->CSSetShader(GetOverrideDepthCS(), nullptr, 0);

			// Dispatch with 8x8 thread groups covering the render size
			uint dispatchX = (uint)std::ceil(renderSize.x / 8.0f);
			uint dispatchY = (uint)std::ceil(renderSize.y / 8.0f);
			context->Dispatch(dispatchX, dispatchY, 1);
		}

		// Clean up compute shader bindings to avoid resource hazards
		ID3D11ShaderResourceView* views[1] = { nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);
	}
}

bool Upscaling::WantsFrameGenerationInputs()
{
	return frameGenerationInputsWanted;
}

bool Upscaling::EnsureFrameGenerationPatchResources(float2 a_renderSize, DXGI_FORMAT a_colorResourceFormat, DXGI_FORMAT a_colorSRVFormat, DXGI_FORMAT a_motionVectorFormat)
{
	const auto width = static_cast<UINT>(a_renderSize.x);
	const auto height = static_cast<UINT>(a_renderSize.y);
	if (width == 0 || height == 0 || a_colorResourceFormat == DXGI_FORMAT_UNKNOWN || a_colorSRVFormat == DXGI_FORMAT_UNKNOWN || a_motionVectorFormat == DXGI_FORMAT_UNKNOWN) {
		return false;
	}

	auto matches = [width, height](const std::unique_ptr<Texture2D>& a_texture, DXGI_FORMAT a_format) {
		if (!a_texture || !a_texture->resource) {
			return false;
		}

		D3D11_TEXTURE2D_DESC desc{};
		a_texture->resource->GetDesc(&desc);
		return desc.Width == width && desc.Height == height && desc.Format == a_format;
	};

	if (!matches(frameGenerationPreAlphaTexture, a_colorResourceFormat)) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_colorResourceFormat;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = a_colorSRVFormat;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		frameGenerationPreAlphaTexture = std::make_unique<Texture2D>(desc);
		frameGenerationPreAlphaTexture->CreateSRV(srvDesc);
	}

	if (!matches(frameGenerationMotionVectorTexture, a_motionVectorFormat)) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_motionVectorFormat;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		frameGenerationMotionVectorTexture = std::make_unique<Texture2D>(desc);
		frameGenerationMotionVectorTexture->CreateSRV(srvDesc);
		frameGenerationMotionVectorTexture->CreateUAV(uavDesc);
	}

	if (!matches(frameGenerationDepthTexture, DXGI_FORMAT_R32_FLOAT)) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		frameGenerationDepthTexture = std::make_unique<Texture2D>(desc);
		frameGenerationDepthTexture->CreateSRV(srvDesc);
		frameGenerationDepthTexture->CreateUAV(uavDesc);
	}

	auto ensureReticleMask = [&](std::unique_ptr<Texture2D>& a_texture) {
		if (matches(a_texture, DXGI_FORMAT_R32_FLOAT)) {
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		a_texture = std::make_unique<Texture2D>(desc);
		a_texture->CreateSRV(srvDesc);
		a_texture->CreateUAV(uavDesc);
	};

	ensureReticleMask(dlssTransparencyMaskTexture);

	return frameGenerationPreAlphaTexture &&
		frameGenerationMotionVectorTexture &&
		frameGenerationDepthTexture &&
		dlssTransparencyMaskTexture &&
		frameGenerationPreAlphaTexture->srv &&
		frameGenerationMotionVectorTexture->srv &&
		frameGenerationMotionVectorTexture->uav &&
		frameGenerationDepthTexture->srv &&
		frameGenerationDepthTexture->uav &&
		dlssTransparencyMaskTexture->srv &&
		dlssTransparencyMaskTexture->uav;
}

void Upscaling::PreFrameGenerationAlpha()
{
	static auto gameViewport = Util::State_GetSingleton();
	frameGenerationBuffersReady = false;
	frameGenerationPreAlphaReady = false;
	dlssTransparencyMaskReady = false;
	if (!WantsFrameGenerationInputs()) {
		return;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto& colorPostAlpha = rendererData->renderTargets[(uint)Util::RenderTarget::kMainTemp];
	auto& motionVector = rendererData->renderTargets[(uint)Util::RenderTarget::kMotionVectors];
	if (!colorPostAlpha.texture || !colorPostAlpha.srView || !motionVector.texture) {
		return;
	}

	D3D11_TEXTURE2D_DESC colorDesc{};
	reinterpret_cast<ID3D11Texture2D*>(colorPostAlpha.texture)->GetDesc(&colorDesc);
	D3D11_SHADER_RESOURCE_VIEW_DESC colorSRVDesc{};
	reinterpret_cast<ID3D11ShaderResourceView*>(colorPostAlpha.srView)->GetDesc(&colorSRVDesc);
	D3D11_TEXTURE2D_DESC motionVectorDesc{};
	reinterpret_cast<ID3D11Texture2D*>(motionVector.texture)->GetDesc(&motionVectorDesc);

	const auto renderSize = float2(
		static_cast<float>(std::min(colorDesc.Width, motionVectorDesc.Width)),
		static_cast<float>(std::min(colorDesc.Height, motionVectorDesc.Height)));
	if (!EnsureFrameGenerationPatchResources(renderSize, colorDesc.Format, colorSRVDesc.Format, motionVectorDesc.Format)) {
		return;
	}

	const D3D11_BOX sourceBox{ 0, 0, 0, static_cast<UINT>(renderSize.x), static_cast<UINT>(renderSize.y), 1 };
	context->CopySubresourceRegion(
		frameGenerationPreAlphaTexture->resource.get(),
		0,
		0,
		0,
		0,
		reinterpret_cast<ID3D11Texture2D*>(colorPostAlpha.texture),
		0,
		&sourceBox);
	frameGenerationPreAlphaFrame = gameViewport->frameCount;
	frameGenerationPreAlphaReady = true;
}

bool Upscaling::PostFrameGenerationAlpha()
{
	dlssTransparencyMaskReady = false;
	static auto gameViewport = Util::State_GetSingleton();
	if (!WantsFrameGenerationInputs() ||
		!frameGenerationPreAlphaTexture ||
		!frameGenerationPreAlphaReady ||
		frameGenerationPreAlphaFrame != gameViewport->frameCount) {
		return false;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto& colorPostAlpha = rendererData->renderTargets[(uint)Util::RenderTarget::kMainTemp];
	auto& motionVector = rendererData->renderTargets[(uint)Util::RenderTarget::kMotionVectors];
	auto& depth = rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain];

	if (!colorPostAlpha.srView) {
		return false;
	}

	context->OMSetRenderTargets(0, nullptr, nullptr);
	bool frameGenerationPatched = false;

	if (motionVector.srView && depth.srViewDepth && frameGenerationMotionVectorTexture && frameGenerationDepthTexture) {
		auto shader = GetGenerateFrameGenerationBuffersCS();
		if (shader) {
			ID3D11ShaderResourceView* views[] = {
				frameGenerationPreAlphaTexture->srv.get(),
				reinterpret_cast<ID3D11ShaderResourceView*>(colorPostAlpha.srView),
				reinterpret_cast<ID3D11ShaderResourceView*>(motionVector.srView),
				reinterpret_cast<ID3D11ShaderResourceView*>(depth.srViewDepth)
			};
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[] = {
				frameGenerationMotionVectorTexture->uav.get(),
				frameGenerationDepthTexture->uav.get()
			};
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
			context->CSSetShader(shader, nullptr, 0);

			D3D11_TEXTURE2D_DESC desc{};
			frameGenerationMotionVectorTexture->resource->GetDesc(&desc);
			context->Dispatch(static_cast<UINT>(std::ceil(desc.Width / 8.0f)), static_cast<UINT>(std::ceil(desc.Height / 8.0f)), 1);

			ID3D11ShaderResourceView* nullViews[4] = {};
			context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
			ID3D11UnorderedAccessView* nullUavs[2] = {};
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
			ID3D11ComputeShader* nullShader = nullptr;
			context->CSSetShader(nullShader, nullptr, 0);

			frameGenerationBuffersFrame = gameViewport->frameCount;
			frameGenerationBuffersReady = true;
			frameGenerationPatched = true;
		}
	}

	return frameGenerationPatched;
}

void Upscaling::CopyFrameGenerationBuffers()
{
	frameGenerationBuffersReady = false;
	frameGenerationPreAlphaReady = false;
	dlssTransparencyMaskReady = false;
	if (!WantsFrameGenerationInputs()) {
		return;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto& colorPostAlpha = rendererData->renderTargets[(uint)Util::RenderTarget::kMainTemp];
	auto& motionVector = rendererData->renderTargets[(uint)Util::RenderTarget::kMotionVectors];
	auto& depth = rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain];
	if (!colorPostAlpha.texture || !colorPostAlpha.srView || !motionVector.texture || !motionVector.srView || !depth.srViewDepth) {
		return;
	}

	D3D11_TEXTURE2D_DESC colorDesc{};
	reinterpret_cast<ID3D11Texture2D*>(colorPostAlpha.texture)->GetDesc(&colorDesc);
	D3D11_SHADER_RESOURCE_VIEW_DESC colorSRVDesc{};
	reinterpret_cast<ID3D11ShaderResourceView*>(colorPostAlpha.srView)->GetDesc(&colorSRVDesc);
	D3D11_TEXTURE2D_DESC motionVectorDesc{};
	reinterpret_cast<ID3D11Texture2D*>(motionVector.texture)->GetDesc(&motionVectorDesc);

	const auto renderSize = float2(
		static_cast<float>(std::min(colorDesc.Width, motionVectorDesc.Width)),
		static_cast<float>(std::min(colorDesc.Height, motionVectorDesc.Height)));
	if (!EnsureFrameGenerationPatchResources(renderSize, colorDesc.Format, colorSRVDesc.Format, motionVectorDesc.Format)) {
		return;
	}

	context->OMSetRenderTargets(0, nullptr, nullptr);
	const D3D11_BOX sourceBox{ 0, 0, 0, static_cast<UINT>(renderSize.x), static_cast<UINT>(renderSize.y), 1 };
	context->CopySubresourceRegion(
		frameGenerationMotionVectorTexture->resource.get(),
		0,
		0,
		0,
		0,
		reinterpret_cast<ID3D11Texture2D*>(motionVector.texture),
		0,
		&sourceBox);

	auto shader = GetCopyDepthToFrameGenerationCS();
	if (!shader) {
		return;
	}

	ID3D11ShaderResourceView* views[] = { reinterpret_cast<ID3D11ShaderResourceView*>(depth.srViewDepth) };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);
	ID3D11UnorderedAccessView* uavs[] = { frameGenerationDepthTexture->uav.get() };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch(static_cast<UINT>(std::ceil(renderSize.x / 8.0f)), static_cast<UINT>(std::ceil(renderSize.y / 8.0f)), 1);

	ID3D11ShaderResourceView* nullViews[1] = {};
	context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
	ID3D11UnorderedAccessView* nullUavs[1] = {};
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
	ID3D11ComputeShader* nullShader = nullptr;
	context->CSSetShader(nullShader, nullptr, 0);

	static auto gameViewport = Util::State_GetSingleton();
	frameGenerationBuffersFrame = gameViewport->frameCount;
	frameGenerationBuffersReady = true;
}

bool Upscaling::ShouldBlockTemporalFeatures() const
{
	const auto main = RE::Main::GetSingleton();
	if (main && (!main->gameActive || main->inMenuMode)) {
		return true;
	}

	if (const auto ui = RE::UI::GetSingleton()) {
		if (ui->freezeFramePause > 0) {
			return true;
		}

		if (!main) {
			RE::BSAutoReadLock lock{ RE::UI::GetMenuMapRWLock() };
			for (const auto& menu : ui->menuStack) {
				if (menu && menu->OnStack() && menu->menuFlags.all(RE::UI_MENU_FLAGS::kPausesGame)) {
					return true;
				}
			}
		}
	}

	return false;
}

bool Upscaling::ShouldBlockUpscaling() const
{
	return ShouldBlockTemporalFeatures();
}

bool Upscaling::ShouldBlockFrameGeneration() const
{
	return ShouldBlockTemporalFeatures() || !dlssgMenuResumeReady;
}

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod(bool a_checkMenu)
{
	auto streamline = Streamline::GetSingleton();

	if (a_checkMenu && ShouldBlockUpscaling()) {
		return UpscaleMethod::kDisabled;
	}

	UpscaleMethod currentUpscaleMethod = (UpscaleMethod)settings.upscaleMethodPreference;
		
	// If DLSS is not available, default to FSR
	if (!streamline->featureDLSS && currentUpscaleMethod == UpscaleMethod::kDLSS)
		currentUpscaleMethod = UpscaleMethod::kFSR;

	return currentUpscaleMethod;
}

bool Upscaling::ShouldUseFrameGeneration(bool a_checkMenu)
{
	if (a_checkMenu) {
		return frameGenerationActive;
	}

	auto streamline = Streamline::GetSingleton();

	if (ShouldUseFSRFrameGeneration(a_checkMenu)) {
		return false;
	}

	if ((settings.frameGenerationMode == 0 && settings.dynamicMFGEnabled == 0) || !streamline->featureDLSSG) {
		return false;
	}

	if (a_checkMenu && ShouldBlockFrameGeneration()) {
		return false;
	}

	return true;
}

bool Upscaling::ShouldUseFSRFrameGeneration(bool a_checkMenu)
{
	if (a_checkMenu) {
		return fsrFrameGenerationActive;
	}

	auto streamline = Streamline::GetSingleton();
	if (static_cast<UpscaleMethod>(settings.upscaleMethodPreference) == UpscaleMethod::kDisabled ||
		(settings.frameGenerationMode == 0 && settings.dynamicMFGEnabled == 0) ||
		!DX12SwapChain::GetSingleton()->IsReady()) {
		return false;
	}

	if (!kForceFSRFrameGenerationForTesting && streamline->featureDLSSG) {
		return false;
	}

	if (a_checkMenu && ShouldBlockFrameGeneration()) {
		return false;
	}

	return true;
}

void Upscaling::CheckResources()
{
	static auto previousUpscaleMethodNoMenu = UpscaleMethod::kDisabled;

	auto streamline = Streamline::GetSingleton();
	auto fidelityFX = FidelityFX::GetSingleton();

	// Detect when upscaling method changes and manage resources accordingly
	if (previousUpscaleMethodNoMenu != upscaleMethodNoMenu) {
		for (std::size_t i = 0; i < dlssD3D12InputsReady.size(); ++i) {
			dlssgInputsReady[i] = false;
			fsrFrameGenerationInputsReady[i] = false;
			fsrD3D12InputsReady[i] = false;
			dlssD3D12InputsReady[i] = false;
		}

		// Clean up resources from the previous upscaling method
		if (previousUpscaleMethodNoMenu == UpscaleMethod::kDisabled)
			CreateUpscalingResources();  // Transitioning from disabled to enabled
		else if (previousUpscaleMethodNoMenu == UpscaleMethod::kFSR)
			fidelityFX->DestroyFSRResources();  // Switching away from FSR
		else if (previousUpscaleMethodNoMenu == UpscaleMethod::kDLSS)
			streamline->DestroyDLSSResources();  // Switching away from DLSS

		// Create resources for the new upscaling method
		if (upscaleMethodNoMenu == UpscaleMethod::kDisabled)
			DestroyUpscalingResources();  // Transitioning to disabled
		else if (upscaleMethodNoMenu == UpscaleMethod::kFSR)
			fidelityFX->CreateFSRResources();  // Switching to FSR
		else if (upscaleMethodNoMenu == UpscaleMethod::kDLSS)
			CreateUpscalingResources();  // Switching to DLSS

		previousUpscaleMethodNoMenu = upscaleMethodNoMenu;
	}
}

ID3D11ComputeShader* Upscaling::GetDilateMotionVectorCS()
{
	if (!dilateMotionVectorCS) {
		logger::debug("Compiling DilateMotionVectorCS.hlsl");
		dilateMotionVectorCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/F4SE/Plugins/Upscaling/DilateMotionVectorCS.hlsl", {}, "cs_5_0"));
	}
	return dilateMotionVectorCS.get();
}

ID3D11ComputeShader* Upscaling::GetOverrideLinearDepthCS()
{
	if (!overrideLinearDepthCS) {
		logger::debug("Compiling OverrideLinearDepthCS.hlsl");
		overrideLinearDepthCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/F4SE/Plugins/Upscaling/OverrideLinearDepthCS.hlsl", {}, "cs_5_0"));
	}
	return overrideLinearDepthCS.get();
}

ID3D11ComputeShader* Upscaling::GetOverrideDepthCS()
{
	if (!overrideDepthCS) {
		logger::debug("Compiling OverrideDepthCS.hlsl");
		overrideDepthCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/F4SE/Plugins/Upscaling/OverrideDepthCS.hlsl", {}, "cs_5_0"));
	}
	return overrideDepthCS.get();
}

ID3D11ComputeShader* Upscaling::GetCopyDepthToFrameGenerationCS()
{
	if (!copyDepthToFrameGenerationCS) {
		logger::debug("Compiling CopyDepthToSharedBufferCS.hlsl");
		copyDepthToFrameGenerationCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/F4SE/Plugins/FrameGeneration/CopyDepthToSharedBufferCS.hlsl", {}, "cs_5_0"));
	}
	return copyDepthToFrameGenerationCS.get();
}

ID3D11ComputeShader* Upscaling::GetGenerateFrameGenerationBuffersCS()
{
	if (!generateFrameGenerationBuffersCS) {
		logger::debug("Compiling GenerateSharedBuffersCS.hlsl");
		generateFrameGenerationBuffersCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/F4SE/Plugins/FrameGeneration/GenerateSharedBuffersCS.hlsl", {}, "cs_5_0"));
	}
	return generateFrameGenerationBuffersCS.get();
}

ID3D11ComputeShader* Upscaling::GetGenerateFrameGenerationUIColorAlphaCS()
{
	if (!generateFrameGenerationUIColorAlphaCS) {
		logger::debug("Compiling GenerateUIColorAlphaCS.hlsl");
		generateFrameGenerationUIColorAlphaCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/F4SE/Plugins/FrameGeneration/GenerateUIColorAlphaCS.hlsl", {}, "cs_5_0"));
	}
	return generateFrameGenerationUIColorAlphaCS.get();
}

ID3D11ComputeShader* Upscaling::GetGenerateDLSSTransparencyMaskCS()
{
	if (!generateDLSSTransparencyMaskCS) {
		logger::debug("Compiling GenerateDLSSTransparencyMaskCS.hlsl");
		generateDLSSTransparencyMaskCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/F4SE/Plugins/Upscaling/GenerateDLSSTransparencyMaskCS.hlsl", {}, "cs_5_0"));
	}
	return generateDLSSTransparencyMaskCS.get();
}

ID3D11PixelShader* Upscaling::GetBSImagespaceShaderSSLRRaytracing()
{
	if (!BSImagespaceShaderSSLRRaytracing) {
		logger::debug("Compiling BSImagespaceShaderSSLRRaytracing.hlsl");
		BSImagespaceShaderSSLRRaytracing.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/F4SE/Plugins/Upscaling/BSImagespaceShaderSSLRRaytracing.hlsl", {}, "ps_5_0"));
	}
	return BSImagespaceShaderSSLRRaytracing.get();
}

ConstantBuffer* Upscaling::GetUpscalingCB()
{
	static std::unique_ptr<ConstantBuffer> upscalingCB = nullptr;

	if (!upscalingCB) {
		logger::debug("Creating UpscalingCB");
		upscalingCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<UpscalingCB>());
	}
	return upscalingCB.get();
}

void Upscaling::UpdateAndBindUpscalingCB(ID3D11DeviceContext* a_context, float2 a_screenSize, float2 a_renderSize)
{
	static auto cameraNear = reinterpret_cast<float*>(REL::ID{ 57985, 2712882 }.address());
	static auto cameraFar = reinterpret_cast<float*>(REL::ID{ 958877, 2712883 }.address());

	float4 cameraData{};
	cameraData.x = *cameraFar;
	cameraData.y = *cameraNear;
	cameraData.z = cameraData.x - cameraData.y;
	cameraData.w = cameraData.x * cameraData.y;

	UpscalingCB upscalingData;
	upscalingData.ScreenSize[0] = static_cast<uint>(a_screenSize.x);
	upscalingData.ScreenSize[1] = static_cast<uint>(a_screenSize.y);
	upscalingData.RenderSize[0] = static_cast<uint>(a_renderSize.x);
	upscalingData.RenderSize[1] = static_cast<uint>(a_renderSize.y);
	upscalingData.CameraData = cameraData;

	auto upscalingCB = GetUpscalingCB();
	upscalingCB->Update(upscalingData);

	auto upscalingBuffer = upscalingCB->CB();
	a_context->CSSetConstantBuffers(0, 1, &upscalingBuffer);
}

void Upscaling::UpdateGameSettings()
{
	static auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();

	// Automatically disable FXAA
	if (imageSpaceManager->effectList[17]->isActive) {
		imageSpaceManager->effectList[17]->isActive = false;
	}

	// Automatically enable TAA
	static auto enableTAA = reinterpret_cast<bool*>(REL::ID{ 460417, 2704658 }.address());
	if (!*enableTAA) {
		*enableTAA = true;
	}
}

void Upscaling::UpdateUpscaling()
{
	static auto gameViewport = Util::State_GetSingleton();
	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();

	auto streamline = Streamline::GetSingleton();
	temporalFeaturesBlocked = ShouldBlockTemporalFeatures();

	upscaleMethodNoMenu = static_cast<UpscaleMethod>(settings.upscaleMethodPreference);
	if (!streamline->featureDLSS && upscaleMethodNoMenu == UpscaleMethod::kDLSS) {
		upscaleMethodNoMenu = UpscaleMethod::kFSR;
	}

	const bool frameGenerationSettingEnabled = settings.frameGenerationMode != 0 || settings.dynamicMFGEnabled != 0;
	const bool upscalerSelected = upscaleMethodNoMenu != UpscaleMethod::kDisabled;
	const bool menuKeepsUpscaling = pauseMenuOpen && temporalFeaturesBlocked && upscalerSelected;
	const bool menuBlocksTemporal = temporalFeaturesBlocked && !menuKeepsUpscaling;

	upscaleMethod = menuBlocksTemporal ? UpscaleMethod::kDisabled : upscaleMethodNoMenu;

	const bool menuBlocksUpscaling = upscalerSelected && menuBlocksTemporal;
	const bool dlssgHeldThroughMenu = menuBlocksUpscaling && frameGenerationSettingEnabled && streamline->featureDLSSG;
	const bool menuSuspendsD3D12DLSS =
		menuBlocksUpscaling &&
		!dlssgHeldThroughMenu &&
		upscaleMethodNoMenu == UpscaleMethod::kDLSS &&
		streamline->UsesD3D12() &&
		streamline->featureDLSS;
	const bool resumeD3D12DLSSFromMenu = d3d12DLSSMenuSuspended && !menuBlocksUpscaling;

	if (menuBlocksUpscaling && !dlssgHeldThroughMenu) {
		dlssgMenuResumeReady = false;
		dlssgStableGameplayFrames = 0;
	} else if (!dlssgMenuResumeReady && (upscaleMethod != UpscaleMethod::kDisabled || dlssgHeldThroughMenu)) {
		dlssgStableGameplayFrames = std::min<uint32_t>(dlssgStableGameplayFrames + 1, kDLSSGResumeStableFrames);
		dlssgMenuResumeReady = dlssgStableGameplayFrames >= kDLSSGResumeStableFrames;
	} else if (upscaleMethod == UpscaleMethod::kDisabled && !dlssgHeldThroughMenu) {
		dlssgStableGameplayFrames = 0;
	}

	if (menuSuspendsD3D12DLSS && !d3d12DLSSMenuSuspended) {
		for (std::size_t i = 0; i < dlssgInputsReady.size(); ++i) {
			dlssD3D12InputsReady[i] = false;
			dlssD3D12Sharpened[i] = false;
			dlssD3D12TransparencyMaskReady[i] = false;
			dlssgInputFrameTokenIndices[i] = std::numeric_limits<uint32_t>::max();
			dlssgInputRenderSizes[i] = { 0.0f, 0.0f };
			dlssgInputDisplaySizes[i] = { 0.0f, 0.0f };
		}
		d3d12DLSSMenuSuspended = true;
	} else if (resumeD3D12DLSSFromMenu) {
		d3d12DLSSMenuSuspended = false;
	}

	const bool dx12Ready = DX12SwapChain::GetSingleton()->IsReady();
	const bool dlssgAllowed = frameGenerationSettingEnabled &&
		streamline->featureDLSSG &&
		(!menuBlocksTemporal || dlssgHeldThroughMenu) &&
		(dlssgHeldThroughMenu || dlssgMenuResumeReady);
	fsrFrameGenerationActive =
		static_cast<UpscaleMethod>(settings.upscaleMethodPreference) != UpscaleMethod::kDisabled &&
		frameGenerationSettingEnabled &&
		dx12Ready &&
		(kForceFSRFrameGenerationForTesting || !streamline->featureDLSSG) &&
		!menuBlocksTemporal &&
		dlssgMenuResumeReady;
	frameGenerationActive =
		!fsrFrameGenerationActive &&
		dlssgAllowed;
	d3d12DLSSActive =
		streamline->UsesD3D12() &&
		upscaleMethod == UpscaleMethod::kDLSS &&
		streamline->featureDLSS;
	frameGenerationInputsWanted =
		dx12Ready &&
		(frameGenerationActive || fsrFrameGenerationActive || d3d12DLSSActive);

	// Menus that render their own scene, like Pip-Boy, disable upscaling and need native render targets.
	// Overlay-only menus keep the gameplay scaler because GetUpscaleMethod(true) remains enabled.
	const auto effectiveQualityMode = GetEffectiveQualityMode(upscaleMethod, settings.qualityMode);
	float resolutionScale = upscaleMethod == UpscaleMethod::kDisabled ? 1.0f : 1.0f / GetUpscaleRatioFromQualityMode(effectiveQualityMode);

	// Calculate mipmap LOD bias
	// Example: 0.67 scale -> log2(0.67) = -0.58
	float currentMipBias = std::log2f(resolutionScale);

	if (upscaleMethodNoMenu == UpscaleMethod::kDLSS || upscaleMethodNoMenu == UpscaleMethod::kFSR)
		currentMipBias -= 1.0f;

	if (!menuBlocksUpscaling) {
		UpdateSamplerStates(currentMipBias);
	}
	UpdateRenderTargets(resolutionScale, resolutionScale);
	UpdateGameSettings();

	auto displayWidth = gameViewport->screenWidth;
	auto displayHeight = gameViewport->screenHeight;
	if (upscalingTexture) {
		D3D11_TEXTURE2D_DESC desc{};
		upscalingTexture->resource->GetDesc(&desc);
		displayWidth = desc.Width;
		displayHeight = desc.Height;
	}

	if (upscaleMethod == UpscaleMethod::kDisabled) {
		jitter = { 0.0f, 0.0f };
		osdRenderSize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
		osdNativeSize = osdRenderSize;
		gameViewport->offsetX = 0.0f;
		gameViewport->offsetY = 0.0f;
	}

	// Apply TAA jitter (shifts projection matrix sub-pixel per frame)
	if (upscaleMethod != UpscaleMethod::kDisabled) {
		auto renderWidth = static_cast<uint>(static_cast<float>(displayWidth) * resolutionScale);
		auto renderHeight = static_cast<uint>(static_cast<float>(displayHeight) * resolutionScale);
		auto phaseCount = GetJitterPhaseCount(renderWidth, displayWidth);
		GetJitterOffset(&jitter.x, &jitter.y, gameViewport->frameCount, phaseCount);

		// Convert to NDC (X negated for DirectX)
		gameViewport->offsetX = 2.0f * -jitter.x / static_cast<float>(renderWidth);
		gameViewport->offsetY = 2.0f * jitter.y / static_cast<float>(renderHeight);
		osdRenderSize = { static_cast<float>(renderWidth), static_cast<float>(renderHeight) };
		osdNativeSize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
	}

	originalDynamicHeightRatio = resolutionScale;
	originalDynamicWidthRatio = resolutionScale;

	SetDynamicResolutionRatio(renderTargetManager, originalDynamicWidthRatio, originalDynamicHeightRatio);

	streamline->UpdateReflex(settings.reflexMode, frameGenerationActive);
	if (!frameGenerationActive) {
		if (streamline->dlssgActive || streamline->HasPendingDLSSGDisable()) {
			streamline->RequestDLSSGDisable();
		}

		for (std::size_t i = 0; i < dlssgInputsReady.size(); ++i) {
			if (dlssgInputsReady[i]) {
				dlssgInputsReady[i] = false;
			}
			if (!fsrFrameGenerationActive && fsrFrameGenerationInputsReady[i]) {
				fsrFrameGenerationInputsReady[i] = false;
			}
		}
	}

	CheckResources();
}

void Upscaling::Upscale(int a_renderTargetIndex)
{
	if (upscaleMethod == UpscaleMethod::kDisabled)
		return;

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	ID3D11RenderTargetView* currentRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
	ID3D11DepthStencilView* currentDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, currentRTVs, &currentDSV);

	// Unbind render targets to avoid resource hazards
	context->OMSetRenderTargets(0, nullptr, nullptr);

	auto frameBufferSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[a_renderTargetIndex].srView);
	if (!frameBufferSRV) {
		logger::warn("[Upscaling] Cannot upscale RT{}: missing SRV", a_renderTargetIndex);
		for (auto* rtv : currentRTVs) {
			if (rtv) {
				rtv->Release();
			}
		}
		if (currentDSV) {
			currentDSV->Release();
		}
		return;
	}

	ID3D11Resource* frameBufferResource;
	frameBufferSRV->GetResource(&frameBufferResource);

	D3D11_TEXTURE2D_DESC frameBufferDesc{};
	static_cast<ID3D11Texture2D*>(frameBufferResource)->GetDesc(&frameBufferDesc);

	// Copy frame buffer to upscaling texture (input for DLSS/FSR)
	context->CopyResource(upscalingTexture->resource.get(), frameBufferResource);

	static auto gameViewport = Util::State_GetSingleton();

	D3D11_TEXTURE2D_DESC upscaleDesc{};
	upscalingTexture->resource->GetDesc(&upscaleDesc);

	auto displaySize = float2(float(upscaleDesc.Width), float(upscaleDesc.Height));
	auto renderSize = float2(displaySize.x * originalDynamicWidthRatio, displaySize.y * originalDynamicHeightRatio);
	osdRenderSize = renderSize;
	osdNativeSize = displaySize;

	for (auto* rtv : currentRTVs) {
		if (rtv) {
			rtv->Release();
		}
	}
	if (currentDSV) {
		currentDSV->Release();
	}

	static uint32_t previousViewportWidth = 0;
	static uint32_t previousViewportHeight = 0;
	static uint32_t previousFrameBufferWidth = 0;
	static uint32_t previousFrameBufferHeight = 0;
	static uint32_t previousUpscaleWidth = 0;
	static uint32_t previousUpscaleHeight = 0;
	if (previousViewportWidth != gameViewport->screenWidth ||
		previousViewportHeight != gameViewport->screenHeight ||
		previousFrameBufferWidth != frameBufferDesc.Width ||
		previousFrameBufferHeight != frameBufferDesc.Height ||
		previousUpscaleWidth != upscaleDesc.Width ||
		previousUpscaleHeight != upscaleDesc.Height) {
		logger::info("[Upscaling] sizes viewport={}x{} framebuffer={}x{} upscale={}x{} render={}x{}",
			gameViewport->screenWidth,
			gameViewport->screenHeight,
			frameBufferDesc.Width,
			frameBufferDesc.Height,
			upscaleDesc.Width,
			upscaleDesc.Height,
			static_cast<uint32_t>(renderSize.x),
			static_cast<uint32_t>(renderSize.y));
		previousViewportWidth = gameViewport->screenWidth;
		previousViewportHeight = gameViewport->screenHeight;
		previousFrameBufferWidth = frameBufferDesc.Width;
		previousFrameBufferHeight = frameBufferDesc.Height;
		previousUpscaleWidth = upscaleDesc.Width;
		previousUpscaleHeight = upscaleDesc.Height;
	}

	// Dilate motion vectors and strip projection jitter before temporal upscalers or DLSS-G consume them.
	if (upscaleMethod == UpscaleMethod::kDLSS || upscaleMethod == UpscaleMethod::kFSR || frameGenerationActive || fsrFrameGenerationActive) {
		if ((upscaleMethod == UpscaleMethod::kDLSS && !dlssOutputTexture) || !dilatedMotionVectorTexture) {
			CreateUpscalingResources();
		}

		if ((upscaleMethod == UpscaleMethod::kDLSS && !dlssOutputTexture) || !dilatedMotionVectorTexture) {
			logger::warn("[Upscaling] motion vector dilation resources unavailable, skipping upscale");
			frameBufferResource->Release();
			return;
		}

		{
			UpdateAndBindUpscalingCB(context, displaySize, renderSize);

			auto motionVectorSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[(uint)Util::RenderTarget::kMotionVectors].srView);
			auto depthTextureSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth);
			const bool usePatchedMotionVectors =
				frameGenerationBuffersReady &&
				frameGenerationBuffersFrame == gameViewport->frameCount &&
				frameGenerationMotionVectorTexture &&
				frameGenerationMotionVectorTexture->srv;
			const bool usePatchedDepth =
				upscaleMethod == UpscaleMethod::kDisabled &&
				usePatchedMotionVectors &&
				frameGenerationDepthTexture &&
				frameGenerationDepthTexture->srv;
			if (usePatchedMotionVectors) {
				motionVectorSRV = frameGenerationMotionVectorTexture->srv.get();
			}
			if (usePatchedDepth) {
				depthTextureSRV = frameGenerationDepthTexture->srv.get();
			}

			ID3D11ShaderResourceView* views[2] = { motionVectorSRV, depthTextureSRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[1] = { dilatedMotionVectorTexture->uav.get() };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(GetDilateMotionVectorCS(), nullptr, 0);

			uint dispatchX = (uint)std::ceil(renderSize.x / 8.0f);
			uint dispatchY = (uint)std::ceil(renderSize.y / 8.0f);
			context->Dispatch(dispatchX, dispatchY, 1);
		}

		// Unbind compute resources
		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);

		ID3D11ShaderResourceView* views[2] = { nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);
	}

	// Execute upscaling
	const bool useD3D12DLSS = d3d12DLSSActive;
	const bool useD3D12FSR = upscaleMethod == UpscaleMethod::kFSR && DX12SwapChain::GetSingleton()->IsReady();
	const bool fsrFrameGenerationSwapChainReady =
		!fsrFrameGenerationActive ||
		DX12SwapChain::GetSingleton()->EnsureFidelityFXFrameGenerationSwapChain();
	bool d3d12FSRInputsReady = false;
	bool presentOverrideUIPrepared = false;
	auto fsrJitter = jitter;
	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	auto prepareD3D12PresentOverrideUI = [&]() {
		if (presentOverrideUIPrepared) {
			return true;
		}
		if (auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(rendererData->renderTargets[a_renderTargetIndex].rtView)) {
			const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			context->ClearRenderTargetView(rtv, clearColor);
		}
		presentOverrideUIPrepared = true;
		return true;
	};
	auto setD3D12PresentOverride = [&](ID3D12Resource* a_finalColor) {
		if (!a_finalColor) {
			return false;
		}
		dx12SwapChain->SetPresentOverride(a_finalColor);
		return true;
	};
	auto setD3D12DLSSPresentFinal = [&](ID3D12Resource* a_finalColor) {
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		if (frameIndex >= dlssD3D12PresentFinal.size()) {
			return;
		}
		dlssD3D12PresentFinal[frameIndex].copy_from(a_finalColor);
	};
	auto getD3D12FSROutput = [&]() -> ID3D12Resource* {
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		if (frameIndex < fsrOutputD3D12.size()) {
			return fsrOutputD3D12[frameIndex].get();
		}
		return nullptr;
	};
	auto getD3D12DLSSOutput = [&]() -> ID3D12Resource* {
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		if (settings.sharpness > 0.0f && frameIndex < dlssSharpenedD3D12.size() && dlssSharpenedD3D12[frameIndex]) {
			return dlssSharpenedD3D12[frameIndex].get();
		}
		if (frameIndex < dlssgHUDLessD3D12.size()) {
			return dlssgHUDLessD3D12[frameIndex].get();
		}
		return nullptr;
	};
	auto copyD3D12FSROutputToD3D11 = [&]() {
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		if (frameIndex < fsrOutputSharedTextures.size() && fsrOutputSharedTextures[frameIndex]) {
			context->CopyResource(frameBufferResource, fsrOutputSharedTextures[frameIndex]->resource.get());
		}
	};
	auto copyD3D12DLSSOutputToD3D11 = [&]() {
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		if (frameIndex < dlssSharpenedSharedTextures.size() && dlssD3D12Sharpened[frameIndex] && dlssSharpenedSharedTextures[frameIndex]) {
			context->CopyResource(frameBufferResource, dlssSharpenedSharedTextures[frameIndex]->resource.get());
		} else if (frameIndex < dlssgHUDLessSharedTextures.size() && dlssgHUDLessSharedTextures[frameIndex]) {
			context->CopyResource(frameBufferResource, dlssgHUDLessSharedTextures[frameIndex]->resource.get());
		}
	};
	if (upscaleMethod == UpscaleMethod::kDLSS && !useD3D12DLSS) {
		const auto effectiveQualityMode = GetEffectiveQualityMode(upscaleMethod, settings.qualityMode);
		Streamline::GetSingleton()->Upscale(upscalingTexture.get(), dlssOutputTexture.get(), dilatedMotionVectorTexture.get(), jitter, renderSize, displaySize, effectiveQualityMode, settings.sharpness, settings.dlssModelPreset);
		context->CopyResource(frameBufferResource, dlssOutputTexture->resource.get());
	}
	else if (upscaleMethod == UpscaleMethod::kFSR && useD3D12FSR) {
		auto motionVectorTexture = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[(uint)Util::RenderTarget::kMotionVectors].texture);
		const bool usePatchedFrameGenerationBuffers =
			upscaleMethod == UpscaleMethod::kDisabled &&
			frameGenerationBuffersReady &&
			frameGenerationBuffersFrame == gameViewport->frameCount &&
			frameGenerationMotionVectorTexture &&
			frameGenerationMotionVectorTexture->resource;
		if (usePatchedFrameGenerationBuffers) {
			motionVectorTexture = frameGenerationMotionVectorTexture->resource.get();
		}
		d3d12FSRInputsReady = CaptureD3D12FSRInputs(a_renderTargetIndex, motionVectorTexture, fsrJitter, renderSize, displaySize);
		if (d3d12FSRInputsReady && !fsrFrameGenerationActive) {
			const auto usePresentOverride = getD3D12FSROutput() != nullptr && !scopeMenuOpen;
			const auto d3d12Result = dx12SwapChain->EvaluateD3D12WorkForCurrentFrame(false, true, false, !usePresentOverride);
			if (d3d12Result.fsr) {
				if (!(usePresentOverride && prepareD3D12PresentOverrideUI() && setD3D12PresentOverride(getD3D12FSROutput()))) {
					copyD3D12FSROutputToD3D11();
				}
			}
		}
	}
	else if (upscaleMethod == UpscaleMethod::kFSR) {
		FidelityFX::GetSingleton()->Upscale(upscalingTexture.get(), fsrJitter, renderSize, 0.0f);
		context->CopyResource(frameBufferResource, upscalingTexture->resource.get());
	}

	if (frameGenerationActive || fsrFrameGenerationActive || useD3D12DLSS) {
		ID3D11Texture2D* motionVectorTexture = nullptr;
		if (dilatedMotionVectorTexture) {
			motionVectorTexture = dilatedMotionVectorTexture->resource.get();
		} else {
			motionVectorTexture = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[(uint)Util::RenderTarget::kMotionVectors].texture);
		}
		const bool usePatchedFrameGenerationBuffers =
			frameGenerationBuffersReady &&
			frameGenerationBuffersFrame == gameViewport->frameCount &&
			frameGenerationMotionVectorTexture &&
			frameGenerationMotionVectorTexture->resource;
		if (usePatchedFrameGenerationBuffers) {
			motionVectorTexture = frameGenerationMotionVectorTexture->resource.get();
		}
		const auto debugFrameIndex = dx12SwapChain->GetFrameIndex();
		if (settings.taggedTextureDebug != 0 && debugFrameIndex < debugMotionVectorSharedTextures.size() && motionVectorTexture) {
			D3D11_TEXTURE2D_DESC debugMotionVectorDesc{};
			motionVectorTexture->GetDesc(&debugMotionVectorDesc);
			debugMotionVectorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			debugMotionVectorDesc.MiscFlags = 0;
			EnsureSharedD3D12Texture(debugMotionVectorDesc, debugMotionVectorSharedTextures[debugFrameIndex], debugMotionVectorD3D12[debugFrameIndex], false);
			context->CopyResource(debugMotionVectorSharedTextures[debugFrameIndex]->resource.get(), motionVectorTexture);
		}

		CaptureDLSSGInputs(a_renderTargetIndex, motionVectorTexture, renderSize, displaySize);
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		const bool dlssPresentOverrideReady =
			useD3D12DLSS &&
			frameIndex < dlssD3D12InputsReady.size() &&
			dlssD3D12InputsReady[frameIndex] &&
			getD3D12DLSSOutput();
		const bool hasPresentOverrideOutput =
			dlssPresentOverrideReady ||
			(d3d12FSRInputsReady && getD3D12FSROutput());
		const auto usePresentOverride = hasPresentOverrideOutput && !scopeMenuOpen;
		const auto d3d12Result = dx12SwapChain->EvaluateD3D12WorkForCurrentFrame(
			useD3D12DLSS,
			d3d12FSRInputsReady && fsrFrameGenerationActive,
			fsrFrameGenerationActive && fsrFrameGenerationSwapChainReady,
			!usePresentOverride);
		if (d3d12Result.fsr) {
			if (!(usePresentOverride && prepareD3D12PresentOverrideUI() && setD3D12PresentOverride(getD3D12FSROutput()))) {
				copyD3D12FSROutputToD3D11();
			}
		}
		if (d3d12Result.dlss) {
			auto* dlssOutput = getD3D12DLSSOutput();
			setD3D12DLSSPresentFinal(dlssOutput);
			if (!(usePresentOverride && prepareD3D12PresentOverrideUI() && setD3D12PresentOverride(dlssOutput))) {
				copyD3D12DLSSOutputToD3D11();
			}
		}
	}

	frameBufferResource->Release();
}

bool Upscaling::CaptureD3D12FSRInputs(int, ID3D11Texture2D* a_motionVectorTexture, float2 a_jitter, float2 a_renderSize, float2 a_displaySize)
{
	if (!upscalingTexture || !DX12SwapChain::GetSingleton()->IsReady()) {
		return false;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	const auto frameIndex = DX12SwapChain::GetSingleton()->GetFrameIndex();
	if (frameIndex >= fsrD3D12InputsReady.size()) {
		return false;
	}
	static auto gameViewport = Util::State_GetSingleton();
	const bool usePatchedFrameGenerationBuffers =
		upscaleMethod == UpscaleMethod::kDisabled &&
		frameGenerationBuffersReady &&
		frameGenerationBuffersFrame == gameViewport->frameCount &&
		frameGenerationDepthTexture &&
		frameGenerationDepthTexture->resource;

	D3D11_TEXTURE2D_DESC inputDesc{};
	upscalingTexture->resource->GetDesc(&inputDesc);
	inputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	inputDesc.MiscFlags = 0;
	EnsureSharedD3D12Texture(inputDesc, fsrInputSharedTextures[frameIndex], fsrInputD3D12[frameIndex], false);
	context->CopyResource(fsrInputSharedTextures[frameIndex]->resource.get(), upscalingTexture->resource.get());

	D3D11_TEXTURE2D_DESC outputDesc = inputDesc;
	outputDesc.Width = static_cast<UINT>(a_displaySize.x);
	outputDesc.Height = static_cast<UINT>(a_displaySize.y);
	outputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	EnsureSharedD3D12Texture(outputDesc, fsrOutputSharedTextures[frameIndex], fsrOutputD3D12[frameIndex], true);

	if (auto* opaqueOnly = FidelityFX::GetSingleton()->colorOpaqueOnlyTexture.get(); opaqueOnly && opaqueOnly->resource) {
		D3D11_TEXTURE2D_DESC opaqueDesc{};
		opaqueOnly->resource->GetDesc(&opaqueDesc);
		opaqueDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		opaqueDesc.MiscFlags = 0;
		EnsureSharedD3D12Texture(opaqueDesc, fsrOpaqueOnlySharedTextures[frameIndex], fsrOpaqueOnlyD3D12[frameIndex], false);
		context->CopyResource(fsrOpaqueOnlySharedTextures[frameIndex]->resource.get(), opaqueOnly->resource.get());
	}

	D3D11_TEXTURE2D_DESC reactiveDesc{};
	reactiveDesc.Width = static_cast<UINT>(a_renderSize.x);
	reactiveDesc.Height = static_cast<UINT>(a_renderSize.y);
	reactiveDesc.MipLevels = 1;
	reactiveDesc.ArraySize = 1;
	reactiveDesc.Format = DXGI_FORMAT_R8_UNORM;
	reactiveDesc.SampleDesc.Count = 1;
	reactiveDesc.Usage = D3D11_USAGE_DEFAULT;
	reactiveDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	EnsureSharedD3D12Texture(reactiveDesc, fsrReactiveMaskSharedTextures[frameIndex], fsrReactiveMaskD3D12[frameIndex], true);

	if (!a_motionVectorTexture) {
		fsrD3D12InputsReady[frameIndex] = false;
		return false;
	}

	D3D11_TEXTURE2D_DESC motionVectorDesc{};
	a_motionVectorTexture->GetDesc(&motionVectorDesc);
	const auto motionVectorInputWidth = std::min<UINT>(motionVectorDesc.Width, static_cast<UINT>(a_renderSize.x));
	const auto motionVectorInputHeight = std::min<UINT>(motionVectorDesc.Height, static_cast<UINT>(a_renderSize.y));
	motionVectorDesc.Width = motionVectorInputWidth;
	motionVectorDesc.Height = motionVectorInputHeight;
	motionVectorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	motionVectorDesc.MiscFlags = 0;
	EnsureSharedD3D12Texture(motionVectorDesc, fsrMotionVectorSharedTextures[frameIndex], fsrMotionVectorD3D12[frameIndex], false);
	if (motionVectorDesc.Width == motionVectorInputWidth && motionVectorDesc.Height == motionVectorInputHeight) {
		const D3D11_BOX sourceBox{ 0, 0, 0, motionVectorInputWidth, motionVectorInputHeight, 1 };
		context->CopySubresourceRegion(fsrMotionVectorSharedTextures[frameIndex]->resource.get(), 0, 0, 0, 0, a_motionVectorTexture, 0, &sourceBox);
	}

	D3D11_TEXTURE2D_DESC depthDesc{};
	depthDesc.Width = static_cast<UINT>(a_renderSize.x);
	depthDesc.Height = static_cast<UINT>(a_renderSize.y);
	depthDesc.MipLevels = 1;
	depthDesc.ArraySize = 1;
	depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Usage = D3D11_USAGE_DEFAULT;
	depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	EnsureSharedD3D12Texture(depthDesc, fsrDepthSharedTextures[frameIndex], fsrDepthD3D12[frameIndex], true);

	auto depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth);
	if (usePatchedFrameGenerationBuffers) {
		D3D11_TEXTURE2D_DESC patchedDepthDesc{};
		frameGenerationDepthTexture->resource->GetDesc(&patchedDepthDesc);
		const D3D11_BOX sourceBox{
			0,
			0,
			0,
			std::min<UINT>(patchedDepthDesc.Width, depthDesc.Width),
			std::min<UINT>(patchedDepthDesc.Height, depthDesc.Height),
			1
		};
		context->CopySubresourceRegion(
			fsrDepthSharedTextures[frameIndex]->resource.get(),
			0,
			0,
			0,
			0,
			frameGenerationDepthTexture->resource.get(),
			0,
			&sourceBox);
	}
	else if (depthSRV && fsrDepthSharedTextures[frameIndex]->uav) {
		UpdateAndBindUpscalingCB(context, a_displaySize, a_renderSize);

		ID3D11ShaderResourceView* views[] = { depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[] = { fsrDepthSharedTextures[frameIndex]->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		context->CSSetShader(GetOverrideDepthCS(), nullptr, 0);

		const auto dispatchX = static_cast<uint>(std::ceil(a_renderSize.x / 8.0f));
		const auto dispatchY = static_cast<uint>(std::ceil(a_renderSize.y / 8.0f));
		context->Dispatch(dispatchX, dispatchY, 1);
		ClearDLSSGComputeBindings(context);
	}

	fsrInputJitters[frameIndex] = a_jitter;
	fsrInputRenderSizes[frameIndex] = a_renderSize;
	fsrInputDisplaySizes[frameIndex] = a_displaySize;
	fsrD3D12InputsReady[frameIndex] =
		fsrInputD3D12[frameIndex] &&
		fsrOutputD3D12[frameIndex] &&
		fsrMotionVectorD3D12[frameIndex] &&
		fsrDepthD3D12[frameIndex];
	return fsrD3D12InputsReady[frameIndex];
}

void Upscaling::CaptureDLSSGInputs(int a_renderTargetIndex, ID3D11Texture2D* a_motionVectorTexture, float2 a_renderSize, float2 a_displaySize)
{
	const bool useD3D12DLSS = d3d12DLSSActive && upscalingTexture;
	const bool useFrameGeneration = frameGenerationActive;
	const bool useFSRFrameGeneration = fsrFrameGenerationActive;
	if (!useFrameGeneration && !useFSRFrameGeneration && !useD3D12DLSS) {
		return;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto streamline = Streamline::GetSingleton();

	auto frameBufferSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[a_renderTargetIndex].srView);
	if (!frameBufferSRV) {
		logger::warn("[Upscaling] Cannot capture DLSS-G HUD-less RT{}: missing SRV", a_renderTargetIndex);
		return;
	}

	ID3D11Resource* frameBufferResource = nullptr;
	frameBufferSRV->GetResource(&frameBufferResource);
	if (!frameBufferResource) {
		return;
	}

	auto frameBufferTexture = static_cast<ID3D11Texture2D*>(frameBufferResource);
	D3D11_TEXTURE2D_DESC frameBufferDesc{};
	frameBufferTexture->GetDesc(&frameBufferDesc);

	if (a_displaySize.x <= 0.0f || a_displaySize.y <= 0.0f) {
		a_displaySize = { static_cast<float>(frameBufferDesc.Width), static_cast<float>(frameBufferDesc.Height) };
	}

	if (a_renderSize.x <= 0.0f || a_renderSize.y <= 0.0f) {
		a_renderSize = { a_displaySize.x * originalDynamicWidthRatio, a_displaySize.y * originalDynamicHeightRatio };
	}

	auto depthTexture = reinterpret_cast<ID3D11Texture2D*>(rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].texture);
	auto motionVectorTexture = a_motionVectorTexture ? a_motionVectorTexture : reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[(uint)Util::RenderTarget::kMotionVectors].texture);
	static auto gameViewport = Util::State_GetSingleton();
	const bool usePatchedFrameGenerationBuffers =
		frameGenerationBuffersReady &&
		frameGenerationBuffersFrame == gameViewport->frameCount &&
		frameGenerationMotionVectorTexture &&
		frameGenerationMotionVectorTexture->resource &&
		frameGenerationDepthTexture &&
		frameGenerationDepthTexture->resource;
	if (usePatchedFrameGenerationBuffers) {
		motionVectorTexture = frameGenerationMotionVectorTexture->resource.get();
	}

	D3D11_TEXTURE2D_DESC motionVectorDesc{};
	if (motionVectorTexture) {
		motionVectorTexture->GetDesc(&motionVectorDesc);
	}

	D3D11_TEXTURE2D_DESC depthDesc{};
	if (depthTexture) {
		depthTexture->GetDesc(&depthDesc);
	}

	context->OMSetRenderTargets(0, nullptr, nullptr);

	if (DX12SwapChain::GetSingleton()->IsReady()) {
		auto dx12SwapChain = DX12SwapChain::GetSingleton();
		if (!motionVectorTexture) {
			frameBufferResource->Release();
			return;
		}

		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		dlssgInputsReady[frameIndex] = false;
		fsrFrameGenerationInputsReady[frameIndex] = false;
		dlssD3D12InputsReady[frameIndex] = false;
		dlssD3D12Sharpened[frameIndex] = false;
		dlssD3D12TransparencyMaskReady[frameIndex] = false;
		dlssD3D12PresentFinal[frameIndex] = nullptr;
		const bool reuseFSRResourcesForFrameGeneration =
			useFSRFrameGeneration &&
			upscaleMethod == UpscaleMethod::kFSR &&
			frameIndex < fsrD3D12InputsReady.size() &&
			fsrD3D12InputsReady[frameIndex] &&
			fsrOutputD3D12[frameIndex] &&
			fsrMotionVectorD3D12[frameIndex] &&
			fsrDepthD3D12[frameIndex];
		if (useD3D12DLSS) {
			D3D11_TEXTURE2D_DESC dlssInputDesc{};
			upscalingTexture->resource->GetDesc(&dlssInputDesc);
			dlssInputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			dlssInputDesc.MiscFlags = 0;
			EnsureSharedD3D12Texture(dlssInputDesc, dlssInputSharedTextures[frameIndex], dlssInputD3D12[frameIndex], false);
			context->CopyResource(dlssInputSharedTextures[frameIndex]->resource.get(), upscalingTexture->resource.get());

			if (settings.sharpness > 0.0f) {
				auto sharpenedDesc = frameBufferDesc;
				sharpenedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				sharpenedDesc.MiscFlags = 0;
				EnsureSharedD3D12Texture(sharpenedDesc, dlssSharpenedSharedTextures[frameIndex], dlssSharpenedD3D12[frameIndex], true);
			}

			dlssTransparencyMaskReady = false;
			dlssD3D12TransparencyMaskReady[frameIndex] = false;
			auto* opaqueOnly = FidelityFX::GetSingleton()->colorOpaqueOnlyTexture.get();
			if (opaqueOnly && opaqueOnly->srv && upscalingTexture->srv) {
				bool recreateTransparencyMask = !dlssTransparencyMaskTexture || !dlssTransparencyMaskTexture->resource;
				if (!recreateTransparencyMask) {
					D3D11_TEXTURE2D_DESC currentMaskDesc{};
					dlssTransparencyMaskTexture->resource->GetDesc(&currentMaskDesc);
					recreateTransparencyMask =
						currentMaskDesc.Width != static_cast<UINT>(a_renderSize.x) ||
						currentMaskDesc.Height != static_cast<UINT>(a_renderSize.y) ||
						currentMaskDesc.Format != DXGI_FORMAT_R32_FLOAT;
				}

				if (recreateTransparencyMask) {
					D3D11_TEXTURE2D_DESC maskDesc{};
					maskDesc.Width = static_cast<UINT>(a_renderSize.x);
					maskDesc.Height = static_cast<UINT>(a_renderSize.y);
					maskDesc.MipLevels = 1;
					maskDesc.ArraySize = 1;
					maskDesc.Format = DXGI_FORMAT_R32_FLOAT;
					maskDesc.SampleDesc.Count = 1;
					maskDesc.Usage = D3D11_USAGE_DEFAULT;
					maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

					D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
					srvDesc.Format = maskDesc.Format;
					srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srvDesc.Texture2D.MipLevels = 1;

					D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
					uavDesc.Format = maskDesc.Format;
					uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

					dlssTransparencyMaskTexture = std::make_unique<Texture2D>(maskDesc);
					dlssTransparencyMaskTexture->CreateSRV(srvDesc);
					dlssTransparencyMaskTexture->CreateUAV(uavDesc);
				}

				auto shader = GetGenerateDLSSTransparencyMaskCS();
				if (shader && dlssTransparencyMaskTexture && dlssTransparencyMaskTexture->uav) {
					ID3D11ShaderResourceView* views[] = {
						opaqueOnly->srv.get(),
						upscalingTexture->srv.get()
					};
					context->CSSetShaderResources(0, ARRAYSIZE(views), views);

					ID3D11UnorderedAccessView* uavs[] = {
						dlssTransparencyMaskTexture->uav.get()
					};
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
					context->CSSetShader(shader, nullptr, 0);

					D3D11_TEXTURE2D_DESC dispatchDesc{};
					dlssTransparencyMaskTexture->resource->GetDesc(&dispatchDesc);
					context->Dispatch(static_cast<UINT>(std::ceil(dispatchDesc.Width / 8.0f)), static_cast<UINT>(std::ceil(dispatchDesc.Height / 8.0f)), 1);
					ClearDLSSGComputeBindings(context);

					dlssTransparencyMaskFrame = gameViewport->frameCount;
					dlssTransparencyMaskReady = true;
				}
			}

			if (dlssTransparencyMaskReady &&
				dlssTransparencyMaskFrame == gameViewport->frameCount &&
				dlssTransparencyMaskTexture &&
				dlssTransparencyMaskTexture->resource) {
				D3D11_TEXTURE2D_DESC maskDesc{};
				dlssTransparencyMaskTexture->resource->GetDesc(&maskDesc);
				maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				maskDesc.MiscFlags = 0;
				EnsureSharedD3D12Texture(maskDesc, dlssTransparencyMaskSharedTextures[frameIndex], dlssTransparencyMaskD3D12[frameIndex], false);
				context->CopyResource(dlssTransparencyMaskSharedTextures[frameIndex]->resource.get(), dlssTransparencyMaskTexture->resource.get());
				dlssD3D12TransparencyMaskReady[frameIndex] = true;
			}
		} else {
			dlssD3D12InputsReady[frameIndex] = false;
			dlssD3D12TransparencyMaskReady[frameIndex] = false;
		}

		D3D11_TEXTURE2D_DESC preAlphaDesc{};
		if (frameGenerationPreAlphaTexture && frameGenerationPreAlphaTexture->resource) {
			frameGenerationPreAlphaTexture->resource->GetDesc(&preAlphaDesc);
		}
		const bool canExtractReticleUI =
			usePatchedFrameGenerationBuffers &&
			frameGenerationPreAlphaReady &&
			frameGenerationPreAlphaFrame == gameViewport->frameCount &&
			frameGenerationPreAlphaTexture &&
			frameGenerationPreAlphaTexture->srv &&
			preAlphaDesc.Width == frameBufferDesc.Width &&
			preAlphaDesc.Height == frameBufferDesc.Height &&
			preAlphaDesc.Format == frameBufferDesc.Format;
		if (reuseFSRResourcesForFrameGeneration && !canExtractReticleUI && !useD3D12DLSS) {
			dlssgHUDLessSharedTextures[frameIndex] = nullptr;
			dlssgHUDLessD3D12[frameIndex].copy_from(fsrOutputD3D12[frameIndex].get());
		} else {
			auto hudlessDesc = frameBufferDesc;
			hudlessDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (useD3D12DLSS) {
				hudlessDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
			}
			hudlessDesc.MiscFlags = 0;
			EnsureSharedD3D12Texture(hudlessDesc, dlssgHUDLessSharedTextures[frameIndex], dlssgHUDLessD3D12[frameIndex], false);
			if (canExtractReticleUI) {
				context->CopyResource(dlssgHUDLessSharedTextures[frameIndex]->resource.get(), frameGenerationPreAlphaTexture->resource.get());
			} else {
				context->CopyResource(dlssgHUDLessSharedTextures[frameIndex]->resource.get(), frameBufferTexture);
			}
		}

		auto sharedMotionVectorDesc = motionVectorDesc;
		const auto motionVectorInputWidth = std::min<UINT>(motionVectorDesc.Width, static_cast<UINT>(a_renderSize.x));
		const auto motionVectorInputHeight = std::min<UINT>(motionVectorDesc.Height, static_cast<UINT>(a_renderSize.y));
		sharedMotionVectorDesc.Width = motionVectorInputWidth;
		sharedMotionVectorDesc.Height = motionVectorInputHeight;
		sharedMotionVectorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		sharedMotionVectorDesc.MiscFlags = 0;
		if (reuseFSRResourcesForFrameGeneration) {
			dlssgMotionVectorSharedTextures[frameIndex] = nullptr;
			dlssgMotionVectorD3D12[frameIndex].copy_from(fsrMotionVectorD3D12[frameIndex].get());
		} else {
			EnsureSharedD3D12Texture(sharedMotionVectorDesc, dlssgMotionVectorSharedTextures[frameIndex], dlssgMotionVectorD3D12[frameIndex], false);
			if (motionVectorDesc.Width == sharedMotionVectorDesc.Width && motionVectorDesc.Height == sharedMotionVectorDesc.Height) {
				context->CopyResource(dlssgMotionVectorSharedTextures[frameIndex]->resource.get(), motionVectorTexture);
			} else {
				const D3D11_BOX sourceBox{ 0, 0, 0, sharedMotionVectorDesc.Width, sharedMotionVectorDesc.Height, 1 };
				context->CopySubresourceRegion(dlssgMotionVectorSharedTextures[frameIndex]->resource.get(), 0, 0, 0, 0, motionVectorTexture, 0, &sourceBox);
			}
		}

		D3D11_TEXTURE2D_DESC sharedDepthDesc{};
		sharedDepthDesc.Width = static_cast<UINT>(a_renderSize.x);
		sharedDepthDesc.Height = static_cast<UINT>(a_renderSize.y);
		sharedDepthDesc.MipLevels = 1;
		sharedDepthDesc.ArraySize = 1;
		sharedDepthDesc.Format = DXGI_FORMAT_R32_FLOAT;
		sharedDepthDesc.SampleDesc.Count = 1;
		sharedDepthDesc.Usage = D3D11_USAGE_DEFAULT;
		sharedDepthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (reuseFSRResourcesForFrameGeneration) {
			dlssgDepthSharedTextures[frameIndex] = nullptr;
			dlssgDepthD3D12[frameIndex].copy_from(fsrDepthD3D12[frameIndex].get());
		} else {
			EnsureSharedD3D12Texture(sharedDepthDesc, dlssgDepthSharedTextures[frameIndex], dlssgDepthD3D12[frameIndex], true);

			auto depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth);
			if (usePatchedFrameGenerationBuffers) {
				D3D11_TEXTURE2D_DESC patchedDepthDesc{};
				frameGenerationDepthTexture->resource->GetDesc(&patchedDepthDesc);
				const D3D11_BOX sourceBox{
					0,
					0,
					0,
					std::min<UINT>(patchedDepthDesc.Width, sharedDepthDesc.Width),
					std::min<UINT>(patchedDepthDesc.Height, sharedDepthDesc.Height),
					1
				};
				context->CopySubresourceRegion(
					dlssgDepthSharedTextures[frameIndex]->resource.get(),
					0,
					0,
					0,
					0,
					frameGenerationDepthTexture->resource.get(),
					0,
					&sourceBox);
			}
			else if (depthSRV && dlssgDepthSharedTextures[frameIndex]->uav) {
				UpdateAndBindUpscalingCB(context, a_displaySize, a_renderSize);

				ID3D11ShaderResourceView* views[] = { depthSRV };
				context->CSSetShaderResources(0, ARRAYSIZE(views), views);

				ID3D11UnorderedAccessView* uavs[] = { dlssgDepthSharedTextures[frameIndex]->uav.get() };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
				context->CSSetShader(GetOverrideDepthCS(), nullptr, 0);

				const auto dispatchX = static_cast<uint>(std::ceil(a_renderSize.x / 8.0f));
				const auto dispatchY = static_cast<uint>(std::ceil(a_renderSize.y / 8.0f));
				context->Dispatch(dispatchX, dispatchY, 1);
				ClearDLSSGComputeBindings(context);
			}
		}

		DXGI_FORMAT uiFormat = DXGI_FORMAT_UNKNOWN;
		if (canExtractReticleUI) {
			auto uiDesc = frameBufferDesc;
			uiDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			uiDesc.MiscFlags = 0;
			EnsureSharedD3D12Texture(uiDesc, dlssgUIColorAlphaSharedTextures[frameIndex], dlssgUIColorAlphaD3D12[frameIndex], true);
			if (dlssgUIColorAlphaSharedTextures[frameIndex] && dlssgUIColorAlphaSharedTextures[frameIndex]->uav) {
				auto shader = GetGenerateFrameGenerationUIColorAlphaCS();
				if (shader) {
					ID3D11ShaderResourceView* views[] = {
						frameGenerationPreAlphaTexture->srv.get(),
						frameBufferSRV
					};
					context->CSSetShaderResources(0, ARRAYSIZE(views), views);

					ID3D11UnorderedAccessView* uavs[] = {
						dlssgUIColorAlphaSharedTextures[frameIndex]->uav.get()
					};
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
					context->CSSetShader(shader, nullptr, 0);
					context->Dispatch(static_cast<UINT>(std::ceil(frameBufferDesc.Width / 8.0f)), static_cast<UINT>(std::ceil(frameBufferDesc.Height / 8.0f)), 1);

					ID3D11ShaderResourceView* nullViews[2] = {};
					context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
					ID3D11UnorderedAccessView* nullUavs[1] = {};
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
					ID3D11ComputeShader* nullShader = nullptr;
					context->CSSetShader(nullShader, nullptr, 0);
					uiFormat = uiDesc.Format;
				} else {
					dlssgUIColorAlphaD3D12[frameIndex] = nullptr;
				}
			}
		} else {
			dlssgUIColorAlphaD3D12[frameIndex] = nullptr;
		}

		if (useFrameGeneration || useD3D12DLSS) {
			streamline->UpdateReflex(settings.reflexMode, useFrameGeneration);
			streamline->UpdateConstants(jitter);
		}
		const auto dlssgInputSize = float2(static_cast<float>(sharedMotionVectorDesc.Width), static_cast<float>(sharedMotionVectorDesc.Height));
		if (useFrameGeneration) {
			streamline->UpdateDLSSG(true, settings.frameGenerationMode, settings.dlssgGeneratedFrames + 1, settings.dynamicMFGEnabled != 0, settings.dynamicMFGTargetFPS, dlssgInputSize, a_displaySize, frameBufferDesc.Format, sharedMotionVectorDesc.Format, sharedDepthDesc.Format, uiFormat);
		}

		static uint64_t fsrFrameGenerationFrameID = 0;
		dlssgInputRenderSizes[frameIndex] = dlssgInputSize;
		dlssgInputDisplaySizes[frameIndex] = a_displaySize;
		dlssgInputFrameTokenIndices[frameIndex] = streamline->GetCurrentFrameTokenIndex();
		dlssgInputsReady[frameIndex] = useFrameGeneration;
		fsrFrameGenerationInputsReady[frameIndex] = useFSRFrameGeneration;
		fsrFrameGenerationColorFormats[frameIndex] = frameBufferDesc.Format;
		fsrFrameGenerationFrameIDs[frameIndex] = fsrFrameGenerationFrameID++;
		dlssD3D12InputsReady[frameIndex] = useD3D12DLSS;
		dlssD3D12ColorFormats[frameIndex] = frameBufferDesc.Format;
		dlssD3D12MotionVectorFormats[frameIndex] = sharedMotionVectorDesc.Format;
		dlssD3D12DepthFormats[frameIndex] = sharedDepthDesc.Format;

		frameBufferResource->Release();
		return;
	}

	bool recreateHUDLess = !dlssgHUDLessTexture;
	if (dlssgHUDLessTexture) {
		D3D11_TEXTURE2D_DESC currentDesc{};
		dlssgHUDLessTexture->resource->GetDesc(&currentDesc);
		recreateHUDLess =
			currentDesc.Width != frameBufferDesc.Width ||
			currentDesc.Height != frameBufferDesc.Height ||
			currentDesc.Format != frameBufferDesc.Format;
	}

	if (recreateHUDLess) {
		auto hudlessDesc = frameBufferDesc;
		hudlessDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		hudlessDesc.CPUAccessFlags = 0;
		hudlessDesc.MiscFlags = 0;
		dlssgHUDLessTexture = std::make_unique<Texture2D>(hudlessDesc);
	}

	context->CopyResource(dlssgHUDLessTexture->resource.get(), frameBufferResource);

	streamline->UpdateReflex(settings.reflexMode, true);
	streamline->UpdateConstants(jitter);
	streamline->UpdateDLSSG(true, settings.frameGenerationMode, settings.dlssgGeneratedFrames + 1, settings.dynamicMFGEnabled != 0, settings.dynamicMFGTargetFPS, a_renderSize, a_displaySize, frameBufferDesc.Format, motionVectorDesc.Format, depthDesc.Format);
	streamline->TagDLSSGResources(dlssgHUDLessTexture->resource.get(), motionVectorTexture, depthTexture, a_renderSize, a_displaySize);

	frameBufferResource->Release();
}

bool Upscaling::EvaluateD3D12DLSS(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex)
{
	if (a_frameIndex >= dlssD3D12InputsReady.size() || !dlssD3D12InputsReady[a_frameIndex]) {
		return false;
	}

	auto* dlssInput = dlssInputD3D12[a_frameIndex].get();
	auto* dlssOutput = dlssgHUDLessD3D12[a_frameIndex].get();
	auto* dlssSharpenedOutput = dlssSharpenedD3D12[a_frameIndex].get();
	auto* motionVectors = dlssgMotionVectorD3D12[a_frameIndex].get();
	auto* depth = dlssgDepthD3D12[a_frameIndex].get();
	auto* transparencyMask = dlssD3D12TransparencyMaskReady[a_frameIndex] ? dlssTransparencyMaskD3D12[a_frameIndex].get() : nullptr;
	if (!dlssInput || !dlssOutput || !motionVectors || !depth || !a_commandList) {
		return false;
	}

	auto streamline = Streamline::GetSingleton();
	const auto frameIndex = dlssgInputFrameTokenIndices[a_frameIndex];
	auto* frameToken = streamline->GetFrameTokenForFrame(frameIndex);
	if (!frameToken) {
		return false;
	}

	const bool useSharpenedOutput = settings.sharpness > 0.0f && dlssSharpenedOutput;
	const auto succeeded = Streamline::GetSingleton()->UpscaleD3D12(
		dlssInput,
		dlssOutput,
		useSharpenedOutput ? dlssSharpenedOutput : nullptr,
		motionVectors,
		depth,
		transparencyMask,
		a_commandList,
		frameToken,
		dlssgInputRenderSizes[a_frameIndex],
		dlssgInputDisplaySizes[a_frameIndex],
		dlssD3D12ColorFormats[a_frameIndex],
		dlssD3D12MotionVectorFormats[a_frameIndex],
		dlssD3D12DepthFormats[a_frameIndex],
		GetEffectiveQualityMode(UpscaleMethod::kDLSS, settings.qualityMode),
		settings.sharpness,
		settings.dlssModelPreset,
		&dlssD3D12Sharpened[a_frameIndex]);
	if (succeeded && useSharpenedOutput && !dlssD3D12Sharpened[a_frameIndex]) {
		D3D12_RESOURCE_BARRIER beforeCopy[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(dlssOutput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(dlssSharpenedOutput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
		};
		a_commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeCopy)), beforeCopy);
		a_commandList->CopyResource(dlssSharpenedOutput, dlssOutput);
		D3D12_RESOURCE_BARRIER afterCopy[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(dlssOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(dlssSharpenedOutput, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)
		};
		a_commandList->ResourceBarrier(static_cast<UINT>(std::size(afterCopy)), afterCopy);
		dlssD3D12Sharpened[a_frameIndex] = true;
	}

	dlssD3D12InputsReady[a_frameIndex] = false;
	dlssD3D12TransparencyMaskReady[a_frameIndex] = false;
	return succeeded;
}

bool Upscaling::EvaluateD3D12FSR(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex)
{
	if (a_frameIndex >= fsrD3D12InputsReady.size() || !fsrD3D12InputsReady[a_frameIndex]) {
		return false;
	}

	auto* color = fsrInputD3D12[a_frameIndex].get();
	auto* output = fsrOutputD3D12[a_frameIndex].get();
	auto* motionVectors = fsrMotionVectorD3D12[a_frameIndex].get();
	auto* depth = fsrDepthD3D12[a_frameIndex].get();
	auto* opaqueOnly = fsrOpaqueOnlyD3D12[a_frameIndex].get();
	auto* reactiveMask = fsrReactiveMaskD3D12[a_frameIndex].get();
	if (!color || !output || !motionVectors || !depth || !a_commandList) {
		fsrD3D12InputsReady[a_frameIndex] = false;
		return false;
	}

	const auto shaderReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	D3D12_RESOURCE_BARRIER beforeDispatch[6]{};
	UINT beforeDispatchCount = 0;
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(color, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (opaqueOnly && reactiveMask) {
		beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(opaqueOnly, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
		beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(reactiveMask, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	a_commandList->ResourceBarrier(beforeDispatchCount, beforeDispatch);

	const auto succeeded = FidelityFX::GetSingleton()->UpscaleD3D12(
		DX12SwapChain::GetSingleton()->GetD3D12Device(),
		a_commandList,
		color,
		output,
		motionVectors,
		depth,
		reactiveMask,
		opaqueOnly,
		fsrInputJitters[a_frameIndex],
		fsrInputRenderSizes[a_frameIndex],
		fsrInputDisplaySizes[a_frameIndex],
		settings.sharpness);

	D3D12_RESOURCE_BARRIER afterDispatch[6]{};
	UINT afterDispatchCount = 0;
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(color, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(depth, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
	if (opaqueOnly && reactiveMask) {
		afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(opaqueOnly, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
		afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(reactiveMask, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	}
	a_commandList->ResourceBarrier(afterDispatchCount, afterDispatch);

	fsrD3D12InputsReady[a_frameIndex] = false;
	return succeeded;
}

bool Upscaling::EvaluateFSRFrameGeneration(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex)
{
	if (a_frameIndex >= fsrFrameGenerationInputsReady.size() || !fsrFrameGenerationInputsReady[a_frameIndex]) {
		return false;
	}

	auto* color = dlssgHUDLessD3D12[a_frameIndex].get();
	auto* motionVectors = dlssgMotionVectorD3D12[a_frameIndex].get();
	auto* depth = dlssgDepthD3D12[a_frameIndex].get();
	auto* uiColorAlpha = dlssgUIColorAlphaD3D12[a_frameIndex].get();
	if (!color || !motionVectors || !depth || !a_commandList) {
		fsrFrameGenerationInputsReady[a_frameIndex] = false;
		return false;
	}

	const auto shaderReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	D3D12_RESOURCE_BARRIER beforeDispatch[4]{};
	UINT beforeDispatchCount = 0;
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(color, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	if (uiColorAlpha) {
		beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(uiColorAlpha, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	}
	a_commandList->ResourceBarrier(beforeDispatchCount, beforeDispatch);

	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	const auto succeeded = FidelityFX::GetSingleton()->ConfigureFrameGeneration(
		dx12SwapChain->GetD3D12Device(),
		a_commandList,
		dx12SwapChain->swapChain.get(),
		color,
		motionVectors,
		depth,
		color,
		uiColorAlpha,
		jitter,
		dlssgInputRenderSizes[a_frameIndex],
		dlssgInputDisplaySizes[a_frameIndex],
		fsrFrameGenerationColorFormats[a_frameIndex],
		fsrFrameGenerationFrameIDs[a_frameIndex],
		true);

	D3D12_RESOURCE_BARRIER afterDispatch[4]{};
	UINT afterDispatchCount = 0;
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(color, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(depth, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	if (uiColorAlpha) {
		afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(uiColorAlpha, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	}
	a_commandList->ResourceBarrier(afterDispatchCount, afterDispatch);

	fsrFrameGenerationInputsReady[a_frameIndex] = false;
	return succeeded;
}

void Upscaling::TagDLSSGInputs(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex)
{
	if (a_frameIndex >= dlssgInputsReady.size() || !dlssgInputsReady[a_frameIndex]) {
		auto streamline = Streamline::GetSingleton();
		if (streamline->NeedsDLSSGPresentSafety()) {
			if (!frameGenerationActive && streamline->dlssgActive) {
				streamline->RequestDLSSGDisable();
				streamline->ApplyPendingDLSSGDisable();
			}
			streamline->ClearDLSSGResourceTags(a_commandList);
		}
		return;
	}

	const auto frameTokenIndex = dlssgInputFrameTokenIndices[a_frameIndex];
	auto streamline = Streamline::GetSingleton();
	auto* hudlessColor = dlssD3D12PresentFinal[a_frameIndex].get();
	if (!hudlessColor) {
		hudlessColor = dlssgHUDLessD3D12[a_frameIndex].get();
	}
	streamline->TagDLSSGResources(
		hudlessColor,
		dlssgMotionVectorD3D12[a_frameIndex].get(),
		dlssgDepthD3D12[a_frameIndex].get(),
		dlssgUIColorAlphaD3D12[a_frameIndex].get(),
		a_commandList,
		frameTokenIndex,
		dlssgInputRenderSizes[a_frameIndex],
		dlssgInputDisplaySizes[a_frameIndex]);
	streamline->SetPresentFrameIndex(frameTokenIndex);

	dlssgInputsReady[a_frameIndex] = false;
}

void Upscaling::GetTaggedTextureDebugResources(uint32_t a_frameIndex, ID3D12Resource*& a_color, ID3D12Resource*& a_depth, ID3D12Resource*& a_motionVectors) const
{
	a_color = nullptr;
	a_depth = nullptr;
	a_motionVectors = nullptr;

	if (a_frameIndex >= dlssInputD3D12.size()) {
		return;
	}

	if (upscaleMethod == UpscaleMethod::kDLSS) {
		a_color = dlssInputD3D12[a_frameIndex].get();
		if (!a_color) {
			a_color = dlssgHUDLessD3D12[a_frameIndex].get();
		}
		a_depth = dlssgDepthD3D12[a_frameIndex].get();
		a_motionVectors = dlssgMotionVectorD3D12[a_frameIndex].get();
		if (!a_motionVectors) {
			a_motionVectors = debugMotionVectorD3D12[a_frameIndex].get();
		}
	} else if (upscaleMethod == UpscaleMethod::kFSR) {
		a_color = fsrInputD3D12[a_frameIndex].get();
		a_depth = fsrDepthD3D12[a_frameIndex].get();
		a_motionVectors = fsrMotionVectorD3D12[a_frameIndex].get();
	}
}

void Upscaling::CreateUpscalingResources()
{
	D3D11_TEXTURE2D_DESC texDesc{};
	auto foundTextureDesc = false;

	if (upscalingTexture && upscalingTexture->resource) {
		upscalingTexture->resource->GetDesc(&texDesc);
		foundTextureDesc = true;
	}
	else {
		auto renderer = RE::BSGraphics::GetRendererData();
		auto& main = renderer->renderTargets[(uint)Util::RenderTarget::kMain];
		if (main.texture) {
			reinterpret_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&texDesc);
			foundTextureDesc = true;
		}
	}

	if (!foundTextureDesc) {
		logger::warn("[Upscaling] Could not create upscaling resources: no valid source texture");
		return;
	}

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Create view descriptions
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = {
			.MostDetailedMip = 0,
			.MipLevels = 1 }
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = {.MipSlice = 0 }
	};

	if (Streamline::GetSingleton()->featureDLSS) {
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		uavDesc.Format = texDesc.Format;

		dlssOutputTexture = std::make_unique<Texture2D>(texDesc);
		dlssOutputTexture->CreateUAV(uavDesc);
	}

	// Create dilated motion vector texture for DLSS and FSR.
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
	uavDesc.Format = texDesc.Format;

	dilatedMotionVectorTexture = std::make_unique<Texture2D>(texDesc);
	dilatedMotionVectorTexture->CreateUAV(uavDesc);
}

void Upscaling::DestroyUpscalingResources()
{
	dlssOutputTexture = nullptr;
	dilatedMotionVectorTexture = nullptr;
	dlssgHUDLessTexture = nullptr;
	frameGenerationPreAlphaTexture = nullptr;
	frameGenerationMotionVectorTexture = nullptr;
	frameGenerationDepthTexture = nullptr;
	dlssTransparencyMaskTexture = nullptr;
	frameGenerationPreAlphaReady = false;
	frameGenerationPreAlphaFrame = 0;
	frameGenerationBuffersReady = false;
	dlssTransparencyMaskReady = false;
	for (std::size_t i = 0; i < dlssgInputsReady.size(); ++i) {
		dlssInputSharedTextures[i] = nullptr;
		dlssSharpenedSharedTextures[i] = nullptr;
		dlssgHUDLessSharedTextures[i] = nullptr;
		dlssgMotionVectorSharedTextures[i] = nullptr;
		dlssgDepthSharedTextures[i] = nullptr;
		dlssgUIColorAlphaSharedTextures[i] = nullptr;
		dlssTransparencyMaskSharedTextures[i] = nullptr;
		debugMotionVectorSharedTextures[i] = nullptr;
		fsrInputSharedTextures[i] = nullptr;
		fsrOutputSharedTextures[i] = nullptr;
		fsrMotionVectorSharedTextures[i] = nullptr;
		fsrDepthSharedTextures[i] = nullptr;
		dlssInputD3D12[i] = nullptr;
		dlssSharpenedD3D12[i] = nullptr;
		dlssD3D12PresentFinal[i] = nullptr;
		dlssgHUDLessD3D12[i] = nullptr;
		dlssgMotionVectorD3D12[i] = nullptr;
		dlssgDepthD3D12[i] = nullptr;
		dlssgUIColorAlphaD3D12[i] = nullptr;
		dlssTransparencyMaskD3D12[i] = nullptr;
		debugMotionVectorD3D12[i] = nullptr;
		fsrInputD3D12[i] = nullptr;
		fsrOutputD3D12[i] = nullptr;
		fsrMotionVectorD3D12[i] = nullptr;
		fsrDepthD3D12[i] = nullptr;
		dlssgInputsReady[i] = false;
		fsrD3D12InputsReady[i] = false;
		dlssD3D12InputsReady[i] = false;
		dlssD3D12Sharpened[i] = false;
		dlssD3D12TransparencyMaskReady[i] = false;
		dlssgInputFrameTokenIndices[i] = std::numeric_limits<uint32_t>::max();
		dlssgInputRenderSizes[i] = { 0.0f, 0.0f };
		dlssgInputDisplaySizes[i] = { 0.0f, 0.0f };
		fsrInputJitters[i] = { 0.0f, 0.0f };
		fsrInputRenderSizes[i] = { 0.0f, 0.0f };
		fsrInputDisplaySizes[i] = { 0.0f, 0.0f };
		dlssD3D12ColorFormats[i] = DXGI_FORMAT_UNKNOWN;
		dlssD3D12MotionVectorFormats[i] = DXGI_FORMAT_UNKNOWN;
		dlssD3D12DepthFormats[i] = DXGI_FORMAT_UNKNOWN;
	}
}

void Upscaling::PatchSSRShader()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	// Replace the game's SSR pixel shader with our custom one that fixes scaled render targets
	context->PSSetShader(GetBSImagespaceShaderSSLRRaytracing(), nullptr, 0);
}
