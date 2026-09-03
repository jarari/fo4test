#include "Upscaling.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <intrin.h>
#include <limits>
#include <memory>
#include <optional>
#include <SimpleIni.h>
#include <utility>
#include <vector>

#include "DX12SwapChain.h"
#include "ENB/ENBSeriesAPI.h"

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
			(a_name == "WorkbenchItem3D" || a_name == "Container3D" || a_name == "PipboyMenu")) {
			renderer->postAA = true;
			renderer->useFullPremultAlpha = true;
		}
		return renderer;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

float originalDynamicHeightRatio = 1.0f;
float originalDynamicWidthRatio = 1.0f;

namespace
{
	constexpr const char* kSettingsPath = "Data\\MCM\\Settings\\Upscaling.ini";
	constexpr uint32_t kDLSSGResumeStableFrames = 2;
	constexpr uint64_t kFeatureRetryGameFrames = 5;
	constexpr uint32_t kFeatureFailuresBeforeRetryBlock = 3;
	constexpr uint64_t kTextureMemoryUpgradeReserveBytes = 512ull * 1024ull * 1024ull;
	constexpr UINT kENBTrackedPSResourceCount = 16;
	constexpr std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>
		kNullD3D11ShaderResources{};
	FILETIME g_lastSettingsWriteTime{};
	bool g_hasLastSettingsWriteTime = false;
	bool g_lastSettingsFileExists = false;
	bool g_textureMemoryReserveApplied = false;
	winrt::com_ptr<ID3D11VertexShader> g_enbScaleCopyVS;
	winrt::com_ptr<ID3D11PixelShader> g_enbScaleCopyPS;
	winrt::com_ptr<ID3D11PixelShader> g_enbCropCopyPS;
	std::array<winrt::com_ptr<ID3D11PixelShader>, 3> g_enbScaleCopyMRTPS;
	winrt::com_ptr<ID3D11SamplerState> g_enbScaleCopySampler;
	winrt::com_ptr<ID3D11BlendState> g_enbScaleCopyBlendState;
	winrt::com_ptr<ID3D11DepthStencilState> g_enbScaleCopyDepthStencilState;
	winrt::com_ptr<ID3D11RasterizerState> g_enbScaleCopyRasterizerState;
	winrt::com_ptr<ID3D11Device> g_enbNativeDevice;
	winrt::com_ptr<ID3D11DeviceContext> g_enbNativeContext;
	winrt::com_ptr<ID3D11DeviceContext1> g_enbNativeContext1;
	winrt::com_ptr<ID3DDeviceContextState> g_enbScaleCopyContextState;
	bool g_enbScaleCopyContextStateInitialized = false;
	winrt::com_ptr<ID3D11Texture2D> g_enbNativeDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> g_enbNativeDepthSRV;
	winrt::com_ptr<ID3D11RenderTargetView> g_enbNativeDepthRTV;
	std::uint64_t g_enbNativeDepthFrame = std::numeric_limits<std::uint64_t>::max();
	std::uintptr_t g_enbTextureOriginalSRVAddress = 0;
	std::array<std::uintptr_t, 2> g_enbPrepassDepthSRVAddresses{};
	std::uint32_t* g_enbFullWidth = nullptr;
	std::uint32_t* g_enbFullHeight = nullptr;
	using ENBScreenEffectRender_t = int (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t);
	using D3D11Draw_t = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
	using ENBPSSetShader_t = void (STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*,
		ID3D11PixelShader*,
		ID3D11ClassInstance* const*,
		UINT);
	ENBScreenEffectRender_t g_enbDetailedShadowRender = nullptr;
	ENBPSSetShader_t g_enbPSSetShader = nullptr;
	bool g_enbDetailedShadowUsesFirstPersonModelPath = false;
	struct ENBDetailedShadowBufferLayout
	{
		std::size_t cpuDataOffset;
		std::size_t gpuBufferOffset;
	};
	std::optional<ENBDetailedShadowBufferLayout> g_enbDetailedShadowBufferLayout;
	constexpr std::size_t kENBDeferMixTrampolineSize = 64;
	REL::Trampoline g_enbDeferMixTrampoline{ "ENB DeferMix"sv };
	bool g_enbDeferMixInstalled = false;
	constexpr std::size_t kHFPFAELoadingLoopTrampolineSize = 64;
	REL::Trampoline g_hfpfAELoadingLoopTrampoline{ "HFPF AE Loading Loop"sv };
	ID3D11ShaderResourceView** g_enbDepthTextureSRVSlot = nullptr;
	thread_local int g_enbPrimaryCompositeScopeDepth = 0;
	thread_local int g_enbNativeImageSpaceParamScopeDepth = 0;
	thread_local int g_enbPrepassDepthBridgeScopeDepth = 0;
	float* GetGlobalDynamicWidthRatio();
	float* GetGlobalDynamicHeightRatio();
	void InstallENBScreenEffectRenderHooks();
	thread_local std::array<void*, 2> g_enbHDRFinalCompositeEffects{};
	thread_local void* g_enbRefractionCompositeEffect = nullptr;
	thread_local std::array<void*, 0x20> g_enbNativeImageSpaceShaders{};
	thread_local std::size_t g_enbNativeImageSpaceShaderCount = 0;
	std::array<std::atomic_bool, 0x48> g_loggedENBNativeImageSpaceEffects{};

	constexpr std::ptrdiff_t kImageSpaceEffectUseDynamicResolutionOffset = 0xA8;
	constexpr std::ptrdiff_t kImageSpaceEffectListOffset = 0x18;
	constexpr std::ptrdiff_t kImageSpaceEffectCountOffset = 0x22;
	constexpr std::ptrdiff_t kImageSpaceManagerNativeGeometryOffset = 0x28;
	constexpr std::ptrdiff_t kDynamicWidthRatioOffsetOG = 0xF88;
	constexpr std::ptrdiff_t kDynamicWidthRatioOffsetAE = 0xFB8;
	constexpr std::ptrdiff_t kDynamicHeightRatioOffsetOG = 0xF8C;
	constexpr std::ptrdiff_t kDynamicHeightRatioOffsetAE = 0xFBC;
	constexpr std::ptrdiff_t kDynamicResolutionActiveOffsetOG = 0xFA8;
	constexpr std::ptrdiff_t kDynamicResolutionActiveOffsetAE = 0xFE5;
	constexpr std::ptrdiff_t kServingThreadStateOffset = 0x68;
	constexpr std::ptrdiff_t kHFPFDisableLoadingAnimationPatchOffsetOG = 0x19D;
	constexpr std::ptrdiff_t kHFPFDisableLoadingAnimationPatchOffsetAE = 0x223;
	constexpr std::array<std::uint8_t, 4> kHFPFDisableLoadingAnimationPatchOG{ 0x0F, 0x1F, 0x40, 0x00 };
	constexpr std::array<std::uint8_t, 5> kHFPFDisableLoadingAnimationPatchAE{ 0x0F, 0x1F, 0x44, 0x00, 0x00 };
	constexpr std::array<std::uint8_t, 6> kHFPFAELoadingLoopBranch{ 0x0F, 0x84, 0xA7, 0xFE, 0xFF, 0xFF };
	constexpr std::array kENBNativeImageSpaceEffectIndices{
		2, 6, 7, 8, 9, 10, 11, 12, 13, 15, 20
	};

	bool IsENBNativeImageSpaceEffectIndex(int32_t a_effectIndex)
	{
		return std::binary_search(
			kENBNativeImageSpaceEffectIndices.begin(),
			kENBNativeImageSpaceEffectIndices.end(),
			a_effectIndex);
	}

	// LockpickingMenu and BookMenu own a custom final composition over a frozen
	// background. Require both flags so unrelated custom-rendering menus (for
	// example PipboyMenu) keep their normal path. This also covers compatible
	// third-party menus.
	bool IsCustomRenderingMenuOpen()
	{
		const auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}

		for (const auto& menu : ui->menuStack) {
			const auto* menuInstance = menu.get();
			if (!menuInstance || !menuInstance->OnStack()) {
				continue;
			}

			if (menuInstance->menuFlags.all(RE::UI_MENU_FLAGS::kCustomRendering) &&
				menuInstance->menuFlags.all(RE::UI_MENU_FLAGS::kFreezeFrameBackground)) {
				return true;
			}
		}

		return false;
	}

	class ScopedENBNativeImageSpaceParams
	{
	public:
		ScopedENBNativeImageSpaceParams()
		{
			++g_enbNativeImageSpaceParamScopeDepth;
		}

		~ScopedENBNativeImageSpaceParams()
		{
			--g_enbNativeImageSpaceParamScopeDepth;
		}

		ScopedENBNativeImageSpaceParams(const ScopedENBNativeImageSpaceParams&) = delete;
		ScopedENBNativeImageSpaceParams& operator=(const ScopedENBNativeImageSpaceParams&) = delete;
	};

	class ScopedENBPrimaryCompositeScope
	{
	public:
		ScopedENBPrimaryCompositeScope()
		{
			++g_enbPrimaryCompositeScopeDepth;
		}

		~ScopedENBPrimaryCompositeScope()
		{
			--g_enbPrimaryCompositeScopeDepth;
		}

		ScopedENBPrimaryCompositeScope(const ScopedENBPrimaryCompositeScope&) = delete;
		ScopedENBPrimaryCompositeScope& operator=(const ScopedENBPrimaryCompositeScope&) = delete;
	};

	class ScopedENBPrepassDepthBridgeActivation
	{
	public:
		explicit ScopedENBPrepassDepthBridgeActivation(bool a_active) :
			active_(a_active)
		{
			if (active_) {
				++g_enbPrepassDepthBridgeScopeDepth;
			}
		}

		~ScopedENBPrepassDepthBridgeActivation()
		{
			if (active_) {
				--g_enbPrepassDepthBridgeScopeDepth;
			}
		}

		ScopedENBPrepassDepthBridgeActivation(const ScopedENBPrepassDepthBridgeActivation&) = delete;
		ScopedENBPrepassDepthBridgeActivation& operator=(const ScopedENBPrepassDepthBridgeActivation&) = delete;

	private:
		bool active_{ false };
	};

	bool ShouldForceENBNativeImageSpaceParams(int32_t a_effectIndex)
	{
		return g_enbNativeImageSpaceParamScopeDepth > 0 &&
			IsENBNativeImageSpaceEffectIndex(a_effectIndex);
	}

	struct JobListManager_ServingThread_DisplayLoadingScreen
	{
		static void thunk(void* a_this)
		{
			func(a_this);
			if (!DX12SwapChain::GetSingleton()->IsReady()) {
				return;
			}

			auto* state = reinterpret_cast<volatile std::int32_t*>(
				reinterpret_cast<std::byte*>(a_this) + kServingThreadStateOffset);
			while (*state == 2) {
				Sleep(250);
				if (*state != 2) {
					break;
				}
				try {
					func(a_this);
				} catch (const std::exception& e) {
					logger::error("[Upscaling] Loading-screen renderer repump aborted: {}", e.what());
					break;
				} catch (...) {
					logger::error("[Upscaling] Loading-screen renderer repump aborted by an unknown exception");
					break;
				}
			}
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	template <std::size_t N>
	bool HasHFPFDisableLoadingAnimationPatch(
		std::uintptr_t a_servingThreadFunction,
		std::ptrdiff_t a_patchOffset,
		const std::array<std::uint8_t, N>& a_patch)
	{
		std::array<std::uint8_t, N> bytes{};
		std::memcpy(
			bytes.data(),
			reinterpret_cast<const void*>(a_servingThreadFunction + a_patchOffset),
			bytes.size());
		return bytes == a_patch;
	}

	std::uint32_t HFPFAELoadingLoopShouldExit(void* a_this) noexcept
	{
		if (!DX12SwapChain::GetSingleton()->IsReady()) {
			return 1;
		}

		const auto* state = reinterpret_cast<volatile const std::int32_t*>(
			reinterpret_cast<const std::byte*>(a_this) + kServingThreadStateOffset);
		if (*state != 2) {
			return 1;
		}

		Sleep(250);
		return *state == 2 ? 0u : 1u;
	}

	bool InstallHFPFAELoadingLoopCompatibility(std::uintptr_t a_servingThreadFunction)
	{
		const auto patchAddress =
			a_servingThreadFunction + kHFPFDisableLoadingAnimationPatchOffsetAE;
		if (!HasHFPFDisableLoadingAnimationPatch(
				a_servingThreadFunction,
				kHFPFDisableLoadingAnimationPatchOffsetAE,
				kHFPFDisableLoadingAnimationPatchAE) ||
			!std::equal(
				kHFPFAELoadingLoopBranch.begin(),
				kHFPFAELoadingLoopBranch.end(),
				reinterpret_cast<const std::uint8_t*>(patchAddress +
					kHFPFDisableLoadingAnimationPatchAE.size()))) {
			return false;
		}

		// AE inlines DisplayLoadingScreen into ServingThread::ThreadProc. Replace
		// only HFPF's five-byte NOP at the loop tail; detouring ID 2227631 as the
		// OG one-argument render helper would replace the entire thread procedure.
		if (g_hfpfAELoadingLoopTrampoline.empty()) {
			g_hfpfAELoadingLoopTrampoline.create(
				kHFPFAELoadingLoopTrampolineSize,
				reinterpret_cast<void*>(patchAddress));
		}

		constexpr std::size_t kStubSize = 26;
		auto* stub = static_cast<std::uint8_t*>(
			g_hfpfAELoadingLoopTrampoline.allocate(kStubSize));
		if (!stub) {
			return false;
		}
		const std::array<std::uint8_t, kStubSize> stubTemplate{
			0x48, 0x83, 0xEC, 0x28,                         // sub rsp,28h
			0x4C, 0x89, 0xF1,                               // mov rcx,r14
			0x48, 0xB8,                                     // mov rax,imm64
			0, 0, 0, 0, 0, 0, 0, 0,
			0xFF, 0xD0,                                     // call rax
			0x48, 0x83, 0xC4, 0x28,                         // add rsp,28h
			0x85, 0xC0,                                     // test eax,eax
			0xC3                                            // ret
		};
		std::memcpy(stub, stubTemplate.data(), stubTemplate.size());
		const auto helper = reinterpret_cast<std::uintptr_t>(&HFPFAELoadingLoopShouldExit);
		std::memcpy(stub + 9, &helper, sizeof(helper));

		const auto displacement64 = reinterpret_cast<std::int64_t>(stub) -
			static_cast<std::int64_t>(patchAddress + 5);
		if (displacement64 < std::numeric_limits<std::int32_t>::min() ||
			displacement64 > std::numeric_limits<std::int32_t>::max()) {
			return false;
		}

		std::array<std::uint8_t, 5> callPatch{ 0xE8, 0, 0, 0, 0 };
		const auto displacement = static_cast<std::int32_t>(displacement64);
		std::memcpy(callPatch.data() + 1, &displacement, sizeof(displacement));
		return REL::WriteSafeData(patchAddress, callPatch);
	}

	class ScopedImageSpaceEffectNativeParams
	{
	public:
		ScopedImageSpaceEffectNativeParams(void* a_effect, int32_t a_effectIndex)
		{
			if (!a_effect || !ShouldForceENBNativeImageSpaceParams(a_effectIndex)) {
				return;
			}

			useDynamicResolution_ = reinterpret_cast<std::uint8_t*>(
				reinterpret_cast<std::byte*>(a_effect) + kImageSpaceEffectUseDynamicResolutionOffset);
			originalUseDynamicResolution_ = *useDynamicResolution_;
			*useDynamicResolution_ = 0;
		}

		~ScopedImageSpaceEffectNativeParams()
		{
			if (useDynamicResolution_) {
				*useDynamicResolution_ = originalUseDynamicResolution_;
			}
		}

		ScopedImageSpaceEffectNativeParams(const ScopedImageSpaceEffectNativeParams&) = delete;
		ScopedImageSpaceEffectNativeParams& operator=(const ScopedImageSpaceEffectNativeParams&) = delete;

	private:
		std::uint8_t* useDynamicResolution_{ nullptr };
		std::uint8_t originalUseDynamicResolution_{ 0 };
	};

	class ScopedENBHDRFinalCompositeEffects
	{
	public:
		ScopedENBHDRFinalCompositeEffects(void* a_effect, int32_t a_effectIndex)
		{
			if (!a_effect || g_enbNativeImageSpaceParamScopeDepth <= 0 || a_effectIndex != 3) {
				return;
			}

			// ImageSpaceEffectHDR owns its final tonemap variants in child slots 2
			// and 3. The engine selects one immediately before the final dispatch.
			const auto childEffects = *reinterpret_cast<void***>(
				reinterpret_cast<std::byte*>(a_effect) + 0x18);
			if (!childEffects) {
				return;
			}

			previousEffects_ = g_enbHDRFinalCompositeEffects;
			g_enbHDRFinalCompositeEffects = { childEffects[2], childEffects[3] };
			active_ = true;
		}

		~ScopedENBHDRFinalCompositeEffects()
		{
			if (active_) {
				g_enbHDRFinalCompositeEffects = previousEffects_;
			}
		}

		ScopedENBHDRFinalCompositeEffects(const ScopedENBHDRFinalCompositeEffects&) = delete;
		ScopedENBHDRFinalCompositeEffects& operator=(const ScopedENBHDRFinalCompositeEffects&) = delete;

	private:
		std::array<void*, 2> previousEffects_{};
		bool active_{ false };
	};

	bool IsENBHDRFinalCompositeEffect(const void* a_effect)
	{
		return a_effect &&
			(a_effect == g_enbHDRFinalCompositeEffects[0] ||
				a_effect == g_enbHDRFinalCompositeEffects[1]);
	}

	class ScopedENBNativeImageSpaceShaders
	{
	public:
		ScopedENBNativeImageSpaceShaders(void* a_effect, int32_t a_effectIndex)
		{
			if (!a_effect || g_enbNativeImageSpaceParamScopeDepth <= 0 ||
				!IsENBNativeImageSpaceEffectIndex(a_effectIndex)) {
				return;
			}

			previousShaders_ = g_enbNativeImageSpaceShaders;
			previousCount_ = g_enbNativeImageSpaceShaderCount;
			g_enbNativeImageSpaceShaders.fill(nullptr);
			if (a_effectIndex == 15) {
				// GammaCorrectLUT is itself a BSImagespaceShader rather than a
				// container effect. Track the effect object directly so the tiled
				// RenderEffect path cannot substitute its +0x40 geometry.
				g_enbNativeImageSpaceShaders[0] = a_effect;
				g_enbNativeImageSpaceShaderCount = 1;
				active_ = true;
				return;
			}

			const auto effectCount = *reinterpret_cast<const uint16_t*>(
				reinterpret_cast<const std::byte*>(a_effect) + kImageSpaceEffectCountOffset);
			const auto effectList = *reinterpret_cast<void***>(
				reinterpret_cast<std::byte*>(a_effect) + kImageSpaceEffectListOffset);
			if (effectCount == 0 || !effectList) {
				return;
			}

			g_enbNativeImageSpaceShaderCount = std::min<std::size_t>(
				effectCount,
				g_enbNativeImageSpaceShaders.size());
			std::copy_n(
				effectList,
				g_enbNativeImageSpaceShaderCount,
				g_enbNativeImageSpaceShaders.begin());
			active_ = true;
		}

		~ScopedENBNativeImageSpaceShaders()
		{
			if (active_) {
				g_enbNativeImageSpaceShaders = previousShaders_;
				g_enbNativeImageSpaceShaderCount = previousCount_;
			}
		}

		ScopedENBNativeImageSpaceShaders(const ScopedENBNativeImageSpaceShaders&) = delete;
		ScopedENBNativeImageSpaceShaders& operator=(const ScopedENBNativeImageSpaceShaders&) = delete;

	private:
		std::array<void*, 0x20> previousShaders_{};
		std::size_t previousCount_{ 0 };
		bool active_{ false };
	};

	bool IsENBNativeImageSpaceShader(const void* a_shader)
	{
		return a_shader && std::find(
			g_enbNativeImageSpaceShaders.begin(),
			g_enbNativeImageSpaceShaders.begin() + g_enbNativeImageSpaceShaderCount,
			a_shader) != g_enbNativeImageSpaceShaders.begin() + g_enbNativeImageSpaceShaderCount;
	}

	std::string_view ImageSpaceEffectName(int32_t a_effectIndex)
	{
		switch (a_effectIndex) {
		case 0: return "WorldCamera"sv;
		case 1: return "TemporalAAPreHDR"sv;
		case 2: return "Sunbeams"sv;
		case 3: return "HDR"sv;
		case 4: return "HDRCS"sv;
		case 5: return "Refraction"sv;
		case 6: return "DepthOfField"sv;
		case 7: return "DepthOfFieldSplitScreen"sv;
		case 8: return "RadialBlur"sv;
		case 9: return "FullScreenBlur"sv;
		case 10: return "MotionBlur"sv;
		case 11: return "GetHit"sv;
		case 12: return "VatsTarget"sv;
		case 13: return "FullScreenColor"sv;
		case 14: return "GammaCorrect"sv;
		case 15: return "GammaCorrectLUT"sv;
		case 16: return "GammaCorrectResize"sv;
		case 17: return "FXAA"sv;
		case 18: return "TemporalAA"sv;
		case 19: return "TemporalOldAA"sv;
		case 20: return "BokehDepthOfField"sv;
		case 21: return "UpsampleDynamicResolution"sv;
		default: return "Unknown"sv;
		}
	}

	void ResetENBNativeImageSpaceEffectLog()
	{
		for (auto& logged : g_loggedENBNativeImageSpaceEffects) {
			logged.store(false, std::memory_order_relaxed);
		}
	}

	void LogENBNativeImageSpaceEffect(
		void* a_effect,
		int32_t a_effectIndex,
		int a_targetA,
		int a_targetB)
	{
		auto* upscaling = Upscaling::GetSingleton();
		if (!a_effect || !upscaling || upscaling->settings.imageSpaceEffectLog == 0 ||
			g_enbNativeImageSpaceParamScopeDepth <= 0 || a_effectIndex < 0 ||
			a_effectIndex >= static_cast<int32_t>(g_loggedENBNativeImageSpaceEffects.size()) ||
			g_loggedENBNativeImageSpaceEffects[a_effectIndex].exchange(true, std::memory_order_relaxed)) {
			return;
		}

		const auto* bytes = reinterpret_cast<const std::byte*>(a_effect);
		const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_effect);
		const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const auto vtableRVA = vtable >= moduleBase ? vtable - moduleBase : 0;
		const auto childCount = *reinterpret_cast<const std::uint16_t*>(
			bytes + kImageSpaceEffectCountOffset);
		const auto rawActive = *reinterpret_cast<const std::uint8_t*>(bytes + 0x08) != 0;
		const auto isComputeShader = *reinterpret_cast<const std::uint8_t*>(bytes + 0xA0) != 0;
		const auto outputCount = *reinterpret_cast<const std::uint32_t*>(bytes + 0xA4);
		const auto useDynamicResolution = *reinterpret_cast<const std::uint8_t*>(
			bytes + kImageSpaceEffectUseDynamicResolutionOffset) != 0;
		logger::info(
			"[ENB IS Log] index={} name={} range={} dispatchActive=true rawActive={} useDR={} compute={} outputs={} children={} targetA={} targetB={} effect={} vtable={} Fallout4RVA=0x{:X}",
			a_effectIndex,
			ImageSpaceEffectName(a_effectIndex),
			a_effectIndex <= 13 ? "early" : "late",
			rawActive,
			useDynamicResolution,
			isComputeShader,
			outputCount,
			childCount,
			a_targetA,
			a_targetB,
			a_effect,
			reinterpret_cast<const void*>(vtable),
			vtableRVA);
	}

	class ScopedENBRefractionCompositeEffect
	{
	public:
		ScopedENBRefractionCompositeEffect(void* a_effect, int32_t a_effectIndex)
		{
			if (!a_effect || g_enbNativeImageSpaceParamScopeDepth <= 0 || a_effectIndex != 5) {
				return;
			}

			const auto effectCount = *reinterpret_cast<const uint16_t*>(
				reinterpret_cast<const std::byte*>(a_effect) + kImageSpaceEffectCountOffset);
			const auto effectList = *reinterpret_cast<void***>(
				reinterpret_cast<std::byte*>(a_effect) + kImageSpaceEffectListOffset);
			if (effectCount == 0 || !effectList || !effectList[0]) {
				return;
			}

			previousEffect_ = g_enbRefractionCompositeEffect;
			g_enbRefractionCompositeEffect = effectList[0];
			active_ = true;
		}

		~ScopedENBRefractionCompositeEffect()
		{
			if (active_) {
				g_enbRefractionCompositeEffect = previousEffect_;
			}
		}

		ScopedENBRefractionCompositeEffect(const ScopedENBRefractionCompositeEffect&) = delete;
		ScopedENBRefractionCompositeEffect& operator=(const ScopedENBRefractionCompositeEffect&) = delete;

	private:
		void* previousEffect_{ nullptr };
		bool active_{ false };
	};

	class ScopedBSImagespaceShaderNativeParams
	{
	public:
		explicit ScopedBSImagespaceShaderNativeParams(void* a_shader)
		{
			if (!a_shader) {
				return;
			}

			useDynamicResolution_ = reinterpret_cast<uint8_t*>(
				reinterpret_cast<std::byte*>(a_shader) + kImageSpaceEffectUseDynamicResolutionOffset);
			originalUseDynamicResolution_ = *useDynamicResolution_;
			*useDynamicResolution_ = 0;
		}

		~ScopedBSImagespaceShaderNativeParams()
		{
			if (useDynamicResolution_) {
				*useDynamicResolution_ = originalUseDynamicResolution_;
			}
		}

		ScopedBSImagespaceShaderNativeParams(const ScopedBSImagespaceShaderNativeParams&) = delete;
		ScopedBSImagespaceShaderNativeParams& operator=(const ScopedBSImagespaceShaderNativeParams&) = delete;

	private:
		uint8_t* useDynamicResolution_{ nullptr };
		uint8_t originalUseDynamicResolution_{ 0 };
	};

	void* GetNativeImageSpaceGeometry()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (!imageSpaceManager) {
			return nullptr;
		}

		return *reinterpret_cast<void**>(
			reinterpret_cast<std::byte*>(imageSpaceManager) + kImageSpaceManagerNativeGeometryOffset);
	}

	HMODULE FindENBModule()
	{
		static HMODULE cached = nullptr;
		if (cached) {
			return cached;
		}

		DWORD cbNeeded = 0;
		std::array<HMODULE, 1000> modules{};
		if (!EnumProcessModules(
				GetCurrentProcess(),
				modules.data(),
				static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
				&cbNeeded)) {
			return nullptr;
		}

		const auto count = std::min<std::size_t>(modules.size(), cbNeeded / sizeof(HMODULE));
		for (std::size_t i = 0; i < count; ++i) {
			if (modules[i] && GetProcAddress(modules[i], "ENBGetSDKVersion")) {
				cached = modules[i];
				break;
			}
		}
		return cached;
	}

	enum class ENBBoolSetting : std::uint8_t
	{
		kUseEffect,
		kSubSurfaceScattering,
		kPrepass,
		kTotal
	};

	std::optional<bool> QueryENBBool(const char* a_category, const char* a_key)
	{
		if (!enbLoaded) {
			return std::nullopt;
		}

		const auto enbModule = FindENBModule();
		if (!enbModule) {
			return std::nullopt;
		}

		static ENB_SDK::_ENBGetParameterA getParameter = nullptr;
		if (!getParameter) {
			getParameter = reinterpret_cast<ENB_SDK::_ENBGetParameterA>(
				GetProcAddress(enbModule, "ENBGetParameter"));
		}
		if (!getParameter) {
			return std::nullopt;
		}

		ENB_SDK::ENBParameter param{};
		const bool found =
			getParameter("enbseries.ini", a_category, a_key, &param) ||
			getParameter(nullptr, a_category, a_key, &param);
		if (!found || param.Type != ENB_SDK::ENBParameterType::ENBParam_BOOL || param.Size < sizeof(BOOL)) {
			return std::nullopt;
		}

		return *reinterpret_cast<BOOL*>(param.Data) != FALSE;
	}

	std::optional<bool> TryGetENBBool(ENBBoolSetting a_setting)
	{
		struct CacheEntry
		{
			std::uint64_t frame{ std::numeric_limits<std::uint64_t>::max() };
			std::optional<bool> value;
		};

		static thread_local std::array<CacheEntry, static_cast<std::size_t>(ENBBoolSetting::kTotal)> cache{};
		static auto* gameViewport = Util::State_GetSingleton();
		const auto frame = gameViewport ?
			static_cast<std::uint64_t>(gameViewport->frameCount) :
			std::numeric_limits<std::uint64_t>::max() - 1;
		auto& entry = cache[static_cast<std::size_t>(a_setting)];
		if (entry.frame == frame) {
			return entry.value;
		}

		entry.frame = frame;
		switch (a_setting) {
		case ENBBoolSetting::kUseEffect:
			entry.value = QueryENBBool("GLOBAL", "UseEffect");
			break;
		case ENBBoolSetting::kSubSurfaceScattering:
			entry.value = QueryENBBool("EFFECT", "EnableSubSurfaceScattering");
			break;
		case ENBBoolSetting::kPrepass:
			entry.value = QueryENBBool("EFFECT", "EnablePrepass");
			break;
		default:
			entry.value = std::nullopt;
			break;
		}
		return entry.value;
	}

	bool IsTemporalSuperResolutionMethod(Upscaling::UpscaleMethod a_upscaleMethod)
	{
		return a_upscaleMethod == Upscaling::UpscaleMethod::kDLSS ||
			a_upscaleMethod == Upscaling::UpscaleMethod::kFSR;
	}

	bool IsENBUseEffectActive()
	{
		return enbLoaded && TryGetENBBool(ENBBoolSetting::kUseEffect).value_or(false);
	}

	bool IsENBSubSurfaceScatteringActive()
	{
		// The SSS compatibility path is valid only while ENB's master effect and
		// the effect-specific switch are both enabled. Keep this as one predicate
		// so a future SSS hook cannot accidentally bypass the master UseEffect gate.
		return IsENBUseEffectActive() &&
			TryGetENBBool(ENBBoolSetting::kSubSurfaceScattering).value_or(false);
	}

	bool ShouldBypassDynamicResolutionHooksForInactiveENB()
	{
		const auto useEffect = TryGetENBBool(ENBBoolSetting::kUseEffect);
		return enbLoaded && useEffect.has_value() && !*useEffect;
	}

	bool IsENBSRCompatibilityActive(Upscaling::UpscaleMethod a_upscaleMethod)
	{
		return IsTemporalSuperResolutionMethod(a_upscaleMethod) &&
			IsENBUseEffectActive();
	}

	bool TryGetSettingsWriteTime(FILETIME& a_writeTime)
	{
		WIN32_FILE_ATTRIBUTE_DATA data{};
		if (!GetFileAttributesExA(kSettingsPath, GetFileExInfoStandard, &data)) {
			return false;
		}

		a_writeTime = data.ftLastWriteTime;
		return true;
	}

	void RememberSettingsWriteTime()
	{
		FILETIME writeTime{};
		if (TryGetSettingsWriteTime(writeTime)) {
			g_lastSettingsWriteTime = writeTime;
			g_lastSettingsFileExists = true;
			g_hasLastSettingsWriteTime = true;
		} else {
			g_lastSettingsWriteTime = {};
			g_lastSettingsFileExists = false;
			g_hasLastSettingsWriteTime = true;
		}
	}

	bool SettingsFileChangedSinceLastLoad()
	{
		FILETIME writeTime{};
		if (!TryGetSettingsWriteTime(writeTime)) {
			return !g_hasLastSettingsWriteTime || g_lastSettingsFileExists;
		}

		return !g_hasLastSettingsWriteTime || !g_lastSettingsFileExists || CompareFileTime(&writeTime, &g_lastSettingsWriteTime) != 0;
	}

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

	struct DynamicResolutionRatios
	{
		float width{ 1.0f };
		float height{ 1.0f };
	};

	DynamicResolutionRatios GetDynamicResolutionRatios()
	{
		return {
			*GetGlobalDynamicWidthRatio(),
			*GetGlobalDynamicHeightRatio()
		};
	}

	bool IsDynamicResolutionScaled()
	{
		const auto ratios = GetDynamicResolutionRatios();
		return ratios.width != 1.0f || ratios.height != 1.0f;
	}

	void SetDynamicResolutionRatio(RE::BSGraphics::RenderTargetManager* a_renderTargetManager, float a_widthRatio, float a_heightRatio)
	{
		if (a_renderTargetManager) {
			// RenderTargetManager's tail layout is not ABI-stable: AE moves the
			// ratio fields and active flag independently of the logical RT table.
			auto* const managerBytes = reinterpret_cast<std::byte*>(a_renderTargetManager);
			const bool isOG = REX::FModule::IsRuntimeOG();
			*reinterpret_cast<float*>(managerBytes + (isOG ? kDynamicWidthRatioOffsetOG : kDynamicWidthRatioOffsetAE)) = a_widthRatio;
			*reinterpret_cast<float*>(managerBytes + (isOG ? kDynamicHeightRatioOffsetOG : kDynamicHeightRatioOffsetAE)) = a_heightRatio;
			*reinterpret_cast<bool*>(managerBytes + (isOG ? kDynamicResolutionActiveOffsetOG : kDynamicResolutionActiveOffsetAE)) =
				a_widthRatio != 1.0f || a_heightRatio != 1.0f;
		}

		*GetGlobalDynamicWidthRatio() = a_widthRatio;
		*GetGlobalDynamicHeightRatio() = a_heightRatio;
	}

	void ApplyCurrentViewportDefault(RE::BSGraphics::RenderTargetManager* a_renderTargetManager)
	{
		using SetCurrentViewportDefault_t = void (*)(RE::BSGraphics::RenderTargetManager*);
		static REL::Relocation<SetCurrentViewportDefault_t> setCurrentViewportDefault{ REL::ID{ 158420, 2277192 } };
		setCurrentViewportDefault(a_renderTargetManager);
	}

	void ApplyFullFrameViewport()
	{
		static auto rendererData = RE::BSGraphics::GetRendererData();
		static auto gameViewport = Util::State_GetSingleton();
		if (!rendererData || !rendererData->context || !gameViewport) {
			return;
		}

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
		reinterpret_cast<ID3D11DeviceContext*>(rendererData->context)->RSSetViewports(1, &viewport);
	}

	bool EnsureENBNativeD3D11Context(ID3D11Resource* a_resource)
	{
		if (g_enbNativeDevice && g_enbNativeContext) {
			return true;
		}
		if (!a_resource) {
			return false;
		}

		g_enbNativeContext = nullptr;
		g_enbNativeDevice = nullptr;
		a_resource->GetDevice(g_enbNativeDevice.put());
		if (!g_enbNativeDevice) {
			return false;
		}
		g_enbNativeDevice->GetImmediateContext(g_enbNativeContext.put());
		if (!g_enbNativeContext) {
			g_enbNativeDevice = nullptr;
			return false;
		}
		return true;
	}

	bool CanBypassENBD3D11Context(ID3D11DeviceContext* a_wrappedContext)
	{
		if (!g_enbNativeContext || g_enbNativeContext.get() == a_wrappedContext) {
			return false;
		}
		struct Cache
		{
			ID3D11DeviceContext* nativeContext{};
			ID3D11DeviceContext* wrappedContext{};
			bool canBypass{};
		};
		static Cache cache;
		if (cache.nativeContext == g_enbNativeContext.get() &&
			cache.wrappedContext == a_wrappedContext) {
			return cache.canBypass;
		}

		auto*** const object = reinterpret_cast<void***>(g_enbNativeContext.get());
		if (!object || !*object || !**object) {
			cache = { g_enbNativeContext.get(), a_wrappedContext, false };
			return false;
		}

		MEMORY_BASIC_INFORMATION memory{};
		if (VirtualQuery(**object, &memory, sizeof(memory)) != sizeof(memory)) {
			cache = { g_enbNativeContext.get(), a_wrappedContext, false };
			return false;
		}
		const auto enbModule = FindENBModule();
		cache = {
			g_enbNativeContext.get(),
			a_wrappedContext,
			enbModule && memory.AllocationBase != enbModule
		};
		return cache.canBypass;
	}

	bool EnsureENBScaleCopyResources(ID3D11Device* a_device)
	{
		if (!a_device) {
			return false;
		}
		if (g_enbScaleCopyVS && g_enbScaleCopyPS && g_enbCropCopyPS && g_enbScaleCopySampler &&
			g_enbScaleCopyBlendState && g_enbScaleCopyDepthStencilState && g_enbScaleCopyRasterizerState) {
			return true;
		}

		constexpr const char* shaderSource = R"(
Texture2D sourceTextures[8] : register(t0);
SamplerState sourceSampler : register(s0);

struct VSOut
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertexID : SV_VertexID)
{
	float2 positions[3] = {
		float2(-1.0,  1.0),
		float2( 3.0,  1.0),
		float2(-1.0, -3.0)
	};
	float2 uvs[3] = {
		float2(0.0, 0.0),
		float2(2.0, 0.0),
		float2(0.0, 2.0)
	};

	VSOut output;
	output.position = float4(positions[vertexID], 0.0, 1.0);
	output.uv = uvs[vertexID];
	return output;
}

float4 PSMain(VSOut input) : SV_Target
{
	return sourceTextures[0].SampleLevel(sourceSampler, input.uv, 0.0);
}

float4 PSMainCrop(VSOut input) : SV_Target
{
	return sourceTextures[0].Load(int3(uint2(input.position.xy), 0));
}

struct PSOut2
{
	float4 color0 : SV_Target0;
	float4 color1 : SV_Target1;
};

PSOut2 PSMainMRT2(VSOut input)
{
	PSOut2 output;
	output.color0 = sourceTextures[0].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color1 = sourceTextures[1].SampleLevel(sourceSampler, input.uv, 0.0);
	return output;
}

struct PSOut4
{
	float4 color0 : SV_Target0;
	float4 color1 : SV_Target1;
	float4 color2 : SV_Target2;
	float4 color3 : SV_Target3;
};

PSOut4 PSMainMRT4(VSOut input)
{
	PSOut4 output;
	output.color0 = sourceTextures[0].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color1 = sourceTextures[1].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color2 = sourceTextures[2].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color3 = sourceTextures[3].SampleLevel(sourceSampler, input.uv, 0.0);
	return output;
}

struct PSOut8
{
	float4 color0 : SV_Target0;
	float4 color1 : SV_Target1;
	float4 color2 : SV_Target2;
	float4 color3 : SV_Target3;
	float4 color4 : SV_Target4;
	float4 color5 : SV_Target5;
	float4 color6 : SV_Target6;
	float4 color7 : SV_Target7;
};

PSOut8 PSMainMRT8(VSOut input)
{
	PSOut8 output;
	output.color0 = sourceTextures[0].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color1 = sourceTextures[1].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color2 = sourceTextures[2].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color3 = sourceTextures[3].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color4 = sourceTextures[4].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color5 = sourceTextures[5].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color6 = sourceTextures[6].SampleLevel(sourceSampler, input.uv, 0.0);
	output.color7 = sourceTextures[7].SampleLevel(sourceSampler, input.uv, 0.0);
	return output;
}
)";

		winrt::com_ptr<ID3DBlob> vertexShaderBlob;
		winrt::com_ptr<ID3DBlob> pixelShaderBlob;
		winrt::com_ptr<ID3DBlob> cropPixelShaderBlob;
		std::array<winrt::com_ptr<ID3DBlob>, 3> mrtPixelShaderBlobs;
		winrt::com_ptr<ID3DBlob> errors;
		const auto compileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		auto hr = D3DCompile(
			shaderSource,
			std::strlen(shaderSource),
			"ENBScaleCopy",
			nullptr,
			nullptr,
			"VSMain",
			"vs_5_0",
			compileFlags,
			0,
			vertexShaderBlob.put(),
			errors.put());
		if (FAILED(hr)) {
			logger::warn("[Upscaling] Failed to compile ENB scale-copy VS: {}", errors ? static_cast<char*>(errors->GetBufferPointer()) : "unknown error");
			return false;
		}

		errors = nullptr;
		hr = D3DCompile(
			shaderSource,
			std::strlen(shaderSource),
			"ENBScaleCopy",
			nullptr,
			nullptr,
			"PSMain",
			"ps_5_0",
			compileFlags,
			0,
			pixelShaderBlob.put(),
			errors.put());
		if (FAILED(hr)) {
			logger::warn("[Upscaling] Failed to compile ENB scale-copy PS: {}", errors ? static_cast<char*>(errors->GetBufferPointer()) : "unknown error");
			return false;
		}

		errors = nullptr;
		hr = D3DCompile(
			shaderSource,
			std::strlen(shaderSource),
			"ENBScaleCopy",
			nullptr,
			nullptr,
			"PSMainCrop",
			"ps_5_0",
			compileFlags,
			0,
			cropPixelShaderBlob.put(),
			errors.put());
		if (FAILED(hr)) {
			logger::warn("[Upscaling] Failed to compile ENB crop-copy PS: {}", errors ? static_cast<char*>(errors->GetBufferPointer()) : "unknown error");
			return false;
		}

		constexpr std::array<const char*, 3> mrtEntryPoints{
			"PSMainMRT2", "PSMainMRT4", "PSMainMRT8"
		};
		for (std::size_t i = 0; i < mrtEntryPoints.size(); ++i) {
			errors = nullptr;
			hr = D3DCompile(
				shaderSource,
				std::strlen(shaderSource),
				"ENBScaleCopy",
				nullptr,
				nullptr,
				mrtEntryPoints[i],
				"ps_5_0",
				compileFlags,
				0,
				mrtPixelShaderBlobs[i].put(),
				errors.put());
			if (FAILED(hr)) {
				logger::warn(
					"[Upscaling] Failed to compile ENB scale-copy {}: {}",
					mrtEntryPoints[i],
					errors ? static_cast<char*>(errors->GetBufferPointer()) : "unknown error");
				mrtPixelShaderBlobs[i] = nullptr;
			}
		}

		winrt::com_ptr<ID3D11VertexShader> scaleCopyVS;
		winrt::com_ptr<ID3D11PixelShader> scaleCopyPS;
		winrt::com_ptr<ID3D11PixelShader> cropCopyPS;
		std::array<winrt::com_ptr<ID3D11PixelShader>, 3> scaleCopyMRTPS;
		winrt::com_ptr<ID3D11SamplerState> scaleCopySampler;
		winrt::com_ptr<ID3D11BlendState> scaleCopyBlendState;
		winrt::com_ptr<ID3D11DepthStencilState> scaleCopyDepthStencilState;
		winrt::com_ptr<ID3D11RasterizerState> scaleCopyRasterizerState;

		if (FAILED(a_device->CreateVertexShader(vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize(), nullptr, scaleCopyVS.put())) ||
			FAILED(a_device->CreatePixelShader(pixelShaderBlob->GetBufferPointer(), pixelShaderBlob->GetBufferSize(), nullptr, scaleCopyPS.put())) ||
			FAILED(a_device->CreatePixelShader(cropPixelShaderBlob->GetBufferPointer(), cropPixelShaderBlob->GetBufferSize(), nullptr, cropCopyPS.put()))) {
			logger::warn("[Upscaling] Failed to create ENB scale-copy shaders");
			return false;
		}
		for (std::size_t i = 0; i < mrtPixelShaderBlobs.size(); ++i) {
			if (mrtPixelShaderBlobs[i] && FAILED(a_device->CreatePixelShader(
					mrtPixelShaderBlobs[i]->GetBufferPointer(),
					mrtPixelShaderBlobs[i]->GetBufferSize(),
					nullptr,
					scaleCopyMRTPS[i].put()))) {
				logger::warn("[Upscaling] Failed to create ENB MRT scale-copy shader");
			}
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(a_device->CreateSamplerState(&samplerDesc, scaleCopySampler.put()))) {
			logger::warn("[Upscaling] Failed to create ENB scale-copy sampler");
			return false;
		}

		D3D11_BLEND_DESC blendDesc{};
		for (auto& target : blendDesc.RenderTarget) {
			target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		if (FAILED(a_device->CreateBlendState(&blendDesc, scaleCopyBlendState.put()))) {
			logger::warn("[Upscaling] Failed to create ENB scale-copy blend state");
			return false;
		}

		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = FALSE;
		depthDesc.StencilEnable = FALSE;
		if (FAILED(a_device->CreateDepthStencilState(&depthDesc, scaleCopyDepthStencilState.put()))) {
			logger::warn("[Upscaling] Failed to create ENB scale-copy depth state");
			return false;
		}

		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = TRUE;
		if (FAILED(a_device->CreateRasterizerState(&rasterizerDesc, scaleCopyRasterizerState.put()))) {
			logger::warn("[Upscaling] Failed to create ENB scale-copy rasterizer state");
			return false;
		}

		g_enbScaleCopyVS = std::move(scaleCopyVS);
		g_enbScaleCopyPS = std::move(scaleCopyPS);
		g_enbCropCopyPS = std::move(cropCopyPS);
		g_enbScaleCopyMRTPS = std::move(scaleCopyMRTPS);
		g_enbScaleCopySampler = std::move(scaleCopySampler);
		g_enbScaleCopyBlendState = std::move(scaleCopyBlendState);
		g_enbScaleCopyDepthStencilState = std::move(scaleCopyDepthStencilState);
		g_enbScaleCopyRasterizerState = std::move(scaleCopyRasterizerState);
		return true;
	}

	bool EnsureENBScaleCopyContextState(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context)
	{
		static ID3D11DeviceContext* unsupportedContext = nullptr;
		if (g_enbNativeContext1 && g_enbScaleCopyContextState &&
			g_enbNativeContext1.get() == a_context) {
			return true;
		}
		if (!a_device || !a_context || a_context != g_enbNativeContext.get()) {
			return false;
		}
		if (unsupportedContext == a_context) {
			return false;
		}

		g_enbScaleCopyContextStateInitialized = false;
		g_enbScaleCopyContextState = nullptr;
		g_enbNativeContext1 = nullptr;
		winrt::com_ptr<ID3D11Device1> device1;
		winrt::com_ptr<ID3D11DeviceContext1> context1;
		if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(device1.put()))) ||
			FAILED(a_context->QueryInterface(IID_PPV_ARGS(context1.put())))) {
			unsupportedContext = a_context;
			return false;
		}

		const auto featureLevel = a_device->GetFeatureLevel();
		D3D_FEATURE_LEVEL chosenFeatureLevel{};
		winrt::com_ptr<ID3DDeviceContextState> contextState;
		if (FAILED(device1->CreateDeviceContextState(
				0,
				&featureLevel,
				1,
				D3D11_SDK_VERSION,
				__uuidof(ID3D11DeviceContext1),
				&chosenFeatureLevel,
				contextState.put())) ||
			!contextState) {
			unsupportedContext = a_context;
			return false;
		}

		g_enbNativeContext1 = std::move(context1);
		g_enbScaleCopyContextState = std::move(contextState);
		return true;
	}

	struct ENBScaleCopyJob
	{
		ID3D11Texture2D* sourceTexture{ nullptr };
		ID3D11ShaderResourceView* sourceSRV{ nullptr };
		ID3D11RenderTargetView* destinationRTV{ nullptr };
		ID3D11Texture2D* destinationTexture{ nullptr };
		D3D11_TEXTURE2D_DESC destinationDesc{};
	};

	class ScopedENBProxyPromotionBoundary
	{
	public:
		explicit ScopedENBProxyPromotionBoundary(ID3D11DeviceContext* a_context) :
			context_(a_context)
		{
			if (!context_) {
				return;
			}

			context_->OMGetRenderTargets(
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
				savedRTVs_.data(),
				&savedDSV_);
			context_->PSGetShaderResources(
				0,
				kENBTrackedPSResourceCount,
				savedPSResources_.data());

			context_->PSSetShaderResources(
				0,
				kENBTrackedPSResourceCount,
				kNullD3D11ShaderResources.data());
			context_->OMSetRenderTargets(0, nullptr, nullptr);
			active_ = true;
		}

		~ScopedENBProxyPromotionBoundary()
		{
			if (!active_) {
				return;
			}

			// Preserve the original ENB-visible boundary ordering. In current ENB the
			// OMSetRenderTargets wrapper uses the 6 -> non-6 transition to hand off
			// its deferred composite, then rebuilds admission state from this final
			// bind. Internal promotion draws run on the native context between them.
			context_->PSSetShaderResources(
				0,
				kENBTrackedPSResourceCount,
				savedPSResources_.data());
			context_->OMSetRenderTargets(
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
				savedRTVs_.data(),
				savedDSV_);

			for (auto* srv : savedPSResources_) {
				if (srv) {
					srv->Release();
				}
			}
			for (auto* rtv : savedRTVs_) {
				if (rtv) {
					rtv->Release();
				}
			}
			if (savedDSV_) {
				savedDSV_->Release();
			}
		}

		ScopedENBProxyPromotionBoundary(const ScopedENBProxyPromotionBoundary&) = delete;
		ScopedENBProxyPromotionBoundary& operator=(const ScopedENBProxyPromotionBoundary&) = delete;

		bool IsActive() const { return active_; }

	private:
		ID3D11DeviceContext* context_{ nullptr };
		bool active_{ false };
		std::array<ID3D11ShaderResourceView*, kENBTrackedPSResourceCount> savedPSResources_{};
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> savedRTVs_{};
		ID3D11DepthStencilView* savedDSV_{ nullptr };
	};

	class ScopedENBScaleCopyState
	{
	public:
		ScopedENBScaleCopyState(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			UINT a_savedPSResourceCount = 1)
		{
			if (!a_context || !EnsureENBScaleCopyResources(a_device)) {
				return;
			}

			context_ = a_context;
			savedPSResourceCount_ = std::clamp<UINT>(
				a_savedPSResourceCount,
				1,
				D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
			if (EnsureENBScaleCopyContextState(a_device, a_context)) {
				g_enbNativeContext1->SwapDeviceContextState(
					g_enbScaleCopyContextState.get(),
					savedContextState_.put());
				usesContextState_ = savedContextState_ != nullptr;
			}
			if (!usesContextState_) {
				context_->VSGetShader(savedVS_.put(), nullptr, nullptr);
				context_->PSGetShader(savedPS_.put(), nullptr, nullptr);
				context_->GSGetShader(savedGS_.put(), nullptr, nullptr);
				context_->HSGetShader(savedHS_.put(), nullptr, nullptr);
				context_->DSGetShader(savedDS_.put(), nullptr, nullptr);
				context_->IAGetInputLayout(savedInputLayout_.put());
				context_->IAGetPrimitiveTopology(&savedTopology_);
				context_->PSGetSamplers(0, 1, savedSampler_.put());
				context_->PSGetShaderResources(0, savedPSResourceCount_, savedPSResources_.data());
				context_->OMGetRenderTargets(
					D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
					savedRTVs_.data(),
					&savedDSV_);
				context_->OMGetBlendState(savedBlendState_.put(), savedBlendFactor_, &savedSampleMask_);
				context_->OMGetDepthStencilState(savedDepthStencilState_.put(), &savedStencilRef_);
				context_->RSGetState(savedRasterizerState_.put());
				savedViewportCount_ = static_cast<UINT>(savedViewports_.size());
				context_->RSGetViewports(&savedViewportCount_, savedViewports_.data());
			}

			if (!usesContextState_ || !g_enbScaleCopyContextStateInitialized) {
				context_->PSSetShaderResources(0, savedPSResourceCount_, kNullD3D11ShaderResources.data());
				const float blendFactor[4]{};
				context_->OMSetBlendState(g_enbScaleCopyBlendState.get(), blendFactor, 0xffffffff);
				context_->OMSetDepthStencilState(g_enbScaleCopyDepthStencilState.get(), 0);
				context_->RSSetState(g_enbScaleCopyRasterizerState.get());
				context_->IASetInputLayout(nullptr);
				context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				context_->VSSetShader(g_enbScaleCopyVS.get(), nullptr, 0);
				context_->PSSetShader(g_enbScaleCopyPS.get(), nullptr, 0);
				context_->GSSetShader(nullptr, nullptr, 0);
				context_->HSSetShader(nullptr, nullptr, 0);
				context_->DSSetShader(nullptr, nullptr, 0);
				ID3D11SamplerState* sampler = g_enbScaleCopySampler.get();
				context_->PSSetSamplers(0, 1, &sampler);
			}
			active_ = true;
		}

		~ScopedENBScaleCopyState()
		{
			if (!active_) {
				return;
			}

			context_->PSSetShaderResources(0, savedPSResourceCount_, kNullD3D11ShaderResources.data());
			if (usesContextState_) {
				g_enbNativeContext1->SwapDeviceContextState(
					savedContextState_.get(),
					g_enbScaleCopyContextState.put());
				g_enbScaleCopyContextStateInitialized = g_enbScaleCopyContextState != nullptr;
				return;
			}

			context_->VSSetShader(savedVS_.get(), nullptr, 0);
			context_->PSSetShader(savedPS_.get(), nullptr, 0);
			context_->GSSetShader(savedGS_.get(), nullptr, 0);
			context_->HSSetShader(savedHS_.get(), nullptr, 0);
			context_->DSSetShader(savedDS_.get(), nullptr, 0);
			context_->IASetInputLayout(savedInputLayout_.get());
			context_->IASetPrimitiveTopology(savedTopology_);
			ID3D11SamplerState* sampler = savedSampler_.get();
			context_->PSSetSamplers(0, 1, &sampler);
			context_->OMSetBlendState(savedBlendState_.get(), savedBlendFactor_, savedSampleMask_);
			context_->OMSetDepthStencilState(savedDepthStencilState_.get(), savedStencilRef_);
			context_->RSSetState(savedRasterizerState_.get());
			if (savedViewportCount_ > 0) {
				context_->RSSetViewports(savedViewportCount_, savedViewports_.data());
			}
			context_->OMSetRenderTargets(
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
				savedRTVs_.data(),
				savedDSV_);
			context_->PSSetShaderResources(0, savedPSResourceCount_, savedPSResources_.data());

			for (UINT i = 0; i < savedPSResourceCount_; ++i) {
				if (savedPSResources_[i]) {
					savedPSResources_[i]->Release();
				}
			}
			for (auto* rtv : savedRTVs_) {
				if (rtv) {
					rtv->Release();
				}
			}
			if (savedDSV_) {
				savedDSV_->Release();
			}
		}

		ScopedENBScaleCopyState(const ScopedENBScaleCopyState&) = delete;
		ScopedENBScaleCopyState& operator=(const ScopedENBScaleCopyState&) = delete;

		bool IsActive() const { return active_; }
		bool UsesContextState() const { return usesContextState_; }

		bool Draw(
			ID3D11ShaderResourceView* a_sourceSRV,
			ID3D11RenderTargetView* a_destinationRTV,
			ID3D11Texture2D* a_destinationTexture,
			float a_outputWidth = 0.0f,
			float a_outputHeight = 0.0f)
		{
			if (!active_ || !a_sourceSRV || !a_destinationRTV || !a_destinationTexture) {
				return false;
			}

			D3D11_TEXTURE2D_DESC desc{};
			a_destinationTexture->GetDesc(&desc);
			const auto width = a_outputWidth > 0.0f ? a_outputWidth : static_cast<float>(desc.Width);
			const auto height = a_outputHeight > 0.0f ? a_outputHeight : static_cast<float>(desc.Height);
			if (!std::isfinite(width) || !std::isfinite(height) ||
				width <= 0.0f || height <= 0.0f ||
				width > static_cast<float>(desc.Width) || height > static_cast<float>(desc.Height)) {
				return false;
			}

			context_->OMSetRenderTargets(1, &a_destinationRTV, nullptr);
			SetViewport(width, height);
			context_->PSSetShaderResources(0, 1, &a_sourceSRV);
			context_->Draw(3, 0);
			context_->PSSetShaderResources(0, 1, kNullD3D11ShaderResources.data());
			return true;
		}

		bool DrawCrop(
			ID3D11ShaderResourceView* a_sourceSRV,
			ID3D11RenderTargetView* a_destinationRTV,
			ID3D11Texture2D* a_destinationTexture,
			float a_outputWidth = 0.0f,
			float a_outputHeight = 0.0f)
		{
			if (!active_ || !g_enbCropCopyPS) {
				return false;
			}

			context_->PSSetShader(g_enbCropCopyPS.get(), nullptr, 0);
			const bool result = Draw(
				a_sourceSRV,
				a_destinationRTV,
				a_destinationTexture,
				a_outputWidth,
				a_outputHeight);
			context_->PSSetShader(g_enbScaleCopyPS.get(), nullptr, 0);
			return result;
		}

		bool DrawMRT(const ENBScaleCopyJob* const* a_jobs, std::size_t a_count)
		{
			if (!active_ || !a_jobs ||
				a_count < 2 || a_count > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
				return false;
			}

			const auto& first = *a_jobs[0];
			if (!first.sourceSRV || !first.destinationRTV ||
				first.destinationDesc.Width == 0 || first.destinationDesc.Height == 0) {
				return false;
			}

			std::array<ID3D11ShaderResourceView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> sources{};
			std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> destinations{};
			for (std::size_t i = 0; i < a_count; ++i) {
				const auto& job = *a_jobs[i];
				if (!job.sourceSRV || !job.destinationRTV ||
					job.destinationDesc.Width != first.destinationDesc.Width ||
					job.destinationDesc.Height != first.destinationDesc.Height ||
					job.destinationDesc.SampleDesc.Count != first.destinationDesc.SampleDesc.Count ||
					job.destinationDesc.SampleDesc.Quality != first.destinationDesc.SampleDesc.Quality) {
					return false;
				}
				sources[i] = job.sourceSRV;
				destinations[i] = job.destinationRTV;
			}

			const auto shaderIndex = a_count <= 2 ? 0u : (a_count <= 4 ? 1u : 2u);
			if (!g_enbScaleCopyMRTPS[shaderIndex]) {
				return false;
			}
			context_->PSSetShader(g_enbScaleCopyMRTPS[shaderIndex].get(), nullptr, 0);
			context_->OMSetRenderTargets(static_cast<UINT>(a_count), destinations.data(), nullptr);
			SetViewport(
				static_cast<float>(first.destinationDesc.Width),
				static_cast<float>(first.destinationDesc.Height));
			context_->PSSetShaderResources(0, static_cast<UINT>(a_count), sources.data());
			context_->Draw(3, 0);
			context_->PSSetShaderResources(
				0,
				static_cast<UINT>(a_count),
				kNullD3D11ShaderResources.data());
			context_->PSSetShader(g_enbScaleCopyPS.get(), nullptr, 0);
			return true;
		}

	private:
		void SetViewport(float a_width, float a_height)
		{
			const D3D11_VIEWPORT viewport{
				0.0f,
				0.0f,
				a_width,
				a_height,
				0.0f,
				1.0f
			};
			context_->RSSetViewports(1, &viewport);
		}

		ID3D11DeviceContext* context_{ nullptr };
		bool active_{ false };
		bool usesContextState_{ false };
		UINT savedPSResourceCount_{ 1 };
		winrt::com_ptr<ID3DDeviceContextState> savedContextState_;
		winrt::com_ptr<ID3D11VertexShader> savedVS_;
		winrt::com_ptr<ID3D11PixelShader> savedPS_;
		winrt::com_ptr<ID3D11GeometryShader> savedGS_;
		winrt::com_ptr<ID3D11HullShader> savedHS_;
		winrt::com_ptr<ID3D11DomainShader> savedDS_;
		winrt::com_ptr<ID3D11InputLayout> savedInputLayout_;
		D3D11_PRIMITIVE_TOPOLOGY savedTopology_{};
		winrt::com_ptr<ID3D11SamplerState> savedSampler_;
		std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> savedPSResources_{};
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> savedRTVs_{};
		ID3D11DepthStencilView* savedDSV_{ nullptr };
		winrt::com_ptr<ID3D11BlendState> savedBlendState_;
		float savedBlendFactor_[4]{};
		UINT savedSampleMask_{ 0 };
		winrt::com_ptr<ID3D11DepthStencilState> savedDepthStencilState_;
		UINT savedStencilRef_{ 0 };
		winrt::com_ptr<ID3D11RasterizerState> savedRasterizerState_;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> savedViewports_{};
		UINT savedViewportCount_{ 0 };
	};

	bool ScaleCopyRenderTarget(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		ID3D11ShaderResourceView* a_sourceSRV,
		ID3D11RenderTargetView* a_destinationRTV,
		ID3D11Texture2D* a_destinationTexture,
		UINT a_outputWidth = 0,
		UINT a_outputHeight = 0)
	{
		auto* resourceDevice = a_device;
		auto* resourceContext = a_context;
		if (EnsureENBNativeD3D11Context(a_destinationTexture) &&
			CanBypassENBD3D11Context(a_context)) {
			// Helper copies do not participate in ENB's pass classification. Submit
			// them on the real context so every state transition avoids the wrapper
			// and can use the D3D11.1 context-state fast path.
			resourceDevice = g_enbNativeDevice.get();
			resourceContext = g_enbNativeContext.get();
		}
		ScopedENBScaleCopyState state(resourceDevice, resourceContext);
		return state.Draw(
			a_sourceSRV,
			a_destinationRTV,
			a_destinationTexture,
			static_cast<float>(a_outputWidth),
			static_cast<float>(a_outputHeight));
	}

	void SelectENBCopyDeviceContext(
		ID3D11Texture2D* a_referenceTexture,
		ID3D11Device*& a_device,
		ID3D11DeviceContext*& a_context)
	{
		static auto* rendererData = RE::BSGraphics::GetRendererData();
		a_device = rendererData ? reinterpret_cast<ID3D11Device*>(rendererData->device) : nullptr;
		a_context = rendererData ? reinterpret_cast<ID3D11DeviceContext*>(rendererData->context) : nullptr;
		if (a_referenceTexture && EnsureENBNativeD3D11Context(a_referenceTexture) &&
			CanBypassENBD3D11Context(a_context)) {
			a_device = g_enbNativeDevice.get();
			a_context = g_enbNativeContext.get();
		}
	}

	bool EnsureENBCompositeRenderTargetProxies(std::initializer_list<int> a_targets)
	{
		auto* upscaling = Upscaling::GetSingleton();
		auto* renderTargetManager = Util::RenderTargetManager_GetSingleton();
		static auto* rendererData = RE::BSGraphics::GetRendererData();
		if (!upscaling || !renderTargetManager || !rendererData) {
			return false;
		}

		const auto ratios = GetDynamicResolutionRatios();
		const auto widthRatio = ratios.width;
		const auto heightRatio = ratios.height;
		if (!(widthRatio > 0.0f && widthRatio < 1.0f) ||
			!(heightRatio > 0.0f && heightRatio < 1.0f)) {
			return false;
		}

		for (const auto target : a_targets) {
			if (target < 0 || target >= static_cast<int>(std::size(upscaling->proxyRenderTargets))) {
				return false;
			}

			auto& current = rendererData->renderTargets[target];
			auto& original = upscaling->originalRenderTargets[target];
			auto& proxy = upscaling->proxyRenderTargets[target];
			if (!current.texture) {
				return false;
			}

			// RT26/27/30/35 are deliberately not part of the global proxy set: only
			// these ENB composites need them. Refresh an on-demand proxy when the live
			// resource or its expected render dimensions change.
			bool needsUpdate = current.texture != proxy.texture &&
				(current.texture != original.texture || !proxy.texture);
			if (!needsUpdate && proxy.texture) {
				D3D11_TEXTURE2D_DESC currentDesc{};
				D3D11_TEXTURE2D_DESC proxyDesc{};
				reinterpret_cast<ID3D11Texture2D*>(current.texture)->GetDesc(&currentDesc);
				reinterpret_cast<ID3D11Texture2D*>(proxy.texture)->GetDesc(&proxyDesc);
				const auto expectedWidth = static_cast<UINT>(static_cast<float>(currentDesc.Width) * widthRatio);
				const auto expectedHeight = static_cast<UINT>(static_cast<float>(currentDesc.Height) * heightRatio);
				needsUpdate = proxyDesc.Width != expectedWidth || proxyDesc.Height != expectedHeight;
			}
			if (needsUpdate) {
				upscaling->UpdateRenderTarget(target, widthRatio, heightRatio);
			}

			if (!upscaling->originalRenderTargets[target].texture ||
				!upscaling->originalRenderTargets[target].srView ||
				!upscaling->proxyRenderTargets[target].texture ||
				!upscaling->proxyRenderTargets[target].rtView ||
				!upscaling->proxyRenderTargets[target].srView) {
				return false;
			}
		}
		return true;
	}

	struct ENBCompositeOutputs
	{
		bool target1{ false };
		bool target2{ false };

		bool Any() const { return target1 || target2; }
		int ReferenceTarget() const { return target1 ? 1 : (target2 ? 2 : -1); }
	};

	ENBCompositeOutputs FindBoundENBCompositeOutputs()
	{
		auto* upscaling = Upscaling::GetSingleton();
		static auto* rendererData = RE::BSGraphics::GetRendererData();
		auto* context = rendererData ? reinterpret_cast<ID3D11DeviceContext*>(rendererData->context) : nullptr;
		if (!upscaling || !context) {
			return {};
		}

		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> boundRTVs{};
		context->OMGetRenderTargets(static_cast<UINT>(boundRTVs.size()), boundRTVs.data(), nullptr);
		ENBCompositeOutputs result{};
		for (const auto candidate : { 1, 2 }) {
			auto* original = reinterpret_cast<ID3D11RenderTargetView*>(
				upscaling->originalRenderTargets[candidate].rtView);
			if (original && std::ranges::find(boundRTVs, original) != boundRTVs.end()) {
				if (candidate == 1) {
					result.target1 = true;
				} else {
					result.target2 = true;
				}
			}
		}
		for (auto* rtv : boundRTVs) {
			if (rtv) {
				rtv->Release();
			}
		}
		return result;
	}

	class ScopedENBCompositeRenderViewport
	{
	public:
		explicit ScopedENBCompositeRenderViewport(int a_outputTarget)
		{
			auto* upscaling = Upscaling::GetSingleton();
			static auto* rendererData = RE::BSGraphics::GetRendererData();
			context_ = rendererData ? reinterpret_cast<ID3D11DeviceContext*>(rendererData->context) : nullptr;
			auto* output = upscaling && a_outputTarget >= 0 ?
				reinterpret_cast<ID3D11Texture2D*>(upscaling->proxyRenderTargets[a_outputTarget].texture) : nullptr;
			if (!context_ || !output) {
				context_ = nullptr;
				return;
			}

			viewportCount_ = static_cast<UINT>(viewports_.size());
			context_->RSGetViewports(&viewportCount_, viewports_.data());
			if (viewportCount_ == 0) {
				context_ = nullptr;
				return;
			}

			D3D11_TEXTURE2D_DESC desc{};
			output->GetDesc(&desc);
			if (desc.Width == 0 || desc.Height == 0) {
				context_ = nullptr;
				return;
			}

			width_ = static_cast<float>(desc.Width);
			height_ = static_cast<float>(desc.Height);
			auto viewport = viewports_[0];
			viewport.TopLeftX = 0.0f;
			viewport.TopLeftY = 0.0f;
			viewport.Width = width_;
			viewport.Height = height_;
			context_->RSSetViewports(1, &viewport);
			active_ = true;
		}

		~ScopedENBCompositeRenderViewport()
		{
			if (active_) {
				context_->RSSetViewports(viewportCount_, viewports_.data());
			}
		}

		ScopedENBCompositeRenderViewport(const ScopedENBCompositeRenderViewport&) = delete;
		ScopedENBCompositeRenderViewport& operator=(const ScopedENBCompositeRenderViewport&) = delete;

		bool IsActive() const { return active_; }
		float Width() const { return width_; }
		float Height() const { return height_; }

	private:
		ID3D11DeviceContext* context_{ nullptr };
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
		UINT viewportCount_{ 0 };
		float width_{ 0.0f };
		float height_{ 0.0f };
		bool active_{ false };
	};

	bool CropENBCompositeTargetsToRenderDomain(std::initializer_list<int> a_targets)
	{
		auto* upscaling = Upscaling::GetSingleton();
		if (!upscaling || a_targets.size() == 0) {
			return false;
		}

		ID3D11Texture2D* reference = nullptr;
		for (const auto target : a_targets) {
			reference = reinterpret_cast<ID3D11Texture2D*>(
				upscaling->proxyRenderTargets[target].texture);
			if (reference) {
				break;
			}
		}

		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		SelectENBCopyDeviceContext(reference, device, context);
		ScopedENBScaleCopyState state(device, context, 16);
		if (!state.IsActive()) {
			return false;
		}

		for (const auto target : a_targets) {
			auto* sourceTexture = reinterpret_cast<ID3D11Texture2D*>(
				upscaling->originalRenderTargets[target].texture);
			auto* sourceSRV = reinterpret_cast<ID3D11ShaderResourceView*>(
				upscaling->originalRenderTargets[target].srView);
			auto* destinationTexture = reinterpret_cast<ID3D11Texture2D*>(
				upscaling->proxyRenderTargets[target].texture);
			auto* destinationRTV = reinterpret_cast<ID3D11RenderTargetView*>(
				upscaling->proxyRenderTargets[target].rtView);
			if (!sourceTexture || !sourceSRV || !destinationTexture || !destinationRTV) {
				return false;
			}

			D3D11_TEXTURE2D_DESC sourceDesc{};
			D3D11_TEXTURE2D_DESC destinationDesc{};
			D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc{};
			D3D11_RENDER_TARGET_VIEW_DESC destinationViewDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			destinationTexture->GetDesc(&destinationDesc);
			sourceSRV->GetDesc(&sourceViewDesc);
			destinationRTV->GetDesc(&destinationViewDesc);
			if (sourceViewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
				destinationViewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
				sourceDesc.SampleDesc.Count != 1 || destinationDesc.SampleDesc.Count != 1 ||
				destinationDesc.Width > sourceDesc.Width || destinationDesc.Height > sourceDesc.Height ||
				!state.DrawCrop(sourceSRV, destinationRTV, destinationTexture)) {
				return false;
			}
		}
		return true;
	}

	bool CopyENBCompositeOutputToNativeSubrect(int a_target)
	{
		auto* upscaling = Upscaling::GetSingleton();
		if (!upscaling || a_target < 0 ||
			a_target >= static_cast<int>(std::size(upscaling->proxyRenderTargets))) {
			return false;
		}

		auto* sourceTexture = reinterpret_cast<ID3D11Texture2D*>(
			upscaling->proxyRenderTargets[a_target].texture);
		auto* sourceSRV = reinterpret_cast<ID3D11ShaderResourceView*>(
			upscaling->proxyRenderTargets[a_target].srView);
		auto* destinationTexture = reinterpret_cast<ID3D11Texture2D*>(
			upscaling->originalRenderTargets[a_target].texture);
		auto* destinationRTV = reinterpret_cast<ID3D11RenderTargetView*>(
			upscaling->originalRenderTargets[a_target].rtView);
		if (!sourceTexture || !sourceSRV || !destinationTexture || !destinationRTV) {
			return false;
		}

		D3D11_TEXTURE2D_DESC sourceDesc{};
		D3D11_TEXTURE2D_DESC destinationDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC destinationViewDesc{};
		sourceTexture->GetDesc(&sourceDesc);
		destinationTexture->GetDesc(&destinationDesc);
		sourceSRV->GetDesc(&sourceViewDesc);
		destinationRTV->GetDesc(&destinationViewDesc);
		if (sourceViewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
			destinationViewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
			sourceDesc.SampleDesc.Count != 1 || destinationDesc.SampleDesc.Count != 1 ||
			sourceDesc.Width > destinationDesc.Width || sourceDesc.Height > destinationDesc.Height) {
			return false;
		}

		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		SelectENBCopyDeviceContext(destinationTexture, device, context);
		ScopedENBScaleCopyState state(device, context, 16);
		return state.DrawCrop(
			sourceSRV,
			destinationRTV,
			destinationTexture,
			static_cast<float>(sourceDesc.Width),
			static_cast<float>(sourceDesc.Height));
	}

	void FinishENBCompositeTargets(
		std::initializer_list<int> a_targets,
		const ENBCompositeOutputs& a_outputs)
	{
		auto* upscaling = Upscaling::GetSingleton();
		if (!upscaling) {
			return;
		}

		const bool fallbackTarget1 =
			a_outputs.target1 && !CopyENBCompositeOutputToNativeSubrect(1);
		const bool fallbackTarget2 =
			a_outputs.target2 && !CopyENBCompositeOutputToNativeSubrect(2);
		if (fallbackTarget1 && fallbackTarget2) {
			upscaling->ResetRenderTargetsSelective(a_targets, { 1, 2 });
		} else if (fallbackTarget1) {
			upscaling->ResetRenderTargetsSelective(a_targets, { 1 });
		} else if (fallbackTarget2) {
			upscaling->ResetRenderTargetsSelective(a_targets, { 2 });
		} else {
			upscaling->ResetRenderTargetsSelective(a_targets);
		}
	}

	std::uintptr_t FindLiveENBPrepass(HMODULE a_module)
	{
		if (!a_module) {
			return 0;
		}

		constexpr std::array<std::uint8_t, 50> signature{
			0x48, 0x8B, 0xC4,
			0x44, 0x89, 0x48, 0x20,
			0x44, 0x89, 0x40, 0x18,
			0x89, 0x50, 0x10,
			0x55, 0x56, 0x57,
			0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
			0x48, 0x81, 0xEC, 0xE0, 0x04, 0x00, 0x00,
			0x48, 0xC7, 0x44, 0x24, 0x70, 0xFE, 0xFF, 0xFF, 0xFF,
			0x48, 0x89, 0x58, 0x08,
			0x4C, 0x8B, 0xF1,
			0x83, 0x3D
		};

		const auto base = reinterpret_cast<std::uintptr_t>(a_module);
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return 0;
		}
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			return 0;
		}

		const auto section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
				continue;
			}

			const auto sectionBegin = reinterpret_cast<const std::uint8_t*>(base + section[i].VirtualAddress);
			const auto sectionSize = static_cast<std::size_t>(section[i].Misc.VirtualSize);
			if (sectionSize < signature.size()) {
				continue;
			}
			const auto match = std::search(
				sectionBegin,
				sectionBegin + sectionSize,
				signature.begin(),
				signature.end());
			if (match != sectionBegin + sectionSize) {
				return reinterpret_cast<std::uintptr_t>(match);
			}
		}
		return 0;
	}

	std::uintptr_t FindENBModuleString(HMODULE a_module, std::string_view a_value)
	{
		if (!a_module || a_value.empty()) {
			return 0;
		}

		const auto base = reinterpret_cast<std::uintptr_t>(a_module);
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return 0;
		}
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			return 0;
		}

		const auto section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			const auto sectionBegin = reinterpret_cast<const char*>(base + section[i].VirtualAddress);
			const auto sectionSize = static_cast<std::size_t>(section[i].Misc.VirtualSize);
			if (sectionSize < a_value.size()) {
				continue;
			}

			const auto match = std::search(
				sectionBegin,
				sectionBegin + sectionSize,
				a_value.begin(),
				a_value.end());
			if (match != sectionBegin + sectionSize) {
				return reinterpret_cast<std::uintptr_t>(match);
			}
		}

		return 0;
	}

	bool ENBModuleContainsString(HMODULE a_module, std::string_view a_value)
	{
		return FindENBModuleString(a_module, a_value) != 0;
	}

	std::uintptr_t ResolveENBRIPRelativeAddress(const std::uint8_t* a_instruction)
	{
		std::int32_t displacement = 0;
		std::memcpy(&displacement, a_instruction + 3, sizeof(displacement));
		return reinterpret_cast<std::uintptr_t>(a_instruction) + 7 + displacement;
	}

	std::uintptr_t ResolveENBRIPRelativeAddress(
		const std::uint8_t* a_instruction,
		std::size_t a_displacementOffset,
		std::size_t a_instructionSize)
	{
		std::int32_t displacement = 0;
		std::memcpy(&displacement, a_instruction + a_displacementOffset, sizeof(displacement));
		return reinterpret_cast<std::uintptr_t>(a_instruction) + a_instructionSize + displacement;
	}

	struct ENBCodeRange
	{
		const std::uint8_t* begin;
		std::size_t size;
	};

	std::optional<ENBCodeRange> GetENBExecutableCode(HMODULE a_module)
	{
		if (!a_module) {
			return std::nullopt;
		}

		const auto base = reinterpret_cast<std::uintptr_t>(a_module);
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return std::nullopt;
		}
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			return std::nullopt;
		}

		ENBCodeRange result{};
		const auto sections = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			const auto size = static_cast<std::size_t>(sections[i].Misc.VirtualSize);
			if ((sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && size > result.size) {
				result = {
					reinterpret_cast<const std::uint8_t*>(base + sections[i].VirtualAddress), size
				};
			}
		}
		return result.begin ? std::optional{ result } : std::nullopt;
	}

	std::uintptr_t FindENBSettingStorage(
		HMODULE a_module,
		const ENBCodeRange& a_code,
		std::string_view a_setting)
	{
		const auto settingName = FindENBModuleString(a_module, a_setting);
		if (!settingName) {
			return 0;
		}

		for (std::size_t offset = 0; offset + 14 <= a_code.size; ++offset) {
			const auto instruction = a_code.begin + offset;
			// ENB registers booleans as: lea r9, storage; lea r8, setting name.
			if (instruction[0] == 0x4C && instruction[1] == 0x8D && instruction[2] == 0x0D &&
				instruction[7] == 0x4C && instruction[8] == 0x8D && instruction[9] == 0x05 &&
				ResolveENBRIPRelativeAddress(instruction + 7) == settingName) {
				return ResolveENBRIPRelativeAddress(instruction);
			}
		}
		return 0;
	}

	bool HasENBViewportCall(const std::uint8_t* a_begin, const std::uint8_t* a_end)
	{
		for (auto instruction = a_begin; instruction + 7 <= a_end; ++instruction) {
			const auto opcodeOffset = (instruction[0] & 0xF0) == 0x40 ? 1u : 0u;
			std::int32_t vtableOffset = 0;
			std::memcpy(&vtableOffset, instruction + opcodeOffset + 2, sizeof(vtableOffset));
			if (instruction[opcodeOffset] == 0xFF &&
				(instruction[opcodeOffset + 1] & 0xF8) == 0x90 && vtableOffset == 0x160) {
				return true;
			}
		}
		return false;
	}

	bool IsWritableProcessAddress(const void* a_address)
	{
		MEMORY_BASIC_INFORMATION memory{};
		if (!a_address || VirtualQuery(a_address, &memory, sizeof(memory)) != sizeof(memory) ||
			memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
			return false;
		}

		const auto protection = memory.Protect & 0xFF;
		return protection == PAGE_READWRITE ||
			protection == PAGE_WRITECOPY ||
			protection == PAGE_EXECUTE_READWRITE ||
			protection == PAGE_EXECUTE_WRITECOPY;
	}

	bool IsReadableProcessRange(const void* a_address, std::size_t a_size)
	{
		if (!a_address || a_size == 0) {
			return false;
		}

		const auto begin = reinterpret_cast<std::uintptr_t>(a_address);
		if (begin > std::numeric_limits<std::uintptr_t>::max() - a_size) {
			return false;
		}
		const auto end = begin + a_size;
		auto current = begin;
		while (current < end) {
			MEMORY_BASIC_INFORMATION memory{};
			if (VirtualQuery(reinterpret_cast<const void*>(current), &memory, sizeof(memory)) !=
					sizeof(memory) ||
				memory.State != MEM_COMMIT ||
				(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
				return false;
			}

			const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
			if (memory.RegionSize > std::numeric_limits<std::uintptr_t>::max() - regionBegin) {
				return false;
			}
			const auto regionEnd = regionBegin + memory.RegionSize;
			if (current < regionBegin || regionEnd <= current) {
				return false;
			}
			current = std::min(end, regionEnd);
		}
		return true;
	}

	bool TryResolveENBRIPRelativeLEA(
		const std::uint8_t* a_instruction,
		std::uintptr_t& a_target)
	{
		if (a_instruction[0] < 0x48 || a_instruction[0] > 0x4F ||
			a_instruction[1] != 0x8D || (a_instruction[2] & 0xC7) != 0x05) {
			return false;
		}
		a_target = ResolveENBRIPRelativeAddress(a_instruction, 3, 7);
		return true;
	}

	bool IsENBModuleStringAt(
		HMODULE a_module,
		std::uintptr_t a_address,
		std::string_view a_value)
	{
		if (!a_module || a_value.empty()) {
			return false;
		}

		const auto base = reinterpret_cast<std::uintptr_t>(a_module);
		const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return false;
		}
		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto imageEnd = base + nt->OptionalHeader.SizeOfImage;
		if (a_address < base || a_address + a_value.size() >= imageEnd) {
			return false;
		}

		const auto* value = reinterpret_cast<const char*>(a_address);
		return std::equal(a_value.begin(), a_value.end(), value) &&
			value[a_value.size()] == '\0';
	}

	void ResolveENBRenderResolutionGlobals()
	{
		if (!enbLoaded || g_enbFullWidth || g_enbFullHeight) {
			return;
		}

		const auto module = FindENBModule();
		const auto code = GetENBExecutableCode(module);
		if (!code) {
			logger::warn("[ENB SR] Cannot inspect ENB executable code for resolution globals");
			return;
		}
		const auto detailedShadow = FindENBSettingStorage(module, *code, "EnableDetailedShadow");
		if (!detailedShadow) {
			// Older ENB builds (including 0.307) do not implement this pass.
			return;
		}

		for (std::size_t offset = 0; offset + 7 <= code->size; ++offset) {
			const auto settingRead = code->begin + offset;
			if (settingRead[0] != 0x83 || settingRead[1] != 0x3D || settingRead[6] != 0x01 ||
				ResolveENBRIPRelativeAddress(settingRead, 2, 7) != detailedShadow) {
				continue;
			}

			const auto scanEnd = std::min(code->size, offset + 0x200);
			for (std::size_t cursor = offset + 7; cursor + 13 <= scanEnd; ++cursor) {
				const auto load = code->begin + cursor;
				std::uintptr_t first = 0;
				std::uintptr_t second = 0;
				const auto twoPlainLoads = load[0] == 0x8B && load[1] == 0x15 && load[6] == 0x8B && load[7] == 0x05;
				const auto rexSecondLoad = load[0] == 0x8B && load[1] == 0x05 &&
					load[6] == 0x44 && load[7] == 0x8B && load[8] == 0x1D;
				if (twoPlainLoads) {
					first = ResolveENBRIPRelativeAddress(load, 2, 6);
					second = ResolveENBRIPRelativeAddress(load + 6, 2, 6);
				} else if (rexSecondLoad) {
					first = ResolveENBRIPRelativeAddress(load, 2, 6);
					second = ResolveENBRIPRelativeAddress(load + 6, 3, 7);
				} else {
					continue;
				}

				const auto lower = std::min(first, second);
				const auto upper = std::max(first, second);
				if (upper - lower != sizeof(std::uint32_t) ||
					!HasENBViewportCall(load + (twoPlainLoads ? 12 : 13), code->begin + scanEnd) ||
					!IsWritableProcessAddress(reinterpret_cast<const void*>(lower)) ||
					!IsWritableProcessAddress(reinterpret_cast<const void*>(upper))) {
					continue;
				}

				auto* fullWidth = reinterpret_cast<std::uint32_t*>(lower);
				auto* fullHeight = reinterpret_cast<std::uint32_t*>(upper);
				auto* halfWidthA = reinterpret_cast<std::uint32_t*>(lower + 0x38);
				auto* halfHeightA = reinterpret_cast<std::uint32_t*>(lower + 0x3C);
				auto* halfWidthB = reinterpret_cast<std::uint32_t*>(lower + 0x40);
				auto* halfHeightB = reinterpret_cast<std::uint32_t*>(lower + 0x44);
				if (!IsWritableProcessAddress(halfWidthA) || !IsWritableProcessAddress(halfHeightA) ||
					!IsWritableProcessAddress(halfWidthB) || !IsWritableProcessAddress(halfHeightB)) {
					continue;
				}

				g_enbFullWidth = fullWidth;
				g_enbFullHeight = fullHeight;
				return;
			}
		}

		logger::warn("[ENB SR] Failed to resolve a coherent full/half ENB resolution family");
	}

	std::uintptr_t FindENBSSAORenderEntry(
		const ENBCodeRange& a_code,
		std::uintptr_t a_ssaoSetting)
	{
		if (!a_ssaoSetting) {
			return 0;
		}

		// The renderer's stack frame grew from 0x130 (0.307), through 0x1C0
		// (0.420), to 0x1F0 (0.496). The stable contract is its push-rdi /
		// large-frame prologue followed immediately by the EnableSSAO gate.
		constexpr std::array<std::uint8_t, 5> kEntryPrefix{
			0x40, 0x57, 0x48, 0x81, 0xEC  // push rdi; sub rsp, imm32
		};
		for (std::size_t offset = 0; offset + 16 <= a_code.size; ++offset) {
			const auto entry = a_code.begin + offset;
			if (!std::equal(kEntryPrefix.begin(), kEntryPrefix.end(), entry)) {
				continue;
			}

			std::uint32_t stackSize = 0;
			std::memcpy(&stackSize, entry + 5, sizeof(stackSize));
			if (stackSize < 0x100 || stackSize > 0x400 || (stackSize & 0xF) != 0) {
				continue;
			}

			const auto settingRead = entry + 9;
			if (settingRead[0] == 0x83 && settingRead[1] == 0x3D && settingRead[6] == 0x00 &&
				ResolveENBRIPRelativeAddress(settingRead, 2, 7) == a_ssaoSetting) {
				return reinterpret_cast<std::uintptr_t>(entry);
			}
		}
		return 0;
	}

	std::uintptr_t FindENBSSAOCompositeWrapper(
		const ENBCodeRange& a_code,
		std::uintptr_t a_ssaoEntry)
	{
		if (!a_ssaoEntry) {
			return 0;
		}

		// The BSDF-composite wrapper calls the SSAO renderer directly. Its first
		// predicate is the target-compatibility flag written by ENB's
		// OMSetRenderTargets hook.
		constexpr std::array<std::uint8_t, 15> kWrapperPrefix{
			0x48, 0x89, 0x5C, 0x24, 0x20,       // mov [rsp+20h], rbx
			0x55, 0x56, 0x57,                   // push rbp/rsi/rdi
			0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00  // sub rsp, 0xC0
		};
		constexpr std::array<std::uint8_t, 23> kLegacyWrapperPrefix{
			0x48, 0x89, 0x5C, 0x24, 0x10,       // mov [rsp+10h], rbx
			0x48, 0x89, 0x6C, 0x24, 0x18,       // mov [rsp+18h], rbp
			0x48, 0x89, 0x74, 0x24, 0x20,       // mov [rsp+20h], rsi
			0x57,                               // push rdi
			0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00  // sub rsp, 0x80
		};

		for (std::size_t callOffset = 0; callOffset + 5 <= a_code.size; ++callOffset) {
			const auto call = a_code.begin + callOffset;
			if (call[0] != 0xE8 ||
				ResolveENBRIPRelativeAddress(call, 1, 5) != a_ssaoEntry) {
				continue;
			}

			const auto scanBegin = callOffset > 0x400 ? callOffset - 0x400 : 0;
			for (std::size_t wrapperOffset = callOffset; wrapperOffset-- > scanBegin;) {
				const auto wrapper = a_code.begin + wrapperOffset;
				std::size_t prefixSize = 0;
				if (wrapperOffset + kWrapperPrefix.size() + 7 <= a_code.size &&
					std::equal(kWrapperPrefix.begin(), kWrapperPrefix.end(), wrapper)) {
					prefixSize = kWrapperPrefix.size();
				} else if (wrapperOffset + kLegacyWrapperPrefix.size() + 7 <= a_code.size &&
					std::equal(kLegacyWrapperPrefix.begin(), kLegacyWrapperPrefix.end(), wrapper)) {
					prefixSize = kLegacyWrapperPrefix.size();
				} else {
					continue;
				}

				const auto gateRead = wrapper + prefixSize;
				if (gateRead[0] != 0x83 || gateRead[1] != 0x3D || gateRead[6] != 0x00) {
					continue;
				}
				return reinterpret_cast<std::uintptr_t>(wrapper);
			}
		}
		return 0;
	}

	std::uintptr_t FindENBDeferMixDrawCallAddress(
		const ENBCodeRange& a_code,
		std::uintptr_t a_compositeWrapper)
	{
		// The post-SSAO handoff is the wrapper's only Draw(6, 0):
		//   mov rcx,[rdi+6C20h]
		//   xor r8d,r8d
		//   mov rax,[rcx]
		//   lea edx,[r8+6]
		//   call qword ptr [rax+68h]  ; ID3D11DeviceContext::Draw
		constexpr std::array<std::uint8_t, 13> kDeferMixDrawTail{
			0x45, 0x33, 0xC0,
			0x48, 0x8B, 0x01,
			0x41, 0x8D, 0x50, 0x06,
			0xFF, 0x50, 0x68
		};

		const auto codeBegin = reinterpret_cast<std::uintptr_t>(a_code.begin);
		const auto codeEnd = codeBegin + a_code.size;
		if (a_compositeWrapper < codeBegin || a_compositeWrapper >= codeEnd) {
			return 0;
		}

		const auto scanEnd = std::min(codeEnd, a_compositeWrapper + 0x1000);
		for (auto cursor = a_compositeWrapper;
			cursor + 20 <= scanEnd;
			++cursor) {
			const auto* instruction = reinterpret_cast<const std::uint8_t*>(cursor);
			// 0.307 keeps the wrapper object in rbp; later builds use rdi.
			// Accept any non-SIB base register while preserving rcx and +0x6C20.
			if (instruction[0] == 0x48 && instruction[1] == 0x8B &&
				(instruction[2] & 0xF8) == 0x88 && instruction[2] != 0x8C &&
				instruction[3] == 0x20 && instruction[4] == 0x6C &&
				instruction[5] == 0x00 && instruction[6] == 0x00 &&
				std::equal(kDeferMixDrawTail.begin(), kDeferMixDrawTail.end(), instruction + 7)) {
				// Return the address of `call qword ptr [rax+68h]`, not the
				// instruction after it. The following `inc esi` is deliberately
				// included in the five-byte call-site replacement.
				return cursor + 17;
			}
		}
		return 0;
	}


	std::uintptr_t FindENBDetailedShadowRenderEntry(
		const ENBCodeRange& a_code,
		std::uintptr_t a_detailedShadowSetting)
	{
		if (!a_detailedShadowSetting) {
			return 0;
		}

		constexpr std::array<std::uint8_t, 7> kEntryPrefix{
			0x48, 0x8B, 0xC4,                   // mov rax, rsp
			0x48, 0x89, 0x58, 0x20              // mov [rax+0x20], rbx
		};
		constexpr std::array<std::uint8_t, 7> kStackFrame{
			0x48, 0x81, 0xEC, 0x30, 0x04, 0x00, 0x00  // sub rsp, 0x430
		};
		for (std::size_t offset = 0; offset + 32 <= a_code.size; ++offset) {
			const auto entry = a_code.begin + offset;
			if (!std::equal(kEntryPrefix.begin(), kEntryPrefix.end(), entry)) {
				continue;
			}

			// Register allocation changed between 0.420 and 0.496. Skip the
			// one- and two-byte nonvolatile push sequence and anchor the frame.
			std::size_t cursor = kEntryPrefix.size();
			std::size_t pushCount = 0;
			while (cursor < 24 && pushCount < 8) {
				if (entry[cursor] >= 0x50 && entry[cursor] <= 0x57) {
					++cursor;
					++pushCount;
				} else if (entry[cursor] == 0x41 && entry[cursor + 1] >= 0x50 && entry[cursor + 1] <= 0x57) {
					cursor += 2;
					++pushCount;
				} else {
					break;
				}
			}
			if (pushCount < 4 || !std::equal(kStackFrame.begin(), kStackFrame.end(), entry + cursor)) {
				continue;
			}

			const auto scanEnd = std::min(a_code.size, offset + 0x500);
			for (std::size_t scan = offset + cursor + kStackFrame.size(); scan + 7 <= scanEnd; ++scan) {
				const auto settingRead = a_code.begin + scan;
				if (settingRead[0] == 0x83 && settingRead[1] == 0x3D && settingRead[6] == 0x01 &&
					ResolveENBRIPRelativeAddress(settingRead, 2, 7) == a_detailedShadowSetting) {
					return reinterpret_cast<std::uintptr_t>(entry);
				}
			}
		}
		return 0;
	}

	std::optional<ENBDetailedShadowBufferLayout> FindENBDetailedShadowBufferLayout(
		const ENBCodeRange& a_code,
		std::uintptr_t a_detailedShadowEntry)
	{
		const auto codeBegin = reinterpret_cast<std::uintptr_t>(a_code.begin);
		const auto codeEnd = codeBegin + a_code.size;
		if (a_detailedShadowEntry < codeBegin || a_detailedShadowEntry >= codeEnd) {
			return std::nullopt;
		}

		// DetailedShadow builds Matrix03 from the projection constant list at
		// a_this+0x98. The list's CPU mirror moved from +0x28 in 0.420 to +0x30
		// in 0.496. Match the stable two-load chain instead of a version number:
		//   mov reg,[nonvolatile+98h]; test reg,reg; jz ...; mov reg,[reg+28h/30h]
		const auto* scanBegin = reinterpret_cast<const std::uint8_t*>(a_detailedShadowEntry);
		const auto* scanEnd = reinterpret_cast<const std::uint8_t*>(
			std::min(codeEnd, a_detailedShadowEntry + 0x1000));
		for (auto cursor = scanBegin; cursor + 16 <= scanEnd; ++cursor) {
			if ((cursor[0] & 0xF8) != 0x48 || cursor[1] != 0x8B ||
				(cursor[2] & 0xC0) != 0x80 || (cursor[2] & 7) == 4) {
				continue;
			}

			std::uint32_t displacement = 0;
			std::memcpy(&displacement, cursor + 3, sizeof(displacement));
			if (displacement != 0x98) {
				continue;
			}

			const auto loadedRegister = static_cast<std::uint8_t>(
				((cursor[0] & 0x04) ? 8 : 0) | ((cursor[2] >> 3) & 7));
			const auto* chainEnd = std::min(scanEnd, cursor + 24);
			for (auto load = cursor + 7; load + 4 <= chainEnd; ++load) {
				if ((load[0] & 0xF8) != 0x48 || load[1] != 0x8B ||
					(load[2] & 0xC0) != 0x40 || (load[2] & 7) == 4) {
					continue;
				}

				const auto destination = static_cast<std::uint8_t>(
					((load[0] & 0x04) ? 8 : 0) | ((load[2] >> 3) & 7));
				const auto source = static_cast<std::uint8_t>(
					((load[0] & 0x01) ? 8 : 0) | (load[2] & 7));
				const auto cpuDataOffset = static_cast<std::size_t>(load[3]);
				if (destination == loadedRegister && source == loadedRegister &&
					(cpuDataOffset == 0x28 || cpuDataOffset == 0x30)) {
					return ENBDetailedShadowBufferLayout{
						cpuDataOffset, cpuDataOffset + 0x10
					};
				}
			}
		}
		return std::nullopt;
	}

	std::uintptr_t FindENBDepthOfFieldRenderEntry(
		const ENBCodeRange& a_code,
		std::uintptr_t a_depthOfFieldSetting)
	{
		constexpr std::array<std::uint8_t, 32> kEntryPrefix{
			0x48, 0x8B, 0xC4,                   // mov rax,rsp
			0x44, 0x89, 0x48, 0x20,             // mov [rax+20h],r9d
			0x44, 0x89, 0x40, 0x18,             // mov [rax+18h],r8d
			0x89, 0x50, 0x10,                   // mov [rax+10h],edx
			0x55, 0x56, 0x57,                   // push rbp/rsi/rdi
			0x41, 0x54, 0x41, 0x55,             // push r12/r13
			0x41, 0x56, 0x41, 0x57,             // push r14/r15
			0x48, 0x81, 0xEC, 0xD0, 0x05, 0x00, 0x00  // sub rsp,5D0h
		};
		for (std::size_t offset = 0; offset + kEntryPrefix.size() <= a_code.size; ++offset) {
			const auto entry = a_code.begin + offset;
			if (!std::equal(kEntryPrefix.begin(), kEntryPrefix.end(), entry)) {
				continue;
			}

			const auto scanEnd = std::min(a_code.size, offset + 0x100);
			for (std::size_t cursor = offset + kEntryPrefix.size(); cursor + 7 <= scanEnd; ++cursor) {
				const auto settingRead = a_code.begin + cursor;
				if (settingRead[0] == 0x83 && settingRead[1] == 0x3D && settingRead[6] == 0x00 &&
					ResolveENBRIPRelativeAddress(settingRead, 2, 7) == a_depthOfFieldSetting) {
					return reinterpret_cast<std::uintptr_t>(entry);
				}
			}
		}
		return 0;
	}

	ID3D11ShaderResourceView** FindENBDepthTextureSRVSlot(
		std::uintptr_t a_depthOfFieldEntry,
		std::uintptr_t a_textureDepthName)
	{
		if (!a_depthOfFieldEntry || !a_textureDepthName) {
			return nullptr;
		}

		const auto* function = reinterpret_cast<const std::uint8_t*>(a_depthOfFieldEntry);
		constexpr std::size_t kBindingScanSize = 0x400;
		for (std::size_t offset = 0; offset + 14 <= kBindingScanSize; ++offset) {
			const auto load = function + offset;
			// mov r8,[depth SRV]; lea rdx,"TextureDepth"
			if (load[0] != 0x4C || load[1] != 0x8B || load[2] != 0x05 ||
				load[7] != 0x48 || load[8] != 0x8D || load[9] != 0x15 ||
				ResolveENBRIPRelativeAddress(load + 7, 3, 7) != a_textureDepthName) {
				continue;
			}

			auto** slot = reinterpret_cast<ID3D11ShaderResourceView**>(
				ResolveENBRIPRelativeAddress(load, 3, 7));
			if (IsWritableProcessAddress(slot)) {
				return slot;
			}
		}
		return nullptr;
	}

	bool ShouldUseENBProxyCompatibility()
	{
		auto upscaling = Upscaling::GetSingleton();
		auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		return upscaling && renderTargetManager && IsDynamicResolutionScaled() &&
			IsENBSRCompatibilityActive(upscaling->upscaleMethod);
	}

	bool GetENBRenderTargetDimensions(
		ID3D11RenderTargetView* a_view,
		UINT& a_width,
		UINT& a_height)
	{
		if (!a_view) {
			return false;
		}

		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put())))) {
			return false;
		}

		D3D11_TEXTURE2D_DESC textureDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
		texture->GetDesc(&textureDesc);
		a_view->GetDesc(&viewDesc);
		UINT mipSlice = 0;
		switch (viewDesc.ViewDimension) {
		case D3D11_RTV_DIMENSION_TEXTURE2D:
			mipSlice = viewDesc.Texture2D.MipSlice;
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
			mipSlice = viewDesc.Texture2DArray.MipSlice;
			break;
		default:
			break;
		}
		mipSlice = std::min(mipSlice, 31u);

		a_width = std::max(1u, textureDesc.Width >> mipSlice);
		a_height = std::max(1u, textureDesc.Height >> mipSlice);
		return true;
	}

	bool GetENBShaderResourceDimensions(
		ID3D11ShaderResourceView* a_view,
		UINT& a_width,
		UINT& a_height)
	{
		if (!a_view) {
			return false;
		}

		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put())))) {
			return false;
		}

		D3D11_TEXTURE2D_DESC textureDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		texture->GetDesc(&textureDesc);
		a_view->GetDesc(&viewDesc);
		UINT mipSlice = 0;
		switch (viewDesc.ViewDimension) {
		case D3D11_SRV_DIMENSION_TEXTURE2D:
			mipSlice = viewDesc.Texture2D.MostDetailedMip;
			break;
		case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
			mipSlice = viewDesc.Texture2DArray.MostDetailedMip;
			break;
		default:
			break;
		}
		mipSlice = std::min(mipSlice, 31u);

		a_width = std::max(1u, textureDesc.Width >> mipSlice);
		a_height = std::max(1u, textureDesc.Height >> mipSlice);
		return true;
	}
	class ScopedENBDeferMixNativeViewport
	{
	public:
		explicit ScopedENBDeferMixNativeViewport(ID3D11DeviceContext* a_context, bool a_active)
		{
			if (!a_active) {
				return;
			}

			auto* upscaling = Upscaling::GetSingleton();
			context_ = a_context;
			auto* nativeP3 = upscaling ?
				reinterpret_cast<ID3D11Texture2D*>(upscaling->originalRenderTargets[3].texture) : nullptr;
			if (!context_ || !nativeP3) {
				context_ = nullptr;
				return;
			}

			viewportCount_ = static_cast<UINT>(viewports_.size());
			context_->RSGetViewports(&viewportCount_, viewports_.data());
			if (viewportCount_ == 0) {
				context_ = nullptr;
				return;
			}

			D3D11_TEXTURE2D_DESC nativeDesc{};
			nativeP3->GetDesc(&nativeDesc);
			auto nativeViewport = viewports_[0];
			nativeViewport.TopLeftX = 0.0f;
			nativeViewport.TopLeftY = 0.0f;
			nativeViewport.Width = static_cast<float>(nativeDesc.Width);
			nativeViewport.Height = static_cast<float>(nativeDesc.Height);
			context_->RSSetViewports(1, &nativeViewport);
			active_ = true;
		}

		~ScopedENBDeferMixNativeViewport()
		{
			if (active_) {
				context_->RSSetViewports(viewportCount_, viewports_.data());
			}
		}

		ScopedENBDeferMixNativeViewport(const ScopedENBDeferMixNativeViewport&) = delete;
		ScopedENBDeferMixNativeViewport& operator=(const ScopedENBDeferMixNativeViewport&) = delete;

	private:
		ID3D11DeviceContext* context_{ nullptr };
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
		UINT viewportCount_{ 0 };
		bool active_{ false };
	};

	void STDMETHODCALLTYPE ENBDeferMixDrawSiteThunk(
		ID3D11DeviceContext* a_context,
		UINT a_vertexCount,
		UINT a_startVertexLocation,
		D3D11Draw_t a_draw)
	{
		const bool isENBDeferMixHandoff =
			g_enbPrimaryCompositeScopeDepth > 0 &&
			a_vertexCount == 6 &&
			a_startVertexLocation == 0;

		// ENB's private scene color and AO textures retain native allocations but
		// contain the current frame in the upper-left render rectangle. DeferMix
		// restores native P3 and samples those textures with normalized UVs. If it
		// draws through Fallout's 2293x960 DRS viewport, that valid rectangle is
		// contracted once more to 1529x640. Use P3's native viewport for this draw
		// only; all SSAO/DetailedShadow passes and the engine viewport stay intact.
		const ScopedENBDeferMixNativeViewport nativeViewport(a_context, isENBDeferMixHandoff);
		a_draw(a_context, a_vertexCount, a_startVertexLocation);
	}

	bool InstallENBDeferMixDrawCallHook(std::uintptr_t a_drawCall)
	{
		// Replace:
		//   call qword ptr [rax+68h]  ; FF 50 68
		//   inc esi                   ; FF C6
		// with a five-byte call to a generated stub. The stub loads the same
		// vtable target into r9, calls our four-argument helper, reproduces the
		// increment, and returns to the original cmp at drawCall+5.
		constexpr std::array<std::uint8_t, 5> kExpected{ 0xFF, 0x50, 0x68, 0xFF, 0xC6 };
		if (!a_drawCall || !std::equal(
				kExpected.begin(), kExpected.end(),
				reinterpret_cast<const std::uint8_t*>(a_drawCall))) {
			logger::warn("[ENB SR] DeferMix Draw call-site bytes do not match");
			return false;
		}

		// The process-wide F4SE trampoline is allocated near Fallout/the plugin and
		// can be more than 2 GiB from ENB's deliberately low 0x180... image. Give
		// this call site its own page anchored at the ENB instruction so CALL rel32
		// can reach the generated stub without a far branch island.
		if (g_enbDeferMixTrampoline.empty()) {
			g_enbDeferMixTrampoline.create(
				kENBDeferMixTrampolineSize, reinterpret_cast<void*>(a_drawCall));
		}
		auto& trampoline = g_enbDeferMixTrampoline;
		constexpr std::size_t kStubSize = 27;
		auto* stub = static_cast<std::uint8_t*>(trampoline.allocate(kStubSize));
		const std::array<std::uint8_t, kStubSize> stubTemplate{
			0x4C, 0x8B, 0x48, 0x68,                         // mov r9,[rax+68h]
			0x48, 0x83, 0xEC, 0x28,                         // sub rsp,28h
			0x48, 0xB8,                                     // mov rax,imm64
			0, 0, 0, 0, 0, 0, 0, 0,
			0xFF, 0xD0,                                     // call rax
			0x48, 0x83, 0xC4, 0x28,                         // add rsp,28h
			0xFF, 0xC6,                                     // inc esi
			0xC3                                            // ret
		};
		std::memcpy(stub, stubTemplate.data(), stubTemplate.size());
		const auto helper = reinterpret_cast<std::uintptr_t>(&ENBDeferMixDrawSiteThunk);
		std::memcpy(stub + 10, &helper, sizeof(helper));

		const auto displacement64 =
			reinterpret_cast<std::int64_t>(stub) - static_cast<std::int64_t>(a_drawCall + 5);
		if (displacement64 < std::numeric_limits<std::int32_t>::min() ||
			displacement64 > std::numeric_limits<std::int32_t>::max()) {
			logger::warn("[ENB SR] DeferMix call-site trampoline is outside rel32 range");
			return false;
		}

		std::array<std::uint8_t, 5> callPatch{ 0xE8, 0, 0, 0, 0 };
		const auto displacement = static_cast<std::int32_t>(displacement64);
		std::memcpy(callPatch.data() + 1, &displacement, sizeof(displacement));
		if (!REL::WriteSafeData(a_drawCall, callPatch)) {
			logger::warn("[ENB SR] Failed to patch the DeferMix Draw call site");
			return false;
		}

		return true;
	}

	bool CopyENBInputToRenderProxy(
		Upscaling* a_upscaling,
		ID3D11DeviceContext* a_context,
		int a_target)
	{
		if (!a_upscaling || !a_context || a_target < 0 ||
			a_target >= static_cast<int>(std::size(a_upscaling->proxyRenderTargets))) {
			return false;
		}

		auto* source = reinterpret_cast<ID3D11Texture2D*>(a_upscaling->originalRenderTargets[a_target].texture);
		auto* destination = reinterpret_cast<ID3D11Texture2D*>(a_upscaling->proxyRenderTargets[a_target].texture);
		auto* destinationSRV = reinterpret_cast<ID3D11ShaderResourceView*>(a_upscaling->proxyRenderTargets[a_target].srView);
		if (!source || !destination || !destinationSRV) {
			return false;
		}

		D3D11_TEXTURE2D_DESC sourceDesc{};
		D3D11_TEXTURE2D_DESC destinationDesc{};
		source->GetDesc(&sourceDesc);
		destination->GetDesc(&destinationDesc);
		if (sourceDesc.Format != destinationDesc.Format ||
			sourceDesc.SampleDesc.Count != destinationDesc.SampleDesc.Count ||
			destinationDesc.Width > sourceDesc.Width || destinationDesc.Height > sourceDesc.Height) {
			return false;
		}

		// Fallout's native allocation contains the current frame in its upper-left
		// render rectangle. Copy that rectangle 1:1 into the render-sized proxy.
		// ENB can then sample the complete current frame with normalized UVs while
		// retaining its native private output family. This is deliberately not a
		// stretch and does not alter the renderer's active target or metadata.
		const D3D11_BOX sourceBox{
			0, 0, 0,
			destinationDesc.Width,
			destinationDesc.Height,
			1
		};
		a_context->CopySubresourceRegion(destination, 0, 0, 0, 0, source, 0, &sourceBox);
		return true;
	}

	void CopyENBInputsToRenderProxies(std::initializer_list<int> a_targets)
	{
		auto* upscaling = Upscaling::GetSingleton();
		auto* rendererData = RE::BSGraphics::GetRendererData();
		auto* context = rendererData ?
			reinterpret_cast<ID3D11DeviceContext*>(rendererData->context) : nullptr;
		if (!upscaling || !context) {
			return;
		}

		ID3D11DeviceContext* copyContext = context;
		for (const auto target : a_targets) {
			if (target < 0 || target >= static_cast<int>(std::size(upscaling->originalRenderTargets))) {
				continue;
			}
			auto* const source = reinterpret_cast<ID3D11Texture2D*>(
				upscaling->originalRenderTargets[target].texture);
			if (!source) {
				continue;
			}
			if (EnsureENBNativeD3D11Context(source) && CanBypassENBD3D11Context(context)) {
				copyContext = g_enbNativeContext.get();
			}
			break;
		}

		for (const auto target : a_targets) {
			CopyENBInputToRenderProxy(upscaling, copyContext, target);
		}
		// Match the observable end state of the removed ResetRenderTargets call.
		for (const auto target : renderTargetsPatch) {
			// OverrideRenderTarget/ResetRenderTarget both leave late-created or
			// unavailable targets untouched when either side of the proxy pair is
			// missing. Do not overwrite those live engine entries with an old null
			// snapshot.
			if (!upscaling->originalRenderTargets[target].texture ||
				!upscaling->proxyRenderTargets[target].texture) {
				continue;
			}
			rendererData->renderTargets[target] = upscaling->originalRenderTargets[target];
		}

		ID3D11ShaderResourceView* boundSRVs[16]{};
		context->PSGetShaderResources(0, static_cast<UINT>(std::size(boundSRVs)), boundSRVs);
		for (UINT slot = 0; slot < std::size(boundSRVs); ++slot) {
			auto* const bound = boundSRVs[slot];
			if (!bound) {
				continue;
			}
			for (const auto target : renderTargetsPatch) {
				auto* const proxy = reinterpret_cast<ID3D11ShaderResourceView*>(
					upscaling->proxyRenderTargets[target].srView);
				auto* const original = reinterpret_cast<ID3D11ShaderResourceView*>(
					upscaling->originalRenderTargets[target].srView);
				if (bound == proxy && original) {
					context->PSSetShaderResources(slot, 1, &original);
					break;
				}
			}
			bound->Release();
		}

		ID3D11RenderTargetView* boundRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* boundDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, boundRTVs, &boundDSV);
		ID3D11RenderTargetView* restoredRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		bool restoreOM = false;
		for (UINT slot = 0; slot < std::size(boundRTVs); ++slot) {
			restoredRTVs[slot] = boundRTVs[slot];
			if (!boundRTVs[slot]) {
				continue;
			}
			for (const auto target : renderTargetsPatch) {
				auto* const proxy = reinterpret_cast<ID3D11RenderTargetView*>(
					upscaling->proxyRenderTargets[target].rtView);
				auto* const original = reinterpret_cast<ID3D11RenderTargetView*>(
					upscaling->originalRenderTargets[target].rtView);
				if (boundRTVs[slot] == proxy && original) {
					restoredRTVs[slot] = original;
					restoreOM = true;
					break;
				}
			}
		}
		if (restoreOM) {
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, restoredRTVs, boundDSV);
		}
		for (auto* rtv : boundRTVs) {
			if (rtv) {
				rtv->Release();
			}
		}
		if (boundDSV) {
			boundDSV->Release();
		}
	}


	class ScopedENBDetailedShadowProjection
	{
	public:
		explicit ScopedENBDetailedShadowProjection(void* a_this)
		{
			const float ratioX = originalDynamicWidthRatio;
			const float ratioY = originalDynamicHeightRatio;
			if (!a_this || !g_enbDetailedShadowBufferLayout || !ShouldUseENBProxyCompatibility() ||
				!std::isfinite(ratioX) || !std::isfinite(ratioY) ||
				ratioX <= 0.0f || ratioY <= 0.0f ||
				(ratioX == 1.0f && ratioY == 1.0f)) {
				return;
			}

			// ENB copies this CPU projection into a temporary matrix and inverts it
			// into Matrix03, whose output samples native Depth P2. Map that projection
			// into the native allocation subrect on both axes. ENB 0.501 introduced
			// DetailedShadowFirstPersonModels and a Parameters03-controlled shader path
			// whose view reconstruction additionally needs the X-only GPU correction.
			// The older shader (including 0.496) already expects its GPU cb1 unchanged.
			auto* fields = reinterpret_cast<std::uintptr_t*>(a_this);
			const auto bufferState = fields[19];
			if (!bufferState) {
				return;
			}
			const auto layout = *g_enbDetailedShadowBufferLayout;
			auto** cpuDataSlot = reinterpret_cast<std::byte**>(
				bufferState + layout.cpuDataOffset);
			if (!IsWritableProcessAddress(cpuDataSlot) || !*cpuDataSlot) {
				return;
			}

			matrix_ = reinterpret_cast<float*>(*cpuDataSlot + 320);
			if (!IsWritableProcessAddress(matrix_) ||
				!IsWritableProcessAddress(matrix_ + matrixBackup_.size() - 1)) {
				matrix_ = nullptr;
				return;
			}

			std::copy_n(matrix_, matrixBackup_.size(), matrixBackup_.begin());
			cpuData_ = *cpuDataSlot;
			float nativeToRenderX = 1.0f / ratioX;
			float nativeToRenderY = 1.0f / ratioY;
			const auto frameBufferState = fields[9];
			auto** frameCpuDataSlot = frameBufferState ?
				reinterpret_cast<std::byte**>(frameBufferState + layout.cpuDataOffset) : nullptr;
			if (frameCpuDataSlot && IsWritableProcessAddress(frameCpuDataSlot) && *frameCpuDataSlot &&
				IsWritableProcessAddress(*frameCpuDataSlot + sizeof(float)) &&
				g_enbFullWidth && g_enbFullHeight && *g_enbFullWidth > 0 && *g_enbFullHeight > 0) {
				const auto* reciprocalRenderSize = reinterpret_cast<const float*>(*frameCpuDataSlot);
				const auto reciprocalRenderWidth = reciprocalRenderSize[0];
				const auto reciprocalRenderHeight = reciprocalRenderSize[1];
				const auto exactScale = static_cast<float>(*g_enbFullWidth) * reciprocalRenderWidth;
				const auto exactScaleY = static_cast<float>(*g_enbFullHeight) * reciprocalRenderHeight;
				if (std::isfinite(exactScale) && exactScale > 0.0f) {
					nativeToRenderX = exactScale;
				}
				if (std::isfinite(exactScaleY) && exactScaleY > 0.0f) {
					nativeToRenderY = exactScaleY;
				}
			}

			const void* gpuUploadData = nullptr;
			if (g_enbDetailedShadowUsesFirstPersonModelPath) {
				auto** gpuBufferSlot = reinterpret_cast<ID3D11Buffer**>(
					bufferState + layout.gpuBufferOffset);
				auto** contextSlot = reinterpret_cast<ID3D11DeviceContext**>(
					reinterpret_cast<std::byte*>(a_this) + 0x6C20);
				if (!IsWritableProcessAddress(gpuBufferSlot) || !*gpuBufferSlot ||
					!IsWritableProcessAddress(contextSlot) || !*contextSlot) {
					matrix_ = nullptr;
					cpuData_ = nullptr;
					return;
				}

				buffer_ = *gpuBufferSlot;
				context_ = *contextSlot;
				buffer_->GetDesc(&bufferDesc_);
				if (bufferDesc_.ByteWidth < 448 ||
					bufferDesc_.ByteWidth > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16 ||
					!IsWritableProcessAddress(cpuData_ + bufferDesc_.ByteWidth - 1)) {
					matrix_ = nullptr;
					cpuData_ = nullptr;
					buffer_ = nullptr;
					context_ = nullptr;
					return;
				}

				// DetailedShadow may enter repeatedly in one frame. Reuse storage per
				// render thread instead of allocating a temporary buffer for every pass.
				static thread_local std::vector<std::byte> gpuData;
				gpuData.resize(bufferDesc_.ByteWidth);
				std::memcpy(gpuData.data(), cpuData_, bufferDesc_.ByteWidth);
				ComposeNativeToRender(
					reinterpret_cast<float*>(gpuData.data() + 320), nativeToRenderX, 1.0f);
				ComposeNativeToRender(
					reinterpret_cast<float*>(gpuData.data() + 384), nativeToRenderX, 1.0f);
				gpuUploadData = gpuData.data();
			}

			ComposeNativeToRender(matrix_, nativeToRenderX, nativeToRenderY);
			if (buffer_ && !Upload(gpuUploadData)) {
				std::copy(matrixBackup_.begin(), matrixBackup_.end(), matrix_);
				matrix_ = nullptr;
				cpuData_ = nullptr;
				buffer_ = nullptr;
				context_ = nullptr;
				return;
			}
			active_ = true;
		}

		~ScopedENBDetailedShadowProjection()
		{
			if (active_) {
				std::copy(matrixBackup_.begin(), matrixBackup_.end(), matrix_);
				if (buffer_) {
					Upload(cpuData_);
				}
			}
		}

		ScopedENBDetailedShadowProjection(const ScopedENBDetailedShadowProjection&) = delete;
		ScopedENBDetailedShadowProjection& operator=(const ScopedENBDetailedShadowProjection&) = delete;

	private:
		static void ComposeNativeToRender(float* a_matrix, float a_scaleX, float a_scaleY)
		{
			const float translateX = a_scaleX - 1.0f;
			const float translateY = 1.0f - a_scaleY;
			for (std::size_t row = 0; row < 4; ++row) {
				const auto offset = row * 4;
				const float column0 = a_matrix[offset];
				const float column1 = a_matrix[offset + 1];
				a_matrix[offset] = column0 * a_scaleX;
				a_matrix[offset + 1] = column1 * a_scaleY;
				a_matrix[offset + 3] += translateX * column0 + translateY * column1;
			}
		}

		bool Upload(const void* a_data) const
		{
			if (!context_ || !buffer_ || !a_data) {
				return false;
			}
			if (bufferDesc_.Usage == D3D11_USAGE_DYNAMIC &&
				(bufferDesc_.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) != 0) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context_->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					return false;
				}
				std::memcpy(mapped.pData, a_data, bufferDesc_.ByteWidth);
				context_->Unmap(buffer_, 0);
				return true;
			}
			if (bufferDesc_.Usage == D3D11_USAGE_DEFAULT) {
				context_->UpdateSubresource(buffer_, 0, nullptr, a_data, 0, 0);
				return true;
			}
			return false;
		}

		ID3D11DeviceContext* context_ = nullptr;
		ID3D11Buffer* buffer_ = nullptr;
		std::byte* cpuData_ = nullptr;
		D3D11_BUFFER_DESC bufferDesc_{};
		float* matrix_ = nullptr;
		std::array<float, 16> matrixBackup_{};
		bool active_ = false;
	};

	int ENBDetailedShadowRenderThunk(void* a_this, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4)
	{
		const ScopedENBDetailedShadowProjection projection(a_this);
		// Keep Texture0 (+0xD38) bound to Fallout's native Depth P2 SRV. The
		// allocation and E70 producer are already pixel-aligned; only ENB's
		// reconstruction matrix lacks Fallout's render-rectangle transform.
		const auto result = g_enbDetailedShadowRender(a_this, a2, a3, a4);
		return result;
	}

	class ScopedENBTextureDepthBinding
	{
	public:
		explicit ScopedENBTextureDepthBinding(bool a_nativeDepthReady)
		{
			auto* replacement = g_enbNativeDepthSRV.get();
			if (!a_nativeDepthReady || !g_enbDepthTextureSRVSlot || !replacement) {
				return;
			}

			slot_ = g_enbDepthTextureSRVSlot;
			original_ = *slot_;
			replacement_ = replacement;
			if (original_ == replacement_) {
				return;
			}

			*slot_ = replacement_;
			active_ = true;
		}

		~ScopedENBTextureDepthBinding()
		{
			// Do not overwrite a resource ENB may have recreated while the scope
			// was active.
			if (active_ && *slot_ == replacement_) {
				*slot_ = original_;
			}
		}

		ScopedENBTextureDepthBinding(const ScopedENBTextureDepthBinding&) = delete;
		ScopedENBTextureDepthBinding& operator=(const ScopedENBTextureDepthBinding&) = delete;

	private:
		ID3D11ShaderResourceView** slot_ = nullptr;
		ID3D11ShaderResourceView* original_ = nullptr;
		ID3D11ShaderResourceView* replacement_ = nullptr;
		bool active_ = false;
	};

	class ScopedFalloutMainDepthBinding
	{
	public:
		explicit ScopedFalloutMainDepthBinding(bool a_nativeDepthReady)
		{
			auto* replacement = g_enbNativeDepthSRV.get();
			static auto* rendererData = RE::BSGraphics::GetRendererData();
			if (!a_nativeDepthReady || !replacement || !rendererData) {
				return;
			}

			slot_ = &rendererData->depthStencilTargets[
				static_cast<std::uint32_t>(Util::DepthStencilTarget::kMain)].srViewDepth;
			original_ = *slot_;
			replacement_ = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(replacement);
			if (original_ == replacement_) {
				return;
			}

			*slot_ = replacement_;
			active_ = true;
		}

		~ScopedFalloutMainDepthBinding()
		{
			// Preserve a replacement made by the engine while Bokeh was active.
			if (active_ && *slot_ == replacement_) {
				*slot_ = original_;
			}
		}

		ScopedFalloutMainDepthBinding(const ScopedFalloutMainDepthBinding&) = delete;
		ScopedFalloutMainDepthBinding& operator=(const ScopedFalloutMainDepthBinding&) = delete;

	private:
		REX::W32::ID3D11ShaderResourceView** slot_ = nullptr;
		REX::W32::ID3D11ShaderResourceView* original_ = nullptr;
		REX::W32::ID3D11ShaderResourceView* replacement_ = nullptr;
		bool active_ = false;
	};

	constexpr std::uint32_t kENBSSSProjectionShaderHash = 0xB0CA0F9C;
	constexpr std::size_t kENBSSSProjectionShaderSize = 8848;
	// Ghidra-verified ENB 0.420, 0.496, and 0.501 wrapper layout.
	constexpr std::ptrdiff_t kENBPixelShaderHashOffset = 0x8;
	constexpr std::ptrdiff_t kENBPixelShaderNativeAlternateOffset = 0x2D0;
	constexpr std::ptrdiff_t kENBPixelShaderBytecodeOffset = 0x2D8;
	constexpr std::ptrdiff_t kENBPixelShaderBytecodeSizeOffset = 0x2E0;
	// Exact immediate locations in the checksum-gated 8,848-byte shader revision.
	constexpr std::size_t kENBProjectionScaleImmediateOffset = 0x73C;
	constexpr std::size_t kENBProjectionOffsetImmediateOffset = 0x750;
	constexpr std::array<std::uint8_t, 16> kENBSSSProjectionShaderChecksum{
		0x4A, 0xAB, 0x0D, 0xBF, 0xF5, 0xC1, 0x44, 0xE4,
		0x3B, 0x06, 0x70, 0x25, 0xC8, 0xB4, 0x68, 0xA5
	};

	struct ENBSSSProjectionShaderCache
	{
		ID3D11Device* device{};
		UINT outputWidth{};
		UINT outputHeight{};
		float viewportWidth{};
		float viewportHeight{};
		winrt::com_ptr<ID3D11PixelShader> shader;
	};
	struct ENBSSSProjectionBinding
	{
		ID3D11PixelShader* wrapper{};
		ID3D11PixelShader** alternateSlot{};
		ID3D11PixelShader* originalAlternate{};
	};

	ENBSSSProjectionShaderCache g_enbSSSProjectionShaderCache;
	ENBSSSProjectionBinding g_enbSSSProjectionBinding;
	std::uintptr_t* g_enbPSSetShaderVtableSlot = nullptr;
	bool g_enbPSSetShaderHookInstalled = false;
	bool g_enbSSSProjectionDiscoveryActive = false;
	bool g_enbSSSProjectionDiscoveryRejected = false;

	std::array<std::uint32_t, 4> ComputeDXBCChecksum(
		const std::uint8_t* a_container,
		std::size_t a_containerSize)
	{
		constexpr std::array<std::uint32_t, 64> shifts{
			7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
			5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
			4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
			6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
		};
		constexpr std::array<std::uint32_t, 64> constants{
			0xD76AA478, 0xE8C7B756, 0x242070DB, 0xC1BDCEEE,
			0xF57C0FAF, 0x4787C62A, 0xA8304613, 0xFD469501,
			0x698098D8, 0x8B44F7AF, 0xFFFF5BB1, 0x895CD7BE,
			0x6B901122, 0xFD987193, 0xA679438E, 0x49B40821,
			0xF61E2562, 0xC040B340, 0x265E5A51, 0xE9B6C7AA,
			0xD62F105D, 0x02441453, 0xD8A1E681, 0xE7D3FBC8,
			0x21E1CDE6, 0xC33707D6, 0xF4D50D87, 0x455A14ED,
			0xA9E3E905, 0xFCEFA3F8, 0x676F02D9, 0x8D2A4C8A,
			0xFFFA3942, 0x8771F681, 0x6D9D6122, 0xFDE5380C,
			0xA4BEEA44, 0x4BDECFA9, 0xF6BB4B60, 0xBEBFBC70,
			0x289B7EC6, 0xEAA127FA, 0xD4EF3085, 0x04881D05,
			0xD9D4D039, 0xE6DB99E5, 0x1FA27CF8, 0xC4AC5665,
			0xF4292244, 0x432AFF97, 0xAB9423A7, 0xFC93A039,
			0x655B59C3, 0x8F0CCC92, 0xFFEFF47D, 0x85845DD1,
			0x6FA87E4F, 0xFE2CE6E0, 0xA3014314, 0x4E0811A1,
			0xF7537E82, 0xBD3AF235, 0x2AD7D2BB, 0xEB86D391
		};

		if (!a_container || a_containerSize < 20) {
			return {};
		}
		const auto* data = a_container + 20;
		const auto dataSize = a_containerSize - 20;
		const auto completeSize = dataSize - dataSize % 64;
		const auto tailSize = dataSize - completeSize;
		std::vector<std::uint8_t> padded(data, data + completeSize);
		padded.reserve(completeSize + 128);
		const auto appendWord = [&padded](std::uint32_t a_value) {
			const auto offset = padded.size();
			padded.resize(offset + sizeof(a_value));
			std::memcpy(padded.data() + offset, &a_value, sizeof(a_value));
		};

		const auto bitLength = static_cast<std::uint32_t>(dataSize * 8);
		const auto finalWord = (bitLength >> 2) | 1u;
		const auto* tail = data + completeSize;
		if (tailSize >= 56) {
			padded.insert(padded.end(), tail, tail + tailSize);
			padded.push_back(0x80);
			padded.resize(padded.size() + 63 - tailSize, 0);
			appendWord(bitLength);
			padded.resize(padded.size() + 56, 0);
			appendWord(finalWord);
		} else {
			appendWord(bitLength);
			padded.insert(padded.end(), tail, tail + tailSize);
			padded.push_back(0x80);
			padded.resize(padded.size() + 55 - tailSize, 0);
			appendWord(finalWord);
		}

		std::array<std::uint32_t, 4> state{
			0x67452301,
			0xEFCDAB89,
			0x98BADCFE,
			0x10325476
		};
		const auto rotateLeft = [](std::uint32_t a_value, std::uint32_t a_shift) {
			return (a_value << a_shift) | (a_value >> (32 - a_shift));
		};
		for (std::size_t blockOffset = 0; blockOffset < padded.size(); blockOffset += 64) {
			std::array<std::uint32_t, 16> words{};
			std::memcpy(words.data(), padded.data() + blockOffset, 64);
			auto a = state[0];
			auto b = state[1];
			auto c = state[2];
			auto d = state[3];
			for (std::uint32_t index = 0; index < 64; ++index) {
				std::uint32_t function = 0;
				std::uint32_t wordIndex = 0;
				if (index < 16) {
					function = (b & c) | (~b & d);
					wordIndex = index;
				} else if (index < 32) {
					function = (d & b) | (~d & c);
					wordIndex = (5 * index + 1) % 16;
				} else if (index < 48) {
					function = b ^ c ^ d;
					wordIndex = (3 * index + 5) % 16;
				} else {
					function = c ^ (b | ~d);
					wordIndex = (7 * index) % 16;
				}

				const auto nextB = b + rotateLeft(
					a + function + constants[index] + words[wordIndex],
					shifts[index]);
				a = d;
				d = c;
				c = b;
				b = nextB;
			}
			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
		}
		return state;
	}

	bool IsSupportedENBSSSProjectionShader(const void* a_bytecode, std::uint32_t a_size)
	{
		if (!a_bytecode || a_size != kENBSSSProjectionShaderSize ||
			!IsReadableProcessRange(a_bytecode, a_size) ||
			std::memcmp(a_bytecode, "DXBC", 4) != 0 ||
			std::memcmp(
				static_cast<const std::byte*>(a_bytecode) + 4,
				kENBSSSProjectionShaderChecksum.data(),
				kENBSSSProjectionShaderChecksum.size()) != 0) {
			return false;
		}
		std::uint32_t containerSize = 0;
		std::memcpy(
			&containerSize,
			static_cast<const std::byte*>(a_bytecode) + 24,
			sizeof(containerSize));
		return containerSize == a_size;
	}

	winrt::com_ptr<ID3D11PixelShader> CreateENBSSSProjectionShader(
		ID3D11Device* a_device,
		const void* a_bytecode,
		std::uint32_t a_bytecodeSize,
		UINT a_outputWidth,
		UINT a_outputHeight,
		const D3D11_VIEWPORT& a_viewport)
	{
		winrt::com_ptr<ID3D11PixelShader> result;
		std::vector<std::uint8_t> patched(a_bytecodeSize);
		std::memcpy(patched.data(), a_bytecode, patched.size());

		const float projectionScaleX =
			2.0f * static_cast<float>(a_outputWidth) / a_viewport.Width;
		const float projectionScaleY =
			2.0f * static_cast<float>(a_outputHeight) / a_viewport.Height;
		// Exact live-test edit: change only the first two immediates of each
		// float4 used by the cubemap view-ray projection MAD.
		const std::array<float, 2> projectionScale{
			projectionScaleX,
			projectionScaleY
		};
		const std::array<float, 2> projectionOffset{
			-1.0f,
			1.0f - projectionScaleY
		};
		std::memcpy(
			patched.data() + kENBProjectionScaleImmediateOffset,
			projectionScale.data(),
			sizeof(projectionScale));
		std::memcpy(
			patched.data() + kENBProjectionOffsetImmediateOffset,
			projectionOffset.data(),
			sizeof(projectionOffset));
		const auto checksum = ComputeDXBCChecksum(patched.data(), patched.size());
		std::memcpy(patched.data() + 4, checksum.data(), sizeof(checksum));

		const auto hr = a_device->CreatePixelShader(
			patched.data(),
			patched.size(),
			nullptr,
			result.put());
		if (FAILED(hr) || !result) {
			logger::warn(
				"[ENB SSS] Cannot create projection shader 0x{:08X}: 0x{:08X}",
				kENBSSSProjectionShaderHash,
				static_cast<std::uint32_t>(hr));
			return {};
		}

		logger::info(
			"[ENB SSS] Projection shader 0x{:08X}: output={}x{}, viewport="
			"({:.3f},{:.3f}) {:.3f}x{:.3f}, mad=({:.6f},{:.6f})+({:.6f},{:.6f})",
			kENBSSSProjectionShaderHash,
			a_outputWidth,
			a_outputHeight,
			a_viewport.TopLeftX,
			a_viewport.TopLeftY,
			a_viewport.Width,
			a_viewport.Height,
			projectionScaleX,
			projectionScaleY,
			projectionOffset[0],
			projectionOffset[1]);
		return result;
	}

	ID3D11PixelShader* GetENBSSSProjectionShader(
		ID3D11DeviceContext* a_context,
		ID3D11PixelShader* a_originalShader,
		const void* a_bytecode,
		std::uint32_t a_bytecodeSize)
	{
		if (!a_context || !a_originalShader ||
			!IsSupportedENBSSSProjectionShader(a_bytecode, a_bytecodeSize)) {
			return nullptr;
		}

		D3D11_VIEWPORT viewport{};
		UINT viewportCount = 1;
		a_context->RSGetViewports(&viewportCount, &viewport);
		if (viewportCount != 1 || !std::isfinite(viewport.TopLeftX) ||
			!std::isfinite(viewport.TopLeftY) || !std::isfinite(viewport.Width) ||
			!std::isfinite(viewport.Height) || viewport.Width < 1.0f ||
			viewport.Height < 1.0f) {
			return nullptr;
		}

		winrt::com_ptr<ID3D11RenderTargetView> output;
		a_context->OMGetRenderTargets(1, output.put(), nullptr);
		UINT outputWidth = 0;
		UINT outputHeight = 0;
		if (!GetENBRenderTargetDimensions(output.get(), outputWidth, outputHeight)) {
			return nullptr;
		}

		constexpr float kViewportTolerance = 0.5f;
		if (viewport.TopLeftX < -kViewportTolerance ||
			viewport.TopLeftY < -kViewportTolerance ||
			viewport.TopLeftX + viewport.Width >
				static_cast<float>(outputWidth) + kViewportTolerance ||
			viewport.TopLeftY + viewport.Height >
				static_cast<float>(outputHeight) + kViewportTolerance) {
			return nullptr;
		}
		// The accepted live test covered the zero-origin SSS viewport. Do not infer
		// a projection convention for a different viewport placement.
		if (std::abs(viewport.TopLeftX) >= 0.01f ||
			std::abs(viewport.TopLeftY) >= 0.01f) {
			return nullptr;
		}
		if (std::abs(viewport.Width - static_cast<float>(outputWidth)) < 0.01f &&
			std::abs(viewport.Height - static_cast<float>(outputHeight)) < 0.01f) {
			return nullptr;
		}

		winrt::com_ptr<ID3D11Device> device;
		a_originalShader->GetDevice(device.put());
		if (!device) {
			return nullptr;
		}

		if (g_enbSSSProjectionShaderCache.shader &&
			g_enbSSSProjectionShaderCache.device == device.get() &&
			g_enbSSSProjectionShaderCache.outputWidth == outputWidth &&
			g_enbSSSProjectionShaderCache.outputHeight == outputHeight &&
			g_enbSSSProjectionShaderCache.viewportWidth == viewport.Width &&
			g_enbSSSProjectionShaderCache.viewportHeight == viewport.Height) {
			return g_enbSSSProjectionShaderCache.shader.get();
		}

		auto shader = CreateENBSSSProjectionShader(
			device.get(),
			a_bytecode,
			a_bytecodeSize,
			outputWidth,
			outputHeight,
			viewport);
		if (!shader) {
			return nullptr;
		}

		g_enbSSSProjectionShaderCache = {
			device.get(),
			outputWidth,
			outputHeight,
			viewport.Width,
			viewport.Height,
			std::move(shader)
		};
		return g_enbSSSProjectionShaderCache.shader.get();
	}

	ID3D11PixelShader* PrepareENBSSSProjectionSubstitution(
		ID3D11DeviceContext* a_context,
		ID3D11PixelShader* a_shaderWrapper,
		ID3D11PixelShader*** a_alternateShaderSlot)
	{
		if (!g_enbSSSProjectionDiscoveryActive || !a_context || !a_shaderWrapper ||
			!a_alternateShaderSlot || !IsReadableProcessRange(
				a_shaderWrapper,
				kENBPixelShaderBytecodeSizeOffset + sizeof(std::uint32_t))) {
			return nullptr;
		}

		const auto shader = reinterpret_cast<std::byte*>(a_shaderWrapper);
		std::uint32_t hash = 0;
		std::memcpy(&hash, shader + kENBPixelShaderHashOffset, sizeof(hash));
		if (hash != kENBSSSProjectionShaderHash) {
			return nullptr;
		}

		auto** alternateShaderSlot = reinterpret_cast<ID3D11PixelShader**>(
			shader + kENBPixelShaderNativeAlternateOffset);
		if (!IsWritableProcessAddress(alternateShaderSlot) || !*alternateShaderSlot) {
			return nullptr;
		}

		const void* bytecode = nullptr;
		std::uint32_t bytecodeSize = 0;
		std::memcpy(
			&bytecode,
			shader + kENBPixelShaderBytecodeOffset,
			sizeof(bytecode));
		std::memcpy(
			&bytecodeSize,
			shader + kENBPixelShaderBytecodeSizeOffset,
			sizeof(bytecodeSize));
		if (!IsSupportedENBSSSProjectionShader(bytecode, bytecodeSize)) {
			g_enbSSSProjectionDiscoveryRejected = true;
			return nullptr;
		}
		auto* replacement = GetENBSSSProjectionShader(
			a_context,
			*alternateShaderSlot,
			bytecode,
			bytecodeSize);
		if (replacement) {
			*a_alternateShaderSlot = alternateShaderSlot;
		}
		return replacement;
	}

	void RemoveENBSSSProjectionDiscoveryHook()
	{
		g_enbSSSProjectionDiscoveryActive = false;
		if (!g_enbPSSetShaderHookInstalled || !g_enbPSSetShaderVtableSlot || !g_enbPSSetShader) {
			return;
		}

		const auto original = reinterpret_cast<std::uintptr_t>(g_enbPSSetShader);
		if (REL::WriteSafeData(
				reinterpret_cast<std::uintptr_t>(g_enbPSSetShaderVtableSlot),
				original)) {
			g_enbPSSetShaderHookInstalled = false;
		}
	}

	bool IsENBSSSProjectionSubstitutionActive()
	{
		return g_enbSSSProjectionBinding.alternateSlot &&
			IsReadableProcessRange(
				g_enbSSSProjectionBinding.alternateSlot,
				sizeof(*g_enbSSSProjectionBinding.alternateSlot)) &&
			g_enbSSSProjectionShaderCache.shader &&
			*g_enbSSSProjectionBinding.alternateSlot == g_enbSSSProjectionShaderCache.shader.get();
	}

	bool IsENBSSSProjectionSubstitutionCurrent()
	{
		if (!IsENBSSSProjectionSubstitutionActive()) {
			return false;
		}

		static auto* renderTargetManager = Util::RenderTargetManager_GetSingleton();
		static auto* gameViewport = Util::State_GetSingleton();
		if (!renderTargetManager || !gameViewport) {
			return false;
		}
		const auto ratios = GetDynamicResolutionRatios();
		const auto expectedViewportWidth = std::max(
			1u,
			static_cast<UINT>(
				static_cast<float>(gameViewport->screenWidth) * ratios.width));
		const auto expectedViewportHeight = std::max(
			1u,
			static_cast<UINT>(
				static_cast<float>(gameViewport->screenHeight) * ratios.height));
		return g_enbSSSProjectionShaderCache.outputWidth == gameViewport->screenWidth &&
			g_enbSSSProjectionShaderCache.outputHeight == gameViewport->screenHeight &&
			std::abs(g_enbSSSProjectionShaderCache.viewportWidth -
				static_cast<float>(expectedViewportWidth)) < 0.5f &&
			std::abs(g_enbSSSProjectionShaderCache.viewportHeight -
				static_cast<float>(expectedViewportHeight)) < 0.5f;
	}

	void ResetENBSSSProjectionSubstitution()
	{
		RemoveENBSSSProjectionDiscoveryHook();
		if (IsENBSSSProjectionSubstitutionActive() &&
			IsWritableProcessAddress(g_enbSSSProjectionBinding.alternateSlot)) {
			*g_enbSSSProjectionBinding.alternateSlot =
				g_enbSSSProjectionBinding.originalAlternate;
		}
		g_enbSSSProjectionBinding = {};
	}

	void STDMETHODCALLTYPE ENBPSSetShaderThunk(
		ID3D11DeviceContext* a_context,
		ID3D11PixelShader* a_shaderWrapper,
		ID3D11ClassInstance* const* a_classInstances,
		UINT a_classInstanceCount)
	{
		ID3D11PixelShader** alternateShaderSlot = nullptr;
		ID3D11PixelShader* replacement = nullptr;
		try {
			replacement = PrepareENBSSSProjectionSubstitution(
				a_context,
				a_shaderWrapper,
				&alternateShaderSlot);
		} catch (const std::exception& e) {
			logger::warn("[ENB SSS] Projection shader preparation failed: {}", e.what());
		} catch (...) {
			logger::warn("[ENB SSS] Projection shader preparation failed with an unknown exception");
		}

		if (replacement && alternateShaderSlot) {
			// Keep the confirmed live-test edit on the single validated wrapper.
			// The hook is removed before forwarding this bind, so all subsequent
			// PSSetShader calls return to ENB's original vtable entry.
			g_enbSSSProjectionBinding = {
				a_shaderWrapper,
				alternateShaderSlot,
				*alternateShaderSlot
			};
			*alternateShaderSlot = replacement;
			RemoveENBSSSProjectionDiscoveryHook();
		}

		g_enbPSSetShader(
			a_context,
			a_shaderWrapper,
			a_classInstances,
			a_classInstanceCount);
	}

	bool BeginENBSSSProjectionDiscovery()
	{
		static bool supportChecked = false;
		static bool supported = false;
		static bool installedLogWritten = false;
		static auto* rendererData = RE::BSGraphics::GetRendererData();
		auto* context = rendererData ?
			reinterpret_cast<ID3D11DeviceContext*>(rendererData->context) : nullptr;
		if (!enbLoaded || !context || g_enbSSSProjectionDiscoveryRejected ||
			IsENBSSSProjectionSubstitutionCurrent()) {
			return false;
		}
		if (g_enbSSSProjectionBinding.alternateSlot) {
			ResetENBSSSProjectionSubstitution();
		}

		if (!supportChecked) {
			const auto module = FindENBModule();
			supportChecked = true;

			constexpr std::uint32_t kPSSetShaderVtableIndex = 9;
			const auto code = GetENBExecutableCode(module);
			const auto vtable = *reinterpret_cast<std::uintptr_t**>(context);
			const auto psSetShader = vtable ? vtable[kPSSetShaderVtableIndex] : 0;
			if (!code || psSetShader < reinterpret_cast<std::uintptr_t>(code->begin) ||
				psSetShader >= reinterpret_cast<std::uintptr_t>(code->begin + code->size)) {
				logger::warn("[ENB SSS] The immediate context is not the validated ENB wrapper");
				return false;
			}

			g_enbPSSetShader = reinterpret_cast<ENBPSSetShader_t>(psSetShader);
			g_enbPSSetShaderVtableSlot = &vtable[kPSSetShaderVtableIndex];
			supported = true;
		}
		if (!supported || !g_enbPSSetShaderVtableSlot || !g_enbPSSetShader) {
			return false;
		}

		const auto hook = reinterpret_cast<std::uintptr_t>(&ENBPSSetShaderThunk);
		if (!REL::WriteSafeData(
				reinterpret_cast<std::uintptr_t>(g_enbPSSetShaderVtableSlot),
				hook)) {
			logger::warn("[ENB SSS] Failed to arm the ENB PSSetShader projection discovery hook");
			return false;
		}

		g_enbPSSetShaderHookInstalled = true;
		g_enbSSSProjectionDiscoveryActive = true;
		if (!installedLogWritten) {
			installedLogWritten = true;
			logger::info("[ENB SSS] Armed the one-shot ENB projection discovery hook");
		}
		return true;
	}

	void InstallENBScreenEffectRenderHooks()
	{
		if (!enbLoaded) {
			return;
		}

		const auto module = FindENBModule();
		const auto code = GetENBExecutableCode(module);
		if (!module || !code) {
			logger::warn("[ENB SR] Cannot inspect ENB code for screen-effect renderer hooks");
			return;
		}
		const auto ssaoSetting = FindENBSettingStorage(module, *code, "EnableSSAO");
		const auto detailedShadowSetting = FindENBSettingStorage(module, *code, "EnableDetailedShadow");
		const auto depthOfFieldSetting = FindENBSettingStorage(module, *code, "EnableDepthOfField");

		if (!g_enbDeferMixInstalled && ssaoSetting) {
			const auto ssaoEntry = FindENBSSAORenderEntry(*code, ssaoSetting);
			const auto compositeWrapperEntry = FindENBSSAOCompositeWrapper(*code, ssaoEntry);
			const auto deferMixDrawCallAddress =
				FindENBDeferMixDrawCallAddress(*code, compositeWrapperEntry);
			if (!ssaoEntry || !compositeWrapperEntry || !deferMixDrawCallAddress) {
				logger::warn("[ENB SR] Cannot resolve the DeferMix Draw handoff");
			} else if (!InstallENBDeferMixDrawCallHook(deferMixDrawCallAddress)) {
				logger::warn("[ENB SR] DeferMix Draw call-site hook is unavailable");
			} else {
				g_enbDeferMixInstalled = true;
			}
		}

		if (!g_enbDetailedShadowRender && detailedShadowSetting) {
			const auto detailedShadowEntry =
				FindENBDetailedShadowRenderEntry(*code, detailedShadowSetting);
			if (!detailedShadowEntry) {
				logger::warn("[ENB SR] Cannot resolve the DetailedShadow renderer");
			} else {
				const auto bufferLayout =
					FindENBDetailedShadowBufferLayout(*code, detailedShadowEntry);
				if (!bufferLayout) {
					logger::warn("[ENB SR] Cannot resolve the DetailedShadow constant-list layout");
				} else {
					const auto trampoline = Detours::X64::DetourFunction(
						detailedShadowEntry,
						reinterpret_cast<std::uintptr_t>(&ENBDetailedShadowRenderThunk));
					if (trampoline) {
						g_enbDetailedShadowBufferLayout = bufferLayout;
						logger::info(
							"[ENB SR] DetailedShadow constant-list layout: CPU +0x{:X}, GPU +0x{:X}",
							bufferLayout->cpuDataOffset,
							bufferLayout->gpuBufferOffset);
						// 0.501 introduced this setting together with the Parameters03 DShadow
						// branch whose GPU inverse projection has the asymmetric X coordinate.
						// Use the feature marker instead of assuming a contiguous version range.
						g_enbDetailedShadowUsesFirstPersonModelPath = ENBModuleContainsString(
							module, "DetailedShadowFirstPersonModels");
						g_enbDetailedShadowRender = reinterpret_cast<ENBScreenEffectRender_t>(trampoline);
					} else {
						logger::warn("[ENB SR] Failed to install DetailedShadow projection hook");
					}
				}
			}
		}

		if (!g_enbDepthTextureSRVSlot && depthOfFieldSetting) {
			// DepthOfField is only the discovery anchor. This global SRV slot is
			// shared by ENB's SSAO, main effect, DOF, and postpass renderers.
			const auto depthOfFieldEntry = FindENBDepthOfFieldRenderEntry(*code, depthOfFieldSetting);
			const auto textureDepthName = FindENBModuleString(module, "TextureDepth");
			const auto depthTextureSRVSlot =
				FindENBDepthTextureSRVSlot(depthOfFieldEntry, textureDepthName);
			if (!depthOfFieldEntry || !depthTextureSRVSlot) {
				logger::warn("[ENB SR] Cannot resolve the shared TextureDepth SRV binding");
			} else {
				g_enbDepthTextureSRVSlot = depthTextureSRVSlot;
			}
		}
	}

	std::vector<std::uintptr_t> FindENBPrepassVariableSRVSlots(
		std::uintptr_t a_prepass,
		std::uintptr_t a_variableName)
	{
		std::vector<std::uintptr_t> slots;
		if (!a_prepass || !a_variableName) {
			return slots;
		}

		// ENB assigns an effect texture variable with a RIP-relative SRV load
		// followed by a RIP-relative load of the variable name. Resolve the
		// resource slots from that contract so wrapper RVAs can vary by version.
		constexpr std::size_t kVariableBindingScanSize = 0x800;
		const auto function = reinterpret_cast<const std::uint8_t*>(a_prepass);
		for (std::size_t offset = 0; offset + 14 <= kVariableBindingScanSize; ++offset) {
			const auto load = function + offset;
			const auto name = load + 7;
			if (load[0] != 0x4C || load[1] != 0x8B || load[2] != 0x05 ||
				name[0] != 0x48 || name[1] != 0x8D || name[2] != 0x15 ||
				ResolveENBRIPRelativeAddress(name) != a_variableName) {
				continue;
			}

			const auto slot = ResolveENBRIPRelativeAddress(load);
			if (std::find(slots.begin(), slots.end(), slot) == slots.end()) {
				slots.push_back(slot);
			}
			offset += 13;
		}
		return slots;
	}

	class ScopedENBPrepassDepthSRVBindings
	{
	public:
		ScopedENBPrepassDepthSRVBindings()
		{
			auto* nativeDepth = g_enbNativeDepthSRV.get();
			if (!nativeDepth ||
				std::ranges::any_of(g_enbPrepassDepthSRVAddresses, [](const auto address) { return address == 0; })) {
				return;
			}

			for (std::size_t i = 0; i < g_enbPrepassDepthSRVAddresses.size(); ++i) {
				auto** slot = reinterpret_cast<ID3D11ShaderResourceView**>(g_enbPrepassDepthSRVAddresses[i]);
				original_[i] = *slot;
				*slot = nativeDepth;
			}
			active_ = true;
		}

		~ScopedENBPrepassDepthSRVBindings()
		{
			if (!active_) {
				return;
			}

			for (std::size_t i = 0; i < g_enbPrepassDepthSRVAddresses.size(); ++i) {
				auto** slot = reinterpret_cast<ID3D11ShaderResourceView**>(g_enbPrepassDepthSRVAddresses[i]);
				*slot = original_[i];
			}
		}

		ScopedENBPrepassDepthSRVBindings(const ScopedENBPrepassDepthSRVBindings&) = delete;
		ScopedENBPrepassDepthSRVBindings& operator=(const ScopedENBPrepassDepthSRVBindings&) = delete;

	private:
		std::array<ID3D11ShaderResourceView*, 2> original_{};
		bool active_{ false };
	};

	struct ENBPrepassDepthBridge
	{
		static bool thunk(
			ID3D11DeviceContext* a_context,
			UINT a_indexCount,
			UINT a_startIndexLocation,
			INT a_baseVertexLocation)
		{
			if (g_enbPrepassDepthBridgeScopeDepth <= 0) {
				return func(a_context, a_indexCount, a_startIndexLocation, a_baseVertexLocation);
			}

			const ScopedENBPrepassDepthSRVBindings depthBindings;
			return func(a_context, a_indexCount, a_startIndexLocation, a_baseVertexLocation);
		}

		static inline bool (*func)(ID3D11DeviceContext*, UINT, UINT, INT) = nullptr;
	};

	void ResolveENBPrepassResourceSlots()
	{
		if (!enbLoaded || g_enbTextureOriginalSRVAddress) {
			return;
		}

		const auto module = FindENBModule();
		if (!ENBModuleContainsString(module, "EnablePrepass")) {
			return;
		}
		const auto prepass = FindLiveENBPrepass(module);
		if (!prepass) {
			return;
		}

		const auto textureOriginalName = FindENBModuleString(module, "TextureOriginal");
		const auto textureDepthName = FindENBModuleString(module, "TextureDepth");
		const auto textureOriginalSlots = FindENBPrepassVariableSRVSlots(prepass, textureOriginalName);
		const auto textureDepthSlots = FindENBPrepassVariableSRVSlots(prepass, textureDepthName);
		if (textureOriginalSlots.empty()) {
			return;
		}
		if (textureDepthSlots.size() != g_enbPrepassDepthSRVAddresses.size()) {
			return;
		}

		const auto trampoline = Detours::X64::DetourFunction(
			prepass,
			reinterpret_cast<std::uintptr_t>(&ENBPrepassDepthBridge::thunk));
		if (!trampoline) {
			return;
		}
		g_enbTextureOriginalSRVAddress = textureOriginalSlots.front();
		std::ranges::copy(textureDepthSlots, g_enbPrepassDepthSRVAddresses.begin());
		ENBPrepassDepthBridge::func = reinterpret_cast<decltype(ENBPrepassDepthBridge::func)>(trampoline);
	}

	ID3D11Texture2D* GetENBPrepassTexture(std::ptrdiff_t a_textureOffset)
	{
		if (!g_enbTextureOriginalSRVAddress) {
			return nullptr;
		}

		constexpr std::ptrdiff_t kTextureOriginalTextureFromSRV = -0x10;
		const auto textureSlot = g_enbTextureOriginalSRVAddress + kTextureOriginalTextureFromSRV + a_textureOffset;
		return *reinterpret_cast<ID3D11Texture2D**>(textureSlot);
	}

	ID3D11RenderTargetView* GetENBPrepassRTV(std::ptrdiff_t a_textureOffset)
	{
		if (!g_enbTextureOriginalSRVAddress) {
			return nullptr;
		}

		constexpr std::ptrdiff_t kTextureOriginalRTVFromSRV = -0x08;
		const auto rtvSlot = g_enbTextureOriginalSRVAddress + kTextureOriginalRTVFromSRV + a_textureOffset;
		return *reinterpret_cast<ID3D11RenderTargetView**>(rtvSlot);
	}

	bool PrimeENBPrepassNativeColor(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		ID3D11ShaderResourceView* a_nativeScene)
	{
		if (!a_device || !a_context || !a_nativeScene) {
			return false;
		}

		auto* textureOriginal = GetENBPrepassTexture(0x00);
		auto* textureOriginalRTV = GetENBPrepassRTV(0x00);
		if (!textureOriginal || !textureOriginalRTV) {
			return false;
		}

		// ENB 0.496 and 0.502 bind TextureOriginal for the first prepass technique,
		// writes the result to one TextureColor surface, then ping-pongs A/B.
		// Neither color surface is read before ENB overwrites it.
		return ScaleCopyRenderTarget(
			a_device,
			a_context,
			a_nativeScene,
			textureOriginalRTV,
			textureOriginal);
	}

	bool PrepareENBSuperResolutionInput(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		ID3D11ShaderResourceView* a_nativeSource,
		Texture2D* a_upscalingInput,
		uint32_t a_renderWidth,
		uint32_t a_renderHeight)
	{
		if (!a_device || !a_context || !a_nativeSource || !a_upscalingInput ||
			!a_upscalingInput->resource || !a_upscalingInput->uav || !a_upscalingInput->rtv ||
			a_renderWidth == 0 || a_renderHeight == 0) {
			return false;
		}

		D3D11_TEXTURE2D_DESC inputDesc{};
		a_upscalingInput->resource->GetDesc(&inputDesc);
		if (a_renderWidth > inputDesc.Width || a_renderHeight > inputDesc.Height) {
			return false;
		}

		auto* workDevice = a_device;
		auto* workContext = a_context;
		// Upscale() has already unbound OM outputs. Keep this helper draw invisible
		// to ENB and restore the real context before the engine resumes.
		if (EnsureENBNativeD3D11Context(a_upscalingInput->resource.get()) &&
			CanBypassENBD3D11Context(a_context)) {
			workDevice = g_enbNativeDevice.get();
			workContext = g_enbNativeContext.get();
		}

		// Temporal upscalers consume only the supplied render rectangle. The rest
		// of this native allocation is intentionally undefined on the ordinary
		// CopyResource path as well, so a full-surface UAV clear is redundant.
		return ScaleCopyRenderTarget(
			workDevice,
			workContext,
			a_nativeSource,
			a_upscalingInput->rtv.get(),
			a_upscalingInput->resource.get(),
			a_renderWidth,
			a_renderHeight);
	}

	bool PrepareENBNativeDepth(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		ID3D11ShaderResourceView* a_renderDepth,
		uint32_t a_nativeWidth,
		uint32_t a_nativeHeight)
	{
		if (!a_device || !a_context || !a_renderDepth || a_nativeWidth == 0 || a_nativeHeight == 0) {
			return false;
		}

		D3D11_TEXTURE2D_DESC desired{};
		desired.Width = a_nativeWidth;
		desired.Height = a_nativeHeight;
		desired.MipLevels = 1;
		desired.ArraySize = 1;
		desired.Format = DXGI_FORMAT_R32_FLOAT;
		desired.SampleDesc.Count = 1;
		desired.Usage = D3D11_USAGE_DEFAULT;
		desired.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		bool recreate = !g_enbNativeDepth || !g_enbNativeDepthSRV || !g_enbNativeDepthRTV;
		if (!recreate) {
			D3D11_TEXTURE2D_DESC current{};
			g_enbNativeDepth->GetDesc(&current);
			recreate = current.Width != desired.Width ||
				current.Height != desired.Height ||
				current.Format != desired.Format;
		}

		if (recreate) {
			g_enbNativeDepthRTV = nullptr;
			g_enbNativeDepthSRV = nullptr;
			g_enbNativeDepth = nullptr;
			if (FAILED(a_device->CreateTexture2D(&desired, nullptr, g_enbNativeDepth.put())) ||
				FAILED(a_device->CreateShaderResourceView(g_enbNativeDepth.get(), nullptr, g_enbNativeDepthSRV.put())) ||
				FAILED(a_device->CreateRenderTargetView(g_enbNativeDepth.get(), nullptr, g_enbNativeDepthRTV.put()))) {
				g_enbNativeDepthRTV = nullptr;
				g_enbNativeDepthSRV = nullptr;
				g_enbNativeDepth = nullptr;
				return false;
			}
		}

		return ScaleCopyRenderTarget(
			a_device,
			a_context,
			a_renderDepth,
			g_enbNativeDepthRTV.get(),
			g_enbNativeDepth.get());
	}

	void ScaleProxyTargetsToOriginalNative(std::initializer_list<int> a_targets)
	{
		static auto rendererData = RE::BSGraphics::GetRendererData();
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
		auto* upscaling = Upscaling::GetSingleton();
		if (!context || !upscaling) {
			return;
		}

		auto* wrappedDevice = reinterpret_cast<ID3D11Device*>(rendererData->device);
		std::array<ENBScaleCopyJob, std::size(renderTargetsPatch)> jobs{};
		std::size_t jobCount = 0;
		for (const auto targetIndex : a_targets) {
			if (jobCount >= jobs.size()) {
				break;
			}
			if (targetIndex < 0 ||
				targetIndex >= static_cast<int>(std::size(upscaling->proxyRenderTargets))) {
				continue;
			}

			auto* sourceTexture = reinterpret_cast<ID3D11Texture2D*>(
				upscaling->proxyRenderTargets[targetIndex].texture);
			auto* sourceSRV = reinterpret_cast<ID3D11ShaderResourceView*>(
				upscaling->proxyRenderTargets[targetIndex].srView);
			auto* destinationTexture = reinterpret_cast<ID3D11Texture2D*>(
				upscaling->originalRenderTargets[targetIndex].texture);
			auto* destinationRTV = reinterpret_cast<ID3D11RenderTargetView*>(
				upscaling->originalRenderTargets[targetIndex].rtView);
			if (!sourceTexture || !sourceSRV || !destinationTexture || !destinationRTV) {
				continue;
			}

			ENBScaleCopyJob job{
				sourceTexture,
				sourceSRV,
				destinationRTV,
				destinationTexture
			};
			destinationTexture->GetDesc(&job.destinationDesc);
			jobs[jobCount++] = job;
		}

		const bool hasNativeContext = jobCount > 0 &&
			EnsureENBNativeD3D11Context(jobs[0].destinationTexture);
		const auto drawWrappedIndividually = [&]() {
			for (std::size_t i = 0; i < jobCount; ++i) {
				ScaleCopyRenderTarget(
					wrappedDevice,
					context,
					jobs[i].sourceSRV,
					jobs[i].destinationRTV,
					jobs[i].destinationTexture);
			}
		};

		// Keep the two outer calls visible to ENB: its context wrapper uses
		// the initial OM unbind as the deferred-composite handoff and reconstructs
		// its tracked state from the final restore. Native resources return the real
		// device/context, so the independent helper draws can be batched without ENB
		// mistaking their MRTs for engine passes. If that unwrap is unavailable, use
		// the original one-target helper sequence through the wrapper.
		ScopedENBProxyPromotionBoundary promotionBoundary(context);
		if (!promotionBoundary.IsActive()) {
			return;
		}
		if (!hasNativeContext || !CanBypassENBD3D11Context(context)) {
			drawWrappedIndividually();
			return;
		}

		ScopedENBScaleCopyState state(g_enbNativeDevice.get(), g_enbNativeContext.get());
		if (!state.IsActive()) {
			drawWrappedIndividually();
			return;
		}

		bool hasBoundOMUAV = false;
		if (!state.UsesContextState()) {
			std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> boundOMUAVs{};
			g_enbNativeContext->OMGetRenderTargetsAndUnorderedAccessViews(
				0,
				nullptr,
				nullptr,
				0,
				static_cast<UINT>(boundOMUAVs.size()),
				boundOMUAVs.data());
			hasBoundOMUAV = std::ranges::any_of(boundOMUAVs, [](const auto* a_view) {
				return a_view != nullptr;
			});
			for (auto* view : boundOMUAVs) {
				if (view) {
					view->Release();
				}
			}
		}

		// All proxy sources and native destinations are distinct in the normal
		// renderer allocation. Verify both read/write aliasing and unique outputs
		// before reordering independent promotions into equal-size MRT batches.
		bool independent = !hasBoundOMUAV;
		for (std::size_t i = 0; independent && i < jobCount; ++i) {
			for (std::size_t j = 0; j < jobCount; ++j) {
				if (jobs[i].sourceTexture == jobs[j].destinationTexture ||
					(i != j && jobs[i].destinationTexture == jobs[j].destinationTexture)) {
					independent = false;
					break;
				}
			}
		}
		if (!independent) {
			for (std::size_t i = 0; i < jobCount; ++i) {
				state.Draw(jobs[i].sourceSRV, jobs[i].destinationRTV, jobs[i].destinationTexture);
			}
			return;
		}

		std::array<bool, std::size(renderTargetsPatch)> promoted{};
		for (std::size_t i = 0; i < jobCount; ++i) {
			if (promoted[i]) {
				continue;
			}

			std::array<const ENBScaleCopyJob*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> batch{};
			std::array<std::size_t, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> batchIndices{};
			std::size_t batchSize = 0;
			for (std::size_t j = i;
				j < jobCount && batchSize < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
				++j) {
				if (promoted[j] ||
					jobs[j].destinationDesc.Width != jobs[i].destinationDesc.Width ||
					jobs[j].destinationDesc.Height != jobs[i].destinationDesc.Height ||
					jobs[j].destinationDesc.SampleDesc.Count != jobs[i].destinationDesc.SampleDesc.Count ||
					jobs[j].destinationDesc.SampleDesc.Quality != jobs[i].destinationDesc.SampleDesc.Quality) {
					continue;
				}
				batch[batchSize] = &jobs[j];
				batchIndices[batchSize] = j;
				++batchSize;
			}

			bool completed = false;
			if (batchSize > 1) {
				completed = state.DrawMRT(batch.data(), batchSize);
			}
			if (!completed) {
				for (std::size_t j = 0; j < batchSize; ++j) {
					const auto& job = *batch[j];
					state.Draw(job.sourceSRV, job.destinationRTV, job.destinationTexture);
				}
			}
			for (std::size_t j = 0; j < batchSize; ++j) {
				promoted[batchIndices[j]] = true;
			}
		}
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
		(void)a_upscaleMethod;
		return a_qualityMode;
	}

	const char* FeatureRequestName(Upscaling::FeatureRequest a_feature)
	{
		switch (a_feature) {
		case Upscaling::FeatureRequest::kDLSS:
			return "DLSS";
		case Upscaling::FeatureRequest::kFSR:
			return "FSR";
		case Upscaling::FeatureRequest::kDLSSG:
			return "DLSS-G";
		case Upscaling::FeatureRequest::kFSRFrameGeneration:
			return "FSR frame generation";
		default:
			return "unknown";
		}
	}

	uint64_t CurrentGameFrame()
	{
		static auto gameViewport = Util::State_GetSingleton();
		return gameViewport ? gameViewport->frameCount : 0;
	}

	uint64_t* GetTextureMemoryUpgradeLimit()
	{
		static REL::Relocation<uint64_t*> limit{
			REL::ID {
				610992,
				2666768
			}
		};
		return limit.get();
	}

	void ApplyTextureMemoryUpgradeReserve()
	{
		auto* limit = GetTextureMemoryUpgradeLimit();
		if (!limit || g_textureMemoryReserveApplied) {
			return;
		}

		const auto originalLimit = *limit;
		const auto reservedLimit = originalLimit > kTextureMemoryUpgradeReserveBytes ?
			originalLimit - kTextureMemoryUpgradeReserveBytes :
			originalLimit / 2;

		*limit = reservedLimit;
		g_textureMemoryReserveApplied = true;
		logger::info(
			"[Upscaling] Lowered engine texture memory upgrade limit from {} MiB to {} MiB (reserved {} MiB)",
			originalLimit / (1024ull * 1024ull),
			reservedLimit / (1024ull * 1024ull),
			kTextureMemoryUpgradeReserveBytes / (1024ull * 1024ull));
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
			currentDesc.MipLevels == a_desc.MipLevels &&
			currentDesc.ArraySize == a_desc.ArraySize &&
			currentDesc.Format == a_desc.Format &&
			currentDesc.SampleDesc.Count == a_desc.SampleDesc.Count &&
			currentDesc.SampleDesc.Quality == a_desc.SampleDesc.Quality &&
			currentDesc.Usage == a_desc.Usage &&
			currentDesc.BindFlags == a_desc.BindFlags &&
			currentDesc.CPUAccessFlags == a_desc.CPUAccessFlags &&
			currentDesc.MiscFlags == a_desc.MiscFlags;
	}

	void EnsureTexture2D(
		D3D11_TEXTURE2D_DESC a_desc,
		std::unique_ptr<Texture2D>& a_texture,
		bool a_createSRV,
		bool a_createUAV,
		bool a_createRTV = false)
	{
		a_desc.Usage = D3D11_USAGE_DEFAULT;
		a_desc.CPUAccessFlags = 0;

		if (!SharedTextureMatches(a_texture, a_desc)) {
			a_texture = std::make_unique<Texture2D>(a_desc);
		}

		if (a_createSRV && !a_texture->srv) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = a_desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			a_texture->CreateSRV(srvDesc);
		}

		if (a_createUAV && !a_texture->uav) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = a_desc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			a_texture->CreateUAV(uavDesc);
		}

		if (a_createRTV && !a_texture->rtv) {
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = a_desc.Format;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rtvDesc.Texture2D.MipSlice = 0;
			a_texture->CreateRTV(rtvDesc);
		}
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

	constexpr std::array<std::uint32_t, 2> kBokehTransientTargets{ 84, 85 };
	constexpr std::uint32_t kInvalidRenderTarget = std::numeric_limits<std::uint32_t>::max();

	thread_local std::uint32_t g_proxyImageSpaceDimensionsDepth = 0;
	thread_local std::uint32_t g_bokehProxyScopeDepth = 0;

	struct BokehProxyRenderTarget
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11Texture2D> copyTexture;
		winrt::com_ptr<ID3D11RenderTargetView> rtView;
		winrt::com_ptr<ID3D11ShaderResourceView> srView;
		winrt::com_ptr<ID3D11ShaderResourceView> copySRView;
		winrt::com_ptr<ID3D11UnorderedAccessView> uaView;

		[[nodiscard]] RE::BSGraphics::RenderTarget GetGameTarget() const
		{
			return {
				reinterpret_cast<REX::W32::ID3D11Texture2D*>(texture.get()),
				reinterpret_cast<REX::W32::ID3D11Texture2D*>(copyTexture.get()),
				reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(rtView.get()),
				reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(srView.get()),
				reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(copySRView.get()),
				reinterpret_cast<REX::W32::ID3D11UnorderedAccessView*>(uaView.get())
			};
		}
	};

	struct BokehPhysicalTargetOverride
	{
		std::uint32_t physicalIndex{ kInvalidRenderTarget };
		RE::BSGraphics::RenderTarget original{};
	};

	std::array<BokehProxyRenderTarget, kBokehTransientTargets.size()> g_bokehProxyTargets;
	thread_local std::array<BokehPhysicalTargetOverride, kBokehTransientTargets.size()> g_bokehTargetOverrides;

	[[nodiscard]] std::uint32_t GetPhysicalRenderTargetIndex(
		RE::BSGraphics::RenderTargetManager* a_manager,
		std::uint32_t a_logicalIndex)
	{
		// AE added three RenderTargetProperties entries before the ID table.
		// The CommonLib structure currently describes the OG 0xDC4 layout.
		const auto idTableOffset = REX::FModule::IsRuntimeOG() ? 0xDC4u : 0xDF4u;
		const auto* idTable = reinterpret_cast<const std::uint32_t*>(
			reinterpret_cast<const std::byte*>(a_manager) + idTableOffset);
		return idTable[a_logicalIndex];
	}

	[[nodiscard]] bool HasMatchingBokehProxy(
		const BokehProxyRenderTarget& a_proxy,
		const RE::BSGraphics::RenderTarget& a_source,
		const D3D11_TEXTURE2D_DESC& a_desiredDesc)
	{
		if (!a_proxy.texture ||
			static_cast<bool>(a_proxy.copyTexture) != (a_source.copyTexture != nullptr) ||
			static_cast<bool>(a_proxy.rtView) != (a_source.rtView != nullptr) ||
			static_cast<bool>(a_proxy.srView) != (a_source.srView != nullptr) ||
			static_cast<bool>(a_proxy.copySRView) != (a_source.copySRView != nullptr) ||
			static_cast<bool>(a_proxy.uaView) != (a_source.uaView != nullptr)) {
			return false;
		}

		D3D11_TEXTURE2D_DESC proxyDesc{};
		a_proxy.texture->GetDesc(&proxyDesc);
		return std::memcmp(&proxyDesc, &a_desiredDesc, sizeof(proxyDesc)) == 0;
	}

	[[nodiscard]] bool CreateBokehProxyRenderTarget(
		std::size_t a_slot,
		const RE::BSGraphics::RenderTarget& a_source,
		std::uint32_t a_width,
		std::uint32_t a_height)
	{
		if (a_slot >= g_bokehProxyTargets.size() || !a_source.texture || a_width == 0 || a_height == 0) {
			return false;
		}

		auto* sourceTexture = reinterpret_cast<ID3D11Texture2D*>(a_source.texture);
		D3D11_TEXTURE2D_DESC textureDesc{};
		sourceTexture->GetDesc(&textureDesc);
		textureDesc.Width = a_width;
		textureDesc.Height = a_height;

		if (HasMatchingBokehProxy(g_bokehProxyTargets[a_slot], a_source, textureDesc)) {
			return true;
		}

		static auto* rendererData = RE::BSGraphics::GetRendererData();
		auto* device = reinterpret_cast<ID3D11Device*>(rendererData->device);
		if (!device) {
			return false;
		}

		BokehProxyRenderTarget proxy;
		auto result = device->CreateTexture2D(&textureDesc, nullptr, proxy.texture.put());
		if (FAILED(result)) {
			logger::warn(
				"[Upscaling] Failed to create Bokeh RT{} proxy texture {}x{}: 0x{:08X}",
				kBokehTransientTargets[a_slot],
				a_width,
				a_height,
				static_cast<std::uint32_t>(result));
			return false;
		}

		if (a_source.rtView) {
			D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
			reinterpret_cast<ID3D11RenderTargetView*>(a_source.rtView)->GetDesc(&viewDesc);
			result = device->CreateRenderTargetView(proxy.texture.get(), &viewDesc, proxy.rtView.put());
			if (FAILED(result)) {
				return false;
			}
		}

		if (a_source.srView) {
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			reinterpret_cast<ID3D11ShaderResourceView*>(a_source.srView)->GetDesc(&viewDesc);
			result = device->CreateShaderResourceView(proxy.texture.get(), &viewDesc, proxy.srView.put());
			if (FAILED(result)) {
				return false;
			}
		}

		if (a_source.uaView) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
			reinterpret_cast<ID3D11UnorderedAccessView*>(a_source.uaView)->GetDesc(&viewDesc);
			result = device->CreateUnorderedAccessView(proxy.texture.get(), &viewDesc, proxy.uaView.put());
			if (FAILED(result)) {
				return false;
			}
		}

		if (a_source.copyTexture) {
			D3D11_TEXTURE2D_DESC copyDesc{};
			reinterpret_cast<ID3D11Texture2D*>(a_source.copyTexture)->GetDesc(&copyDesc);
			copyDesc.Width = a_width;
			copyDesc.Height = a_height;
			result = device->CreateTexture2D(&copyDesc, nullptr, proxy.copyTexture.put());
			if (FAILED(result)) {
				return false;
			}

			if (a_source.copySRView) {
				D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
				reinterpret_cast<ID3D11ShaderResourceView*>(a_source.copySRView)->GetDesc(&viewDesc);
				result = device->CreateShaderResourceView(proxy.copyTexture.get(), &viewDesc, proxy.copySRView.put());
				if (FAILED(result)) {
					return false;
				}
			}
		}

#ifndef NDEBUG
		const auto debugName = std::format("Bokeh RT{} proxy", kBokehTransientTargets[a_slot]);
		proxy.texture->SetPrivateData(
			WKPDID_D3DDebugObjectName,
			static_cast<UINT>(debugName.size()),
			debugName.data());
#endif

		g_bokehProxyTargets[a_slot] = std::move(proxy);
		return true;
	}

	void OverrideBokehTransientTarget(
		RE::BSGraphics::RenderTargetManager* a_manager,
		std::uint32_t a_logicalIndex)
	{
		if (g_bokehProxyScopeDepth == 0 || !a_manager) {
			return;
		}

		const auto target = std::find(
			kBokehTransientTargets.begin(),
			kBokehTransientTargets.end(),
			a_logicalIndex);
		if (target == kBokehTransientTargets.end()) {
			return;
		}

		const auto slot = static_cast<std::size_t>(target - kBokehTransientTargets.begin());
		if (g_bokehTargetOverrides[slot].physicalIndex != kInvalidRenderTarget) {
			return;
		}

		static auto* rendererData = RE::BSGraphics::GetRendererData();
		const auto physicalIndex = GetPhysicalRenderTargetIndex(a_manager, a_logicalIndex);
		if (physicalIndex == kInvalidRenderTarget || physicalIndex >= std::size(rendererData->renderTargets)) {
			return;
		}

		auto& physicalTarget = rendererData->renderTargets[physicalIndex];
		const auto& properties = a_manager->renderTargetData[a_logicalIndex];
		if (!CreateBokehProxyRenderTarget(slot, physicalTarget, properties.width, properties.height)) {
			return;
		}

		D3D11_TEXTURE2D_DESC physicalDesc{};
		reinterpret_cast<ID3D11Texture2D*>(physicalTarget.texture)->GetDesc(&physicalDesc);
		if (physicalDesc.Width == properties.width && physicalDesc.Height == properties.height) {
			return;
		}

		g_bokehTargetOverrides[slot].physicalIndex = physicalIndex;
		g_bokehTargetOverrides[slot].original = physicalTarget;
		physicalTarget = g_bokehProxyTargets[slot].GetGameTarget();
	}

	void ResetBokehTransientTargets()
	{
		static auto* rendererData = RE::BSGraphics::GetRendererData();
		for (std::size_t slot = g_bokehTargetOverrides.size(); slot-- > 0;) {
			auto& targetOverride = g_bokehTargetOverrides[slot];
			if (targetOverride.physicalIndex == kInvalidRenderTarget ||
				targetOverride.physicalIndex >= std::size(rendererData->renderTargets)) {
				targetOverride = {};
				continue;
			}

			auto& physicalTarget = rendererData->renderTargets[targetOverride.physicalIndex];
			if (physicalTarget.texture == g_bokehProxyTargets[slot].GetGameTarget().texture) {
				physicalTarget = targetOverride.original;
			}
			targetOverride = {};
		}
	}

	class ScopedBokehProxyTargets
	{
	public:
		ScopedBokehProxyTargets() : active_(g_proxyImageSpaceDimensionsDepth != 0)
		{
			if (active_) {
				++g_bokehProxyScopeDepth;
			}
		}

		~ScopedBokehProxyTargets()
		{
			if (!active_ || g_bokehProxyScopeDepth == 0) {
				return;
			}

			if (--g_bokehProxyScopeDepth == 0) {
				ResetBokehTransientTargets();
			}
		}

	private:
		bool active_{ false };
	};

	ID3DBlob* CompileInlineShader(const char* a_source, const char* a_entry, const char* a_target)
	{
		constexpr uint32_t flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		winrt::com_ptr<ID3DBlob> shader;
		winrt::com_ptr<ID3DBlob> errors;
		const auto result = D3DCompile(
			a_source,
			std::strlen(a_source),
			nullptr,
			nullptr,
			nullptr,
			a_entry,
			a_target,
			flags,
			0,
			shader.put(),
			errors.put());
		if (FAILED(result)) {
			logger::warn("[Upscaling] Inline shader compilation failed:\n{}", errors ? static_cast<char*>(errors->GetBufferPointer()) : "Unknown error");
			return nullptr;
		}

		return shader.detach();
	}
}

void RestoreDynamicResolutionViewportDefaultFlag(RE::BSGraphics::RenderTargetManager* a_renderTargetManager);

class ScopedProxyImageSpaceDimensions
{
public:
	ScopedProxyImageSpaceDimensions(
		RE::BSGraphics::State* a_state,
		float a_widthRatio,
		float a_heightRatio) :
		state_(a_state)
	{
		if (!state_ || !(a_widthRatio > 0.0f && a_widthRatio <= 1.0f) ||
			!(a_heightRatio > 0.0f && a_heightRatio <= 1.0f) ||
			(a_widthRatio == 1.0f && a_heightRatio == 1.0f)) {
			state_ = nullptr;
			return;
		}

		backBufferWidth_ = state_->backBufferWidth;
		backBufferHeight_ = state_->backBufferHeight;
		screenWidth_ = state_->screenWidth;
		screenHeight_ = state_->screenHeight;

		state_->backBufferWidth = ScaleDimension(backBufferWidth_, a_widthRatio);
		state_->backBufferHeight = ScaleDimension(backBufferHeight_, a_heightRatio);
		state_->screenWidth = ScaleDimension(screenWidth_, a_widthRatio);
		state_->screenHeight = ScaleDimension(screenHeight_, a_heightRatio);
		++g_proxyImageSpaceDimensionsDepth;
	}

	~ScopedProxyImageSpaceDimensions()
	{
		if (!state_) {
			return;
		}

		state_->backBufferWidth = backBufferWidth_;
		state_->backBufferHeight = backBufferHeight_;
		state_->screenWidth = screenWidth_;
		state_->screenHeight = screenHeight_;
		if (g_proxyImageSpaceDimensionsDepth != 0) {
			--g_proxyImageSpaceDimensionsDepth;
		}
	}

	ScopedProxyImageSpaceDimensions(const ScopedProxyImageSpaceDimensions&) = delete;
	ScopedProxyImageSpaceDimensions& operator=(const ScopedProxyImageSpaceDimensions&) = delete;

private:
	static std::uint32_t ScaleDimension(std::uint32_t a_dimension, float a_ratio)
	{
		return std::max(1u, static_cast<std::uint32_t>(static_cast<float>(a_dimension) * a_ratio));
	}

	RE::BSGraphics::State* state_{ nullptr };
	std::uint32_t backBufferWidth_{ 0 };
	std::uint32_t backBufferHeight_{ 0 };
	std::uint32_t screenWidth_{ 0 };
	std::uint32_t screenHeight_{ 0 };
};

/** @brief Replace Bokeh's transient pooled targets after the engine resolves their physical slots. */
struct BSGraphics_RenderTargetManager_AcquireRenderTarget_BokehProxy
{
	static void thunk(RE::BSGraphics::RenderTargetManager* This, int a_target)
	{
		func(This, a_target);
		if (a_target >= 0) {
			OverrideBokehTransientTarget(This, static_cast<std::uint32_t>(a_target));
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Limit transient target replacement to the Bokeh effect's four passes. */
struct ImageSpaceEffectBokehDepthOfField_Render_BokehProxy
{
	static void thunk(void* This, RE::BSTriShape* a_geometry, std::uint32_t a_param, void* a_effectDesc)
	{
		// The ENB path promotes Bokeh to native geometry/constants. Its engine
		// implementation then resolves DepthBuffer 1 through RendererData, after
		// the early image-space range has restored the dynamic-resolution depth.
		// Expose the expanded native-depth bridge only for Bokeh's four passes.
		const bool useENBNativeDepth =
			g_enbNativeImageSpaceParamScopeDepth > 0 &&
			g_enbNativeDepthSRV &&
			g_enbNativeDepthFrame == CurrentGameFrame();
		const ScopedFalloutMainDepthBinding nativeDepth(useENBNativeDepth);
		const ScopedBokehProxyTargets proxyTargets;
		func(This, a_geometry, a_param, a_effectDesc);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

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

		const bool requiresOverride = IsDynamicResolutionScaled();

		auto originalOffsetX = gameViewport->offsetX;
		auto originalOffsetY = gameViewport->offsetY;
		const auto originalFrameBufferViewport = gameViewport->frameBufferViewport;

		// Disable removal of jitter in some passes
		if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDisabled){
			gameViewport->offsetX = originalOffsetX;
			gameViewport->offsetY = originalOffsetY;
		}

		const auto ratios = GetDynamicResolutionRatios();
		originalDynamicHeightRatio = ratios.height;
		originalDynamicWidthRatio = ratios.width;
		const auto frameDynamicHeightRatio = originalDynamicHeightRatio;
		const auto frameDynamicWidthRatio = originalDynamicWidthRatio;
		const bool enbCompatibilityActive =
			requiresOverride && IsENBSRCompatibilityActive(upscaling->upscaleMethod);

		if (ShouldBypassDynamicResolutionHooksForInactiveENB()) {
			func(This, a2, a3, a4, a5);
			gameViewport->offsetX = originalOffsetX;
			gameViewport->offsetY = originalOffsetY;
			return;
		}

		if (requiresOverride) {
			if (enbCompatibilityActive) {
				const std::initializer_list<int> kENBSpatialBridgeTargets{
					0, 20, 57, 24, 25, 23, 58, 59, 28, 3, 9, 60, 61, 4, 1, 2, 36, 37, 22, 10, 11, 7, 8, 64, 14, 16
				};

				upscaling->OverrideDepth(true);
				static auto rendererData = RE::BSGraphics::GetRendererData();
				auto* device = reinterpret_cast<ID3D11Device*>(rendererData->device);
				auto* context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
				const bool nativeDepthReady = PrepareENBNativeDepth(
					device,
					context,
					upscaling->depthOverrideTexture ? upscaling->depthOverrideTexture->srv.get() : nullptr,
					static_cast<uint32_t>(gameViewport->screenWidth),
					static_cast<uint32_t>(gameViewport->screenHeight));
				g_enbNativeDepthFrame = nativeDepthReady ?
					CurrentGameFrame() :
					std::numeric_limits<std::uint64_t>::max();

				// Promote only after the deferred/ENB composites are complete. Copy the
				// current render rectangles directly: the former override/reset pair did
				// no rendering between its swaps, so every other state change cancelled.
				// P29 remains render-sized because motion-vector dilation consumes it.
				CopyENBInputsToRenderProxies(kENBSpatialBridgeTargets);
				RestoreDynamicResolutionViewportDefaultFlag(renderTargetManager);
				ScaleProxyTargetsToOriginalNative(kENBSpatialBridgeTargets);
				if (nativeDepthReady) {
					rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth =
						reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(g_enbNativeDepthSRV.get());
				}

				SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);
				ApplyFullFrameViewport();

				const bool shouldPrimeENBPrepass =
					g_enbTextureOriginalSRVAddress != 0 &&
					TryGetENBBool(ENBBoolSetting::kPrepass).value_or(false);
				if (shouldPrimeENBPrepass) {
					auto* nativeScene = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[4].srView);
					PrimeENBPrepassNativeColor(device, context, nativeScene);
				}
				{
					// ENB otherwise captures the native DSV allocation 1:1 and
					// leaves its dynamic-resolution render rectangle unexpanded.
					const ScopedENBTextureDepthBinding bindENBTextureDepth(nativeDepthReady);
					const ScopedENBPrepassDepthBridgeActivation bridgeENBPrepassDepth(shouldPrimeENBPrepass);
					const ScopedENBNativeImageSpaceParams forceNativeImageSpaceParams;
					func(This, a2, a3, a4, a5);
				}
				upscaling->ResetDepth();
				SetDynamicResolutionRatio(renderTargetManager, frameDynamicWidthRatio, frameDynamicHeightRatio);
				originalDynamicWidthRatio = frameDynamicWidthRatio;
				originalDynamicHeightRatio = frameDynamicHeightRatio;
				gameViewport->frameBufferViewport = originalFrameBufferViewport;
				ApplyCurrentViewportDefault(renderTargetManager);

				gameViewport->offsetX = originalOffsetX;
				gameViewport->offsetY = originalOffsetY;
				return;
			}

			// Preserve the original non-ENB dynamic-resolution path.
			func(This, 0, 3, 1, 1);

			upscaling->OverrideRenderTargets({ 1, 4, 29, 16 });
			upscaling->OverrideDepth(true);
			SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);

			{
				// The proxy targets and their RenderTargetProperties are render-sized, but
				// several effects read State's allocation dimensions directly. Bokeh DOF,
				// for example, derives its vertex constants from backBufferWidth/Height.
				// Keep those constants in the same domain while the proxy range executes.
				const ScopedProxyImageSpaceDimensions proxyDimensions(
					gameViewport,
					frameDynamicWidthRatio,
					frameDynamicHeightRatio);
				func(This, 4, 13, 1, 1);
			}
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
		static auto gameViewport = Util::State_GetSingleton();
		const auto originalFrameBufferViewport = gameViewport->frameBufferViewport;

		const auto ratios = GetDynamicResolutionRatios();
		originalDynamicHeightRatio = ratios.height;
		originalDynamicWidthRatio = ratios.width;
		const auto frameDynamicHeightRatio = originalDynamicHeightRatio;
		const auto frameDynamicWidthRatio = originalDynamicWidthRatio;

		if (ShouldBypassDynamicResolutionHooksForInactiveENB()) {
			func(This, a2, a3, a4, a5);
			if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDisabled) {
				upscaling->Upscale(static_cast<int>(a5));
			} else {
				upscaling->CaptureDLSSGInputs(static_cast<int>(a5));
			}
			return;
		}

		if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDisabled &&
			IsDynamicResolutionScaled()) {
			if (IsENBSRCompatibilityActive(upscaling->upscaleMethod)) {
				SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);
				ApplyFullFrameViewport();
				{
					const ScopedENBNativeImageSpaceParams forceNativeImageSpaceParams;
					func(This, a2, a3, a4, a5);
				}
				SetDynamicResolutionRatio(renderTargetManager, frameDynamicWidthRatio, frameDynamicHeightRatio);
				originalDynamicWidthRatio = frameDynamicWidthRatio;
				originalDynamicHeightRatio = frameDynamicHeightRatio;
				gameViewport->frameBufferViewport = originalFrameBufferViewport;
				ApplyCurrentViewportDefault(renderTargetManager);
				// ENB's final imagespace work is complete when this late effect
				// range returns. Upscale and clear here so Fallout's following
				// Interface3DPostAARenderFn draws a native-resolution overlay for
				// the D3D12 present composite instead of being baked into DLSS.
				upscaling->Upscale(static_cast<int>(a5));
				return;
			}

			upscaling->OverrideRenderTargets({ static_cast<int>(a4) });
			upscaling->OverrideDepth(true);
			SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);
			{
				const ScopedProxyImageSpaceDimensions proxyDimensions(
					gameViewport,
					frameDynamicWidthRatio,
					frameDynamicHeightRatio);
				func(This, a2, a3, a4, a5);
			}
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

		const auto ratios = GetDynamicResolutionRatios();
		originalDynamicHeightRatio = ratios.height;
		originalDynamicWidthRatio = ratios.width;

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

void RestoreDynamicResolutionViewportDefaultFlag(RE::BSGraphics::RenderTargetManager* a_renderTargetManager)
{
	// The removed override/reset pair ended by restoring this single engine byte.
	DrawWorld_Imagespace_SetUseDynamicResolutionViewportAsDefaultViewport::func(
		a_renderTargetManager,
		true);
}

struct BSImagespaceShader_Render_ENBFinalComposite
{
	static void thunk(void* This, void* a_geometry, void* a_shaderParams)
	{
		if (g_enbNativeImageSpaceParamScopeDepth <= 0) {
			func(This, a_geometry, a_shaderParams);
			return;
		}

		const bool isHDRFinalComposite = IsENBHDRFinalCompositeEffect(This);
		const bool isRefractionComposite = This && This == g_enbRefractionCompositeEffect;
		const bool isNativeEffectShader = IsENBNativeImageSpaceShader(This);
		if (!isHDRFinalComposite && !isRefractionComposite && !isNativeEffectShader) {
			func(This, a_geometry, a_shaderParams);
			return;
		}

		// RenderEffect selects the alternate +0x40 geometry in the tiled path,
		// even when the effect is marked non-dynamic. BSImagespaceShader only
		// substitutes the normal +0x38 dynamic geometry, so pass the manager's
		// native +0x28 geometry explicitly for the ENB-intercepted draw. Preserve
		// the HDR final composite's original dynamic-resolution flag: ENB copies
		// its seven-vector game parameter block and enbeffect.fx consumes
		// Params01[6].y as the bloom scale. Only the geometry needs promotion.
		auto* nativeGeometry = GetNativeImageSpaceGeometry();
		if (isHDRFinalComposite) {
			func(This, nativeGeometry ? nativeGeometry : a_geometry, a_shaderParams);
		} else {
			const ScopedBSImagespaceShaderNativeParams forceNativeParams(This);
			func(This, nativeGeometry ? nativeGeometry : a_geometry, a_shaderParams);
		}

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

bool frameGenerationFirstPersonAlphaFix = false;

/** @brief Hook forward rendering to capture frame-generation motion/depth inputs */
struct DrawWorld_FrameGenerationForward
{
	static void thunk(void* This)
	{
		func(This);

		if (!frameGenerationFirstPersonAlphaFix) {
			Upscaling::GetSingleton()->CopyFrameGenerationBuffers();
		}

		frameGenerationFirstPersonAlphaFix = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook first-person alpha rendering to repair its frame-generation motion/depth inputs */
struct DrawWorld_FrameGenerationFirstPersonAlpha
{
	static void thunk(void* This)
	{
		auto upscaling = Upscaling::GetSingleton();
		upscaling->PreFrameGenerationAlpha();
		func(This);
		frameGenerationFirstPersonAlphaFix = upscaling->PostFrameGenerationAlpha();
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
		const bool requiresOverride = IsDynamicResolutionScaled();

		const auto ratios = GetDynamicResolutionRatios();
		originalDynamicHeightRatio = ratios.height;
		originalDynamicWidthRatio = ratios.width;

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

/** @brief Preserve the established native deferred-composite path for ENB SSS. */
struct DrawWorld_DeferredComposite_RenderPassImmediately
{
	static void thunk(RE::BSRenderPass* This, uint a2, bool a3)
	{
		auto upscaling = Upscaling::GetSingleton();
		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		const bool requiresOverride = IsDynamicResolutionScaled();

		const auto ratios = GetDynamicResolutionRatios();
		originalDynamicHeightRatio = ratios.height;
		originalDynamicWidthRatio = ratios.width;
		const bool enbCompatibilityActive = IsENBSRCompatibilityActive(upscaling->upscaleMethod);
		const bool useENBSSSProjection = requiresOverride && enbCompatibilityActive &&
			IsENBSubSurfaceScatteringActive();
		if (!useENBSSSProjection) {
			ResetENBSSSProjectionSubstitution();
		}

		if (ShouldBypassDynamicResolutionHooksForInactiveENB()) {
			func(This, a2, a3);
			return;
		}
		if (useENBSSSProjection) {
			// Preserve Fallout's prepared DRS pass exactly. Runtime tracing confirmed
			// that the pass and all allocation-domain 2D samples must remain intact.
			// Discover the confirmed wrapper once, then leave its alternate shader
			// substituted while this compatibility path remains active.
			const bool discoveryArmed = BeginENBSSSProjectionDiscovery();
			func(This, a2, a3);
			if (discoveryArmed) {
				RemoveENBSSSProjectionDiscoveryHook();
			}
			return;
		}
		if (requiresOverride) {
			upscaling->OverrideRenderTargets({20, 25, 57, 24, 23, 58, 59, 3, 9, 60, 61, 28});
			upscaling->OverrideDepth(true);
			SetDynamicResolutionRatio(renderTargetManager, 1.0f, 1.0f);
			if (enbCompatibilityActive) {
				ApplyCurrentViewportDefault(renderTargetManager);
			}
		}
		func(This, a2, a3);

		if (requiresOverride) {
			upscaling->ResetRenderTargets({4});
			upscaling->ResetDepth();
			SetDynamicResolutionRatio(renderTargetManager, originalDynamicWidthRatio, originalDynamicHeightRatio);
			if (enbCompatibilityActive) {
				ApplyCurrentViewportDefault(renderTargetManager);
			}
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};


struct DrawWorld_DeferredComposite_RenderPassImmediately_First
{
	static void thunk(RE::BSRenderPass* This, uint a2, bool a3)
	{
		auto upscaling = Upscaling::GetSingleton();
		const bool enbCompatibilityActive =
			IsDynamicResolutionScaled() &&
			IsENBSRCompatibilityActive(upscaling->upscaleMethod);

		if (!enbCompatibilityActive) {
			func(This, a2, a3);
			return;
		}

		const ScopedENBPrimaryCompositeScope primaryCompositeScope;
		func(This, a2, a3);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct ImageSpaceManager_RenderEffect
{
	static void thunk(void* This, void* a_effect, int a_targetA, int a_targetB, void* a_params)
	{
		// This detour observes every Fallout image-space effect. The ENB tracking
		// work is useful only inside the two native ENB ranges; avoid the effect
		// array scan and four scoped trackers on the ordinary render path.
		if (g_enbNativeImageSpaceParamScopeDepth <= 0) {
			func(This, a_effect, a_targetA, a_targetB, a_params);
			return;
		}

		const auto effectIndex = FindEffectIndex(This, a_effect);
		LogENBNativeImageSpaceEffect(a_effect, effectIndex, a_targetA, a_targetB);
		const ScopedImageSpaceEffectNativeParams forceNativeParams(a_effect, effectIndex);
		const ScopedENBHDRFinalCompositeEffects hdrFinalCompositeEffects(a_effect, effectIndex);
		const ScopedENBRefractionCompositeEffect refractionCompositeEffect(a_effect, effectIndex);
		const ScopedENBNativeImageSpaceShaders nativeImageSpaceShaders(a_effect, effectIndex);
		func(This, a_effect, a_targetA, a_targetB, a_params);
	}

	static int32_t FindEffectIndex(const void* a_manager, const void* a_effect)
	{
		if (!a_manager || !a_effect) {
			return -1;
		}

		const auto effectArray = *reinterpret_cast<const void* const* const*>(reinterpret_cast<const std::byte*>(a_manager) + 0x18);
		if (!effectArray) {
			return -1;
		}

		const auto count = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const std::byte*>(a_manager) + 0x22);
		const auto boundedCount = std::min<uint16_t>(count, 0x48);
		for (uint16_t i = 0; i < boundedCount; ++i) {
			if (effectArray[i] == a_effect) {
				return i;
			}
		}

		return -1;
	}

	static inline REL::Relocation<decltype(thunk)> func;
};

// AE's RenderEffectRange calls the index-based worker directly. It does not
// pass through the pointer-based overload above (ID 2316597).
struct ImageSpaceManager_RenderEffectByIndexAE
{
	static void thunk(void* This, uint32_t a_effectIndex, int a_targetA, int a_targetB, void* a_params)
	{
		if (g_enbNativeImageSpaceParamScopeDepth <= 0) {
			func(This, a_effectIndex, a_targetA, a_targetB, a_params);
			return;
		}

		void* effect = nullptr;
		if (This) {
			const auto effectArray = *reinterpret_cast<void***>(reinterpret_cast<std::byte*>(This) + kImageSpaceEffectListOffset);
			const auto effectCount = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const std::byte*>(This) + kImageSpaceEffectCountOffset);
			if (effectArray && a_effectIndex < effectCount) {
				effect = effectArray[a_effectIndex];
			}
		}

		LogENBNativeImageSpaceEffect(effect, static_cast<int32_t>(a_effectIndex), a_targetA, a_targetB);
		const ScopedImageSpaceEffectNativeParams forceNativeParams(effect, static_cast<int32_t>(a_effectIndex));
		const ScopedENBHDRFinalCompositeEffects hdrFinalCompositeEffects(effect, static_cast<int32_t>(a_effectIndex));
		const ScopedENBRefractionCompositeEffect refractionCompositeEffect(effect, static_cast<int32_t>(a_effectIndex));
		const ScopedENBNativeImageSpaceShaders nativeImageSpaceShaders(effect, static_cast<int32_t>(a_effectIndex));
		func(This, a_effectIndex, a_targetA, a_targetB, a_params);
	}

	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for BSImagespaceShaderLensFlare with depth override */
struct BSImagespaceShaderLensFlare_RenderLensFlare
{
	static void thunk(RE::NiCamera* a_camera)
	{
		auto upscaling = Upscaling::GetSingleton();

		const bool requiresOverride = IsDynamicResolutionScaled();

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
	static bool thunk(RE::BSShader* This, uint a2, uint a3, uint a4, uint a5)
	{
		const bool result = func(This, a2, a3, a4, a5);
		if (result) {
			Upscaling::GetSingleton()->PatchSSRShader();
		}
		return result;
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
	// Fixed Fallout 4 entry points use explicit gateway prologues. These lengths
	// are instruction-boundary sizes verified in both OG 1.10.163 and AE 1.11.221.
	stl::detour_thunk_gateway<Interface3D_Renderer_Create>(
		REL::ID{ 88488, 2222519 },
		5,
		"Interface3D::Renderer::Create");

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

	// Capture first-person-alpha-safe motion vectors and depth for frame generation.
	stl::detour_thunk_gateway<DrawWorld_FrameGenerationForward>(
		REL::ID{ 656535, 2318315 },
		5,
		"DrawWorld::FrameGenerationForward");
	// ForwardAlphaImpl + 0x253 (OG) / +0x53D (AE) calls the first-person
	// accumulator's RenderAlphaGeometry; it is not a screen-space reticle draw.
	stl::write_thunk_call<DrawWorld_FrameGenerationFirstPersonAlpha>(
		REL::ID{ 338205, 2318315 }.address() + (isOG ? 0x253 : 0x53D));

	// This hook also owns native-AA evaluation and frame-generation input capture.
	stl::write_thunk_call<DrawWorld_Imagespace_LateRenderEffectRange>(
		REL::ID{ 587723, 2318322 }.address() + (isOG ? 0xD3 : 0xB7));

	// Fix dynamic resolution for BSDFComposite
	stl::write_thunk_call<DrawWorld_DeferredComposite_RenderPassImmediately>(
		REL::ID{ 728427, 2318313 }.address() + (isOG ? 0x8DC : 0x915));
	if (enbLoaded) {
		// The first BSDF composite RenderPassImmediately call is the primary P3
		// handoff on both runtimes; AE gained one byte before the call site.
		stl::write_thunk_call<DrawWorld_DeferredComposite_RenderPassImmediately_First>(
			REL::ID{ 728427, 2318313 }.address() + (isOG ? 0x1F4 : 0x1F5));
		// AE's range loop and Addictol's bDofFix use the index-based worker
		// (2316595). OG uses the pointer-based overload (325252). Chain either
		// entry so an already-installed Addictol detour remains the next call.
		if (isOG) {
			stl::detour_thunk_gateway<ImageSpaceManager_RenderEffect>(
				REL::ID{ 325252 },
				7,
				"ImageSpaceManager::RenderEffect");
		} else {
			stl::detour_thunk_gateway<ImageSpaceManager_RenderEffectByIndexAE>(
				REL::ID{ 2316595 },
				7,
				"ImageSpaceManager::RenderEffectByIndex");
		}
		stl::detour_thunk_gateway<BSImagespaceShader_Render_ENBFinalComposite>(
			REL::ID{ 1388477, 2319297 },
			5,
			"BSImagespaceShader::RenderENBFinalComposite");
	}
	// Fix dynamic resolution for Lens Flare visibility
	stl::detour_thunk_gateway<BSImagespaceShaderLensFlare_RenderLensFlare>(
		REL::ID{ 676108, 2317547 },
		6,
		"BSImagespaceShaderLensFlare::RenderLensFlare");

	// Fix dynamic resolution for Screenspace Reflections
	stl::write_thunk_call<BSImagespaceShaderSSLRRaytracing_SetupTechnique_BeginTechnique>(
		REL::ID{ 779077, 2317302 }.address() + 0x1C);

	// Fix dynamic resolution for post processing
	stl::write_thunk_call<DrawWorld_Imagespace_RenderEffectRange>(
		REL::ID{ 587723, 2318322 }.address() + (isOG ? 0x9F : 0x83));

	// Bokeh's RT84/RT85 are logical transient targets backed by native-sized
	// pooled renderer slots. Match those physical allocations to the proxy
	// metadata while Bokeh runs so its normalized blur passes cannot scale the
	// active render rectangle a second time.
	stl::detour_thunk_gateway<BSGraphics_RenderTargetManager_AcquireRenderTarget_BokehProxy>(
		REL::ID{ 1468639, 2277219 },
		6,
		"BSGraphics::RenderTargetManager::AcquireRenderTarget");
	stl::detour_thunk_gateway<ImageSpaceEffectBokehDepthOfField_Render_BokehProxy>(
		REL::ID{ 1108909, 2318617 },
		5,
		"ImageSpaceEffectBokehDepthOfField::Dispatch");

	// Fix dynamic resolution for HBAO
	stl::write_thunk_call<DrawWorld_Render_PreUI_NVHBAO>(
		REL::ID{ 984743, 2318321 }.address() + (isOG ? 0x1BA : 0x397));

	// Fix VATs line thickness
	stl::write_thunk_call<ImageSpaceEffectVatsTarget_UpdateParams_SetPixelConstant>(
		REL::ID{ 1042583, 2317983 }.address() + (isOG ? 0xBB : 0x110));

	// Fix jitter in LoadingMenu
	stl::write_thunk_call<LoadingMenu_Render_UpdateTemporalData>(
		REL::ID{ 135719, 2249225 }.address() + (isOG ? 0x2BD : 0x275));

	// Fix dynamic resolution after upscaling
	stl::detour_thunk_gateway<DrawWorld_Imagespace>(
		REL::ID{ 587723, 2318322 },
		5,
		"DrawWorld::Imagespace");
	if (enbLoaded) {
		ResolveENBRenderResolutionGlobals();
		InstallENBScreenEffectRenderHooks();
		ResolveENBPrepassResourceSlots();
	}
}

void Upscaling::InstallHighFPSPhysicsFixCompatibility()
{
	static bool installed = false;
	if (installed) {
		return;
	}

	const REL::ID servingThreadFunction{ 132841, 2227631 };
	if (REX::FModule::IsRuntimeOG()) {
		if (!HasHFPFDisableLoadingAnimationPatch(
				servingThreadFunction.address(),
				kHFPFDisableLoadingAnimationPatchOffsetOG,
				kHFPFDisableLoadingAnimationPatchOG)) {
			return;
		}

		stl::detour_thunk_gateway<JobListManager_ServingThread_DisplayLoadingScreen>(
			REL::ID{ 132841 },
			5,
			"JobListManager::ServingThread::DisplayLoadingScreen");
		installed = true;
		return;
	}

	if (InstallHFPFAELoadingLoopCompatibility(servingThreadFunction.address())) {
		logger::info(
			"[Upscaling] Installed High FPS Physics Fix loading-loop compatibility for AE");
		installed = true;
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
	const auto previousQualityMode = settings.qualityMode;
	const auto previousFrameGenerationMode = settings.frameGenerationMode;
	const auto previousDLSSGGeneratedFrames = settings.dlssgGeneratedFrames;
	const auto previousDynamicMFGEnabled = settings.dynamicMFGEnabled;
	const auto previousDLSSModelPreset = settings.dlssModelPreset;
	const auto previousDLSSNREnabled = settings.dlssNREnabled;
	const auto previousDLSSNRPerformanceMode = settings.dlssNRPerformanceMode;
	const auto previousDLSSNRPreset = settings.dlssNRPreset;
	const auto previousDLSSNRStyle = settings.dlssNRStyle;
	const auto previousDLSSNRUseAutoMask = settings.dlssNRUseAutoMask;
	const auto previousDLSSNRIntensity = settings.dlssNRIntensity;
	const auto previousDLSSNRLocalToneStrength = settings.dlssNRLocalToneStrength;
	const auto previousDLSSNRLocalStructureStrength = settings.dlssNRLocalStructureStrength;
	const auto previousDLSSNRGlobalToneStrength = settings.dlssNRGlobalToneStrength;
	const auto previousDLSSNRSkinStructureStrength = settings.dlssNRSkinStructureStrength;
	const bool previousImageSpaceEffectLog = settings.imageSpaceEffectLog != 0;

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(kSettingsPath);
	RememberSettingsWriteTime();
	
	settings.upscaleMethodPreference = static_cast<uint>(ini.GetLongValue("Settings", "iUpscaleMethodPreference", 2));
	settings.qualityMode = static_cast<uint>(ini.GetLongValue("Settings", "iQualityMode", 1));
	settings.frameGenerationMode = static_cast<uint>(ini.GetLongValue("Settings", "iFrameGenerationMode", 0));
	settings.dlssgGeneratedFrames = static_cast<uint>(ini.GetLongValue("Settings", "iDLSSGGeneratedFrames", 0));
	settings.dynamicMFGEnabled = static_cast<uint>(ini.GetLongValue("Settings", "bDynamicMFGEnabled", 0));
	settings.dynamicMFGTargetFPS = static_cast<uint>(ini.GetLongValue("Settings", "iDynamicMFGTargetFPS", 300));
	settings.reflexMode = static_cast<uint>(ini.GetLongValue("Settings", "iReflexMode", 1));
	settings.dlssModelPreset = static_cast<uint>(std::clamp<long>(ini.GetLongValue("Settings", "iDLSSModelPreset", 0), 0, 4));
	settings.dlssNREnabled = static_cast<uint>(ini.GetLongValue("DLSSNR", "bEnabled", 1) == 1);
	settings.dlssNRPerformanceMode = static_cast<uint>(std::clamp<long>(ini.GetLongValue("DLSSNR", "iPerformanceMode", 0), 0, 5));
	settings.dlssNRPreset = static_cast<uint>(std::clamp<long>(ini.GetLongValue("DLSSNR", "iPreset", 0), 0, 3));
	settings.dlssNRStyle = static_cast<uint>(std::clamp<long>(ini.GetLongValue("DLSSNR", "iStyle", 0), 0, 1));
	settings.dlssNRUseAutoMask = static_cast<uint>(ini.GetLongValue("DLSSNR", "bUseAutoMask", 0) == 1);
	settings.dlssNRIntensity = std::clamp(static_cast<float>(ini.GetDoubleValue("DLSSNR", "fIntensity", 1.0)), 0.0f, 2.0f);
	settings.dlssNRLocalToneStrength = std::clamp(static_cast<float>(ini.GetDoubleValue("DLSSNR", "fLocalToneStrength", 1.0)), 0.0f, 2.0f);
	settings.dlssNRLocalStructureStrength = std::clamp(static_cast<float>(ini.GetDoubleValue("DLSSNR", "fLocalStructureStrength", 1.0)), 0.0f, 2.0f);
	settings.dlssNRGlobalToneStrength = std::clamp(static_cast<float>(ini.GetDoubleValue("DLSSNR", "fGlobalToneStrength", 1.0)), 0.0f, 2.0f);
	settings.dlssNRSkinStructureStrength = std::clamp(static_cast<float>(ini.GetDoubleValue("DLSSNR", "fSkinStructureStrength", 1.0)), -1.0f, 2.0f);
	settings.osdMode = static_cast<uint>(std::clamp<long>(ini.GetLongValue("Settings", "iOnScreenDisplay", 0), 0, 2));
	settings.taggedTextureDebug = static_cast<uint>(ini.GetLongValue("Settings", "bTaggedTextureDebug", 0) == 1);
	settings.imageSpaceEffectLog = static_cast<uint>(ini.GetLongValue("Settings", "bImageSpaceEffectLog", 0) == 1);
	if (!previousImageSpaceEffectLog && settings.imageSpaceEffectLog != 0) {
		ResetENBNativeImageSpaceEffectLog();
		logger::info("[ENB IS Log] Enabled; waiting for active effects in the ENB native scope");
	}
	const auto legacySharpness = ini.GetDoubleValue("Settings", "fRCASSharpness", 0.2);
	settings.sharpness = std::clamp(static_cast<float>(ini.GetDoubleValue("Settings", "fSharpness", legacySharpness)), 0.0f, 1.0f);

	auto streamline = Streamline::GetSingleton();
	const auto currentUpscaleMethodPreference = static_cast<UpscaleMethod>(settings.upscaleMethodPreference);
	if (previousUpscaleMethodPreference != currentUpscaleMethodPreference ||
		previousQualityMode != settings.qualityMode ||
		previousFrameGenerationMode != settings.frameGenerationMode ||
		previousDLSSGGeneratedFrames != settings.dlssgGeneratedFrames ||
		previousDynamicMFGEnabled != settings.dynamicMFGEnabled ||
		previousDLSSModelPreset != settings.dlssModelPreset ||
		previousDLSSNREnabled != settings.dlssNREnabled ||
		previousDLSSNRPerformanceMode != settings.dlssNRPerformanceMode ||
		previousDLSSNRPreset != settings.dlssNRPreset ||
		previousDLSSNRStyle != settings.dlssNRStyle ||
		previousDLSSNRUseAutoMask != settings.dlssNRUseAutoMask ||
		previousDLSSNRIntensity != settings.dlssNRIntensity ||
		previousDLSSNRLocalToneStrength != settings.dlssNRLocalToneStrength ||
		previousDLSSNRLocalStructureStrength != settings.dlssNRLocalStructureStrength ||
		previousDLSSNRGlobalToneStrength != settings.dlssNRGlobalToneStrength ||
		previousDLSSNRSkinStructureStrength != settings.dlssNRSkinStructureStrength) {
		streamline->RequestTemporalReset();
	}
	const auto switchedBetweenD3D12Upscalers =
		(previousUpscaleMethodPreference == UpscaleMethod::kFSR && currentUpscaleMethodPreference == UpscaleMethod::kDLSS) ||
		(previousUpscaleMethodPreference == UpscaleMethod::kDLSS && currentUpscaleMethodPreference == UpscaleMethod::kFSR);
	const auto switchedToD3D12Upscaler =
		previousUpscaleMethodPreference != currentUpscaleMethodPreference &&
		(currentUpscaleMethodPreference == UpscaleMethod::kFSR || currentUpscaleMethodPreference == UpscaleMethod::kDLSS);

	if (switchedToD3D12Upscaler) {
		if (!DX12SwapChain::GetSingleton()->IsReady()) {
			logger::warn("[Upscaling] {} requires the D3D12 proxy swapchain. D3D11 SR upscaling is deprecated; native rendering will remain active.",
				currentUpscaleMethodPreference == UpscaleMethod::kDLSS ? "DLSS" : "FSR");
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

bool Upscaling::SaveSettings(const Settings& a_settings)
{
	const std::filesystem::path settingsPath(kSettingsPath);
	std::error_code directoryError;
	std::filesystem::create_directories(settingsPath.parent_path(), directoryError);
	if (directoryError) {
		logger::error("[Settings] Could not create settings directory: {}", directoryError.message());
		return false;
	}

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(kSettingsPath);

	ini.SetLongValue("Settings", "iUpscaleMethodPreference", static_cast<long>(a_settings.upscaleMethodPreference));
	ini.SetLongValue("Settings", "iQualityMode", static_cast<long>(a_settings.qualityMode));
	ini.SetDoubleValue("Settings", "fSharpness", a_settings.sharpness);
	ini.SetLongValue("Settings", "iFrameGenerationMode", static_cast<long>(a_settings.frameGenerationMode));
	ini.SetLongValue("Settings", "iDLSSGGeneratedFrames", static_cast<long>(a_settings.dlssgGeneratedFrames));
	ini.SetLongValue("Settings", "bDynamicMFGEnabled", static_cast<long>(a_settings.dynamicMFGEnabled));
	ini.SetLongValue("Settings", "iDynamicMFGTargetFPS", static_cast<long>(a_settings.dynamicMFGTargetFPS));
	ini.SetLongValue("Settings", "iReflexMode", static_cast<long>(a_settings.reflexMode));
	ini.SetLongValue("Settings", "iDLSSModelPreset", static_cast<long>(a_settings.dlssModelPreset));
	ini.SetLongValue("Settings", "iOnScreenDisplay", static_cast<long>(a_settings.osdMode));
	ini.SetLongValue("Settings", "bTaggedTextureDebug", static_cast<long>(a_settings.taggedTextureDebug));
	ini.SetLongValue("Settings", "bImageSpaceEffectLog", static_cast<long>(a_settings.imageSpaceEffectLog));

	ini.SetLongValue("DLSSNR", "bEnabled", static_cast<long>(a_settings.dlssNREnabled));
	ini.SetLongValue("DLSSNR", "iPerformanceMode", static_cast<long>(a_settings.dlssNRPerformanceMode));
	ini.SetLongValue("DLSSNR", "iPreset", static_cast<long>(a_settings.dlssNRPreset));
	ini.SetLongValue("DLSSNR", "iStyle", static_cast<long>(a_settings.dlssNRStyle));
	ini.SetDoubleValue("DLSSNR", "fIntensity", a_settings.dlssNRIntensity);
	ini.SetDoubleValue("DLSSNR", "fLocalToneStrength", a_settings.dlssNRLocalToneStrength);
	ini.SetDoubleValue("DLSSNR", "fLocalStructureStrength", a_settings.dlssNRLocalStructureStrength);
	ini.SetDoubleValue("DLSSNR", "fGlobalToneStrength", a_settings.dlssNRGlobalToneStrength);
	ini.SetLongValue("DLSSNR", "bUseAutoMask", static_cast<long>(a_settings.dlssNRUseAutoMask));
	ini.SetDoubleValue("DLSSNR", "fSkinStructureStrength", a_settings.dlssNRSkinStructureStrength);

	const auto result = ini.SaveFile(kSettingsPath);
	if (result < 0) {
		logger::error("[Settings] Could not write {} (SimpleIni result {})", kSettingsPath, result);
		return false;
	}

	logger::info("[Settings] Saved settings from F4SE Menu Framework");
	return true;
}

void Upscaling::ReloadSettingsIfChanged()
{
	if (SettingsFileChangedSinceLastLoad()) {
		LoadSettings();
	}
}

void Upscaling::OnDataLoaded()
{
	RE::UI::GetSingleton()->RegisterSink<RE::MenuOpenCloseEvent>(this);
	LoadSettings();
	ApplyTextureMemoryUpgradeReserve();
	UpdateGameSettings();
}

RE::BSEventNotifyControl Upscaling::ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	auto singleton = GetSingleton();

	// Preserve the existing pause-menu close path for settings written by
	// external tools or older MCM installations.
	if (a_event.menuName == "PauseMenu") {
		if (!a_event.opening) {
			singleton->ReloadSettingsIfChanged();
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
	Streamline::GetSingleton()->RequestTemporalReset();
	ResetENBSSSProjectionSubstitution();

	// Recreate render targets with new dimensions
	for (int i = 0; i < ARRAYSIZE(renderTargetsPatch); i++)
		UpdateRenderTarget(renderTargetsPatch[i], a_currentWidthRatio, a_currentHeightRatio);

	// Keep render-size dependent local textures allocated across menu
	// suspends. They are descriptor-checked below and in
	// CreateUpscalingResources(), so only real resolution/format changes
	// recreate GPU resources.
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

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;

	// Intermediate upscaling texture (stores DLSS/FSR output)
	EnsureTexture2D(texDesc, upscalingTexture, true, true, true);

	// Do not need to replace render targets at native resolution
	if (a_currentWidthRatio == 1.0f && a_currentHeightRatio == 1.0f)
		return;

	// Dynamic resolution depth texture (R32 float)
	texDesc.Width = static_cast<uint>(static_cast<float>(texDesc.Width) * a_currentWidthRatio);
	texDesc.Height = static_cast<uint>(static_cast<float>(texDesc.Height) * a_currentHeightRatio);

	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	EnsureTexture2D(texDesc, depthOverrideTexture, true, true);
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
	const auto ratios = GetDynamicResolutionRatios();

	for (int i = 0; i < 100; i++) {
		originalRenderTargetData[i] = renderTargetManager->renderTargetData[i];
		renderTargetManager->renderTargetData[i].width = static_cast<uint>(static_cast<float>(renderTargetManager->renderTargetData[i].width) * ratios.width);
		renderTargetManager->renderTargetData[i].height = static_cast<uint>(static_cast<float>(renderTargetManager->renderTargetData[i].height) * ratios.height);
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

void Upscaling::OverrideRenderTargetsSelective(std::initializer_list<int> a_targetIndices, std::initializer_list<int> a_indicesToCopy)
{
	for (const auto targetIndex : a_targetIndices) {
		const bool shouldCopy = std::find(a_indicesToCopy.begin(), a_indicesToCopy.end(), targetIndex) != a_indicesToCopy.end();
		OverrideRenderTarget(targetIndex, shouldCopy);
	}

	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
	const auto ratios = GetDynamicResolutionRatios();
	for (const auto targetIndex : a_targetIndices) {
		originalRenderTargetData[targetIndex] = renderTargetManager->renderTargetData[targetIndex];
		renderTargetManager->renderTargetData[targetIndex].width = static_cast<uint>(static_cast<float>(renderTargetManager->renderTargetData[targetIndex].width) * ratios.width);
		renderTargetManager->renderTargetData[targetIndex].height = static_cast<uint>(static_cast<float>(renderTargetManager->renderTargetData[targetIndex].height) * ratios.height);
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	ID3D11ShaderResourceView* boundSRVs[16] = {};
	context->PSGetShaderResources(0, 16, boundSRVs);

	for (int srvSlot = 0; srvSlot < 16; srvSlot++) {
		if (!boundSRVs[srvSlot]) {
			continue;
		}

		for (const auto targetIndex : a_targetIndices) {
			auto& originalRT = originalRenderTargets[targetIndex];
			auto& proxyRT = proxyRenderTargets[targetIndex];
			if (boundSRVs[srvSlot] == reinterpret_cast<ID3D11ShaderResourceView*>(originalRT.srView) && proxyRT.srView) {
				auto proxySRV = reinterpret_cast<ID3D11ShaderResourceView*>(proxyRT.srView);
				context->PSSetShaderResources(srvSlot, 1, &proxySRV);
				break;
			}
		}

		boundSRVs[srvSlot]->Release();
	}

	ID3D11RenderTargetView* boundRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* boundDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, boundRTVs, &boundDSV);

	ID3D11RenderTargetView* reboundRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	bool rebindOM = false;
	for (int slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot) {
		reboundRTVs[slot] = boundRTVs[slot];
		if (!boundRTVs[slot]) {
			continue;
		}

		for (const auto targetIndex : a_targetIndices) {
			auto& originalRT = originalRenderTargets[targetIndex];
			auto& proxyRT = proxyRenderTargets[targetIndex];
			if (boundRTVs[slot] == reinterpret_cast<ID3D11RenderTargetView*>(originalRT.rtView) && proxyRT.rtView) {
				reboundRTVs[slot] = reinterpret_cast<ID3D11RenderTargetView*>(proxyRT.rtView);
				rebindOM = true;
				break;
			}
		}
	}

	if (rebindOM) {
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, reboundRTVs, boundDSV);
	}
	for (auto* rtv : boundRTVs) {
		if (rtv) {
			rtv->Release();
		}
	}
	if (boundDSV) {
		boundDSV->Release();
	}

	DrawWorld_Imagespace_SetUseDynamicResolutionViewportAsDefaultViewport::func(renderTargetManager, false);
}

void Upscaling::ResetRenderTargets(
	std::initializer_list<int> a_indicesToCopy,
	bool a_copyAllWhenEmpty)
{
	// Restore all original full-resolution render targets
	for (int i = 0; i < ARRAYSIZE(renderTargetsPatch); i++) {
		int targetIndex = renderTargetsPatch[i];
		// If indices array is empty, copy all. Otherwise, only copy if in the array
		bool shouldCopy = (a_copyAllWhenEmpty && a_indicesToCopy.size() == 0) ||
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

	ID3D11RenderTargetView* boundRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* boundDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, boundRTVs, &boundDSV);

	ID3D11RenderTargetView* reboundRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	bool rebindOM = false;
	for (int slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot) {
		reboundRTVs[slot] = boundRTVs[slot];
		if (!boundRTVs[slot]) {
			continue;
		}

		for (int rtIndex = 0; rtIndex < ARRAYSIZE(renderTargetsPatch); ++rtIndex) {
			const int targetIndex = renderTargetsPatch[rtIndex];
			auto& originalRT = originalRenderTargets[targetIndex];
			auto& proxyRT = proxyRenderTargets[targetIndex];
			if (boundRTVs[slot] == reinterpret_cast<ID3D11RenderTargetView*>(proxyRT.rtView) && originalRT.rtView) {
				reboundRTVs[slot] = reinterpret_cast<ID3D11RenderTargetView*>(originalRT.rtView);
				rebindOM = true;
				break;
			}
		}
	}

	if (rebindOM) {
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, reboundRTVs, boundDSV);
	}
	for (auto* rtv : boundRTVs) {
		if (rtv) {
			rtv->Release();
		}
	}
	if (boundDSV) {
		boundDSV->Release();
	}

	// Enable dynamic resolution again
	DrawWorld_Imagespace_SetUseDynamicResolutionViewportAsDefaultViewport::func(renderTargetManager, true);
}

void Upscaling::ResetRenderTargetsSelective(std::initializer_list<int> a_targetIndices, std::initializer_list<int> a_indicesToCopy)
{
	for (const auto targetIndex : a_targetIndices) {
		const bool shouldCopy =
			std::find(a_indicesToCopy.begin(), a_indicesToCopy.end(), targetIndex) != a_indicesToCopy.end();
		ResetRenderTarget(targetIndex, shouldCopy);
	}

	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
	for (const auto targetIndex : a_targetIndices) {
		renderTargetManager->renderTargetData[targetIndex] = originalRenderTargetData[targetIndex];
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	ID3D11ShaderResourceView* boundSRVs[16] = {};
	context->PSGetShaderResources(0, 16, boundSRVs);

	for (int srvSlot = 0; srvSlot < 16; srvSlot++) {
		if (!boundSRVs[srvSlot]) {
			continue;
		}

		for (const auto targetIndex : a_targetIndices) {
			auto& originalRT = originalRenderTargets[targetIndex];
			auto& proxyRT = proxyRenderTargets[targetIndex];
			if (boundSRVs[srvSlot] == reinterpret_cast<ID3D11ShaderResourceView*>(proxyRT.srView) && originalRT.srView) {
				auto originalSRV = reinterpret_cast<ID3D11ShaderResourceView*>(originalRT.srView);
				context->PSSetShaderResources(srvSlot, 1, &originalSRV);
				break;
			}
		}

		boundSRVs[srvSlot]->Release();
	}

	ID3D11RenderTargetView* boundRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* boundDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, boundRTVs, &boundDSV);

	ID3D11RenderTargetView* reboundRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	bool rebindOM = false;
	for (int slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot) {
		reboundRTVs[slot] = boundRTVs[slot];
		if (!boundRTVs[slot]) {
			continue;
		}

		for (const auto targetIndex : a_targetIndices) {
			auto& originalRT = originalRenderTargets[targetIndex];
			auto& proxyRT = proxyRenderTargets[targetIndex];
			if (boundRTVs[slot] == reinterpret_cast<ID3D11RenderTargetView*>(proxyRT.rtView) && originalRT.rtView) {
				reboundRTVs[slot] = reinterpret_cast<ID3D11RenderTargetView*>(originalRT.rtView);
				rebindOM = true;
				break;
			}
		}
	}

	if (rebindOM) {
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, reboundRTVs, boundDSV);
	}
	for (auto* rtv : boundRTVs) {
		if (rtv) {
			rtv->Release();
		}
	}
	if (boundDSV) {
		boundDSV->Release();
	}

	DrawWorld_Imagespace_SetUseDynamicResolutionViewportAsDefaultViewport::func(renderTargetManager, true);
}

void Upscaling::OverrideDepth(bool a_doCopy)
{
	static auto rendererData = RE::BSGraphics::GetRendererData();

	// Save the original depth SRV (with dynamic resolution)
	originalDepthView = reinterpret_cast<ID3D11ShaderResourceView*>(
		rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth);

	// Optionally perform expensive copy operation
	if (a_doCopy) {
		static auto gameViewport = Util::State_GetSingleton();

		// Only copy depth once per frame
		static auto previousFrame = gameViewport->frameCount;
		if (previousFrame != gameViewport->frameCount)
			CopyDepth();
		previousFrame = gameViewport->frameCount;
	}

	rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth =
		reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(depthOverrideTexture->srv.get());
}

void Upscaling::ResetDepth()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();

	rendererData->depthStencilTargets[(uint)Util::DepthStencilTarget::kMain].srViewDepth =
		reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(originalDepthView);

	if (!IsENBSRCompatibilityActive(upscaleMethod)) {
		return;
	}

	if (!originalDepthView || !depthOverrideTexture || !depthOverrideTexture->srv) {
		return;
	}

	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	if (!context) {
		return;
	}

	ID3D11ShaderResourceView* boundSRVs[16] = {};
	context->PSGetShaderResources(0, 16, boundSRVs);

	auto originalSRV = originalDepthView;
	auto* overrideSRV = depthOverrideTexture->srv.get();
	auto* nativeENBSRV = g_enbNativeDepthSRV.get();
	for (UINT srvSlot = 0; srvSlot < 16; ++srvSlot) {
		if (!boundSRVs[srvSlot]) {
			continue;
		}

		if (boundSRVs[srvSlot] == overrideSRV || boundSRVs[srvSlot] == nativeENBSRV) {
			context->PSSetShaderResources(srvSlot, 1, &originalSRV);
		}

		boundSRVs[srvSlot]->Release();
	}
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

	// Calculate both display (screen) and render (scaled) resolutions
	auto screenSize = float2(float(gameViewport->screenWidth), float(gameViewport->screenHeight));
	const auto ratios = GetDynamicResolutionRatios();
	auto renderSize = float2(screenSize.x * ratios.width, screenSize.y * ratios.height);

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
	const auto* dx12SwapChain = DX12SwapChain::GetSingleton();
	if (dx12SwapChain->IsReady() &&
		(dx12SwapChain->IsWindowUnavailable() || dx12SwapChain->AreTemporalFeaturesSuspended())) {
		return true;
	}

	if (const auto ui = RE::UI::GetSingleton()) {
		if (ui->menuMode > 0 || ui->freezeFramePause > 0) {
			return true;
		}
	}

	return false;
}

void Upscaling::OnD3D12TemporalSuspend()
{
	for (std::size_t i = 0; i < dlssgInputsReady.size(); ++i) {
		dlssgInputsReady[i] = false;
		fsrFrameGenerationInputsReady[i] = false;
		fsrD3D12InputsReady[i] = false;
		dlssD3D12InputsReady[i] = false;
		dlssD3D12Sharpened[i] = false;
		dlssD3D12TransparencyMaskReady[i] = false;
		dlssD3D12PresentFinal[i] = nullptr;
		dlssgInputFrameTokenIndices[i] = std::numeric_limits<uint32_t>::max();
		dlssgInputRenderSizes[i] = { 0.0f, 0.0f };
		dlssgInputDisplaySizes[i] = { 0.0f, 0.0f };
	}
	frameGenerationBuffersReady = false;
	frameGenerationActive = false;
	fsrFrameGenerationActive = false;
	frameGenerationInputsWanted = false;
	d3d12DLSSActive = false;
	Streamline::GetSingleton()->RequestTemporalReset();
}

bool Upscaling::ShouldBlockUpscaling() const
{
	const auto* dx12SwapChain = DX12SwapChain::GetSingleton();
	if (dx12SwapChain->IsReady() &&
		(dx12SwapChain->IsWindowUnavailable() || dx12SwapChain->AreTemporalFeaturesSuspended())) {
		return true;
	}

	// Custom-rendering menus must stay in the native game composition path. All
	// other temporal-block conditions keep SR/NR active and only suppress FG.
	if (IsCustomRenderingMenuOpen()) {
		return true;
	}

	return false;
}

bool Upscaling::ShouldBlockFrameGeneration() const
{
	return IsCustomRenderingMenuOpen() || ShouldBlockTemporalFeatures() || !dlssgMenuResumeReady;
}

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod(bool a_checkMenu)
{
	auto streamline = Streamline::GetSingleton();

	if (a_checkMenu && ShouldBlockUpscaling()) {
		return UpscaleMethod::kDisabled;
	}

	UpscaleMethod currentUpscaleMethod = (UpscaleMethod)settings.upscaleMethodPreference;
	const bool dx12Ready = DX12SwapChain::GetSingleton()->IsReady();
	if ((currentUpscaleMethod == UpscaleMethod::kDLSS || currentUpscaleMethod == UpscaleMethod::kFSR) && !dx12Ready) {
		return UpscaleMethod::kDisabled;
	}
		
	// If DLSS is not available, default to FSR
	if (!streamline->featureDLSS && currentUpscaleMethod == UpscaleMethod::kDLSS)
		currentUpscaleMethod = UpscaleMethod::kFSR;

	if ((currentUpscaleMethod == UpscaleMethod::kDLSS && IsFeatureRequestBlocked(FeatureRequest::kDLSS)) ||
		(currentUpscaleMethod == UpscaleMethod::kFSR && IsFeatureRequestBlocked(FeatureRequest::kFSR))) {
		currentUpscaleMethod = UpscaleMethod::kSpatialFallback;
	}

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

bool Upscaling::IsFeatureRequestBlocked(FeatureRequest a_feature) const
{
	const auto index = static_cast<std::size_t>(a_feature);
	if (index >= featureRetryBlocks.size()) {
		return false;
	}

	const auto& block = featureRetryBlocks[index];
	return block.active && CurrentGameFrame() < block.retryGameFrame;
}

void Upscaling::ClearFeatureRequestFailure(FeatureRequest a_feature)
{
	const auto index = static_cast<std::size_t>(a_feature);
	if (index >= featureRetryBlocks.size()) {
		return;
	}

	featureRetryBlocks[index] = {};
}

void Upscaling::ClearFrameFeatureRequests()
{
	frameGenerationInputsWanted = false;
	frameGenerationActive = false;
	fsrFrameGenerationActive = false;
	d3d12DLSSActive = false;
	const bool frameGenerationSettingEnabled = settings.frameGenerationMode != 0 || settings.dynamicMFGEnabled != 0;
	dlssgMenuResumeReady = !frameGenerationSettingEnabled;
	dlssgStableGameplayFrames = frameGenerationSettingEnabled ? 0 : kDLSSGResumeStableFrames;

	auto streamline = Streamline::GetSingleton();
	if (streamline->dlssgActive || streamline->HasPendingDLSSGDisable()) {
		streamline->RequestDLSSGDisable();
	}

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
	}
}

void Upscaling::ReportFeatureRequestFailure(FeatureRequest a_feature, std::string_view a_context)
{
	const auto index = static_cast<std::size_t>(a_feature);
	if (index >= featureRetryBlocks.size()) {
		return;
	}

	const auto currentGameFrame = CurrentGameFrame();
	auto& block = featureRetryBlocks[index];
	if (block.lastFailureGameFrame != currentGameFrame) {
		if (block.consecutiveFailures < std::numeric_limits<uint32_t>::max()) {
			++block.consecutiveFailures;
		}
		block.lastFailureGameFrame = currentGameFrame;
	}
	if (block.consecutiveFailures >= kFeatureFailuresBeforeRetryBlock) {
		block.active = true;
		block.retryGameFrame = currentGameFrame + kFeatureRetryGameFrames;
	}

	ClearFrameFeatureRequests();

	const auto cameraProjection = Util::GetCameraProjection();
	const auto* cameraState = cameraProjection.cameraState;
	const auto* ui = RE::UI::GetSingleton();
	const auto menuMode = ui ? ui->menuMode : 0;
	const auto itemMenuMode = ui ? ui->itemMenuMode.load_unchecked() : 0;
	const auto freezeFrameMenuBG = ui ? ui->freezeFrameMenuBG : 0;
	const auto freezeFramePause = ui ? ui->freezeFramePause : 0;

	logger::warn(
		"[Upscaling] {} request failed in {}; failures={}/{} blocked={} retryFrame={} currentFrame={} "
		"ui(menu={} item={} freezeBG={} freezePause={}) "
		"worldCamera(state={} reference={} jitter={} matrixFov={} fov={})",
		FeatureRequestName(a_feature),
		a_context,
		block.consecutiveFailures,
		kFeatureFailuresBeforeRetryBlock,
		block.active,
		block.retryGameFrame,
		currentGameFrame,
		menuMode,
		itemMenuMode,
		freezeFrameMenuBG,
		freezeFramePause,
		static_cast<const void*>(cameraState),
		cameraState ? static_cast<const void*>(cameraState->referenceCamera) : nullptr,
		cameraState ? cameraState->useJitter : false,
		cameraProjection.usedMatrixFOV,
		cameraProjection.cameraFOV);
}

void Upscaling::CheckResources()
{
	static auto previousResourceUpscaleMethodNoMenu = UpscaleMethod::kDisabled;

	auto streamline = Streamline::GetSingleton();
	auto fidelityFX = FidelityFX::GetSingleton();
	auto resourceUpscaleMethodNoMenu = static_cast<UpscaleMethod>(settings.upscaleMethodPreference);
	if (resourceUpscaleMethodNoMenu == UpscaleMethod::kSpatialFallback) {
		resourceUpscaleMethodNoMenu = streamline->featureDLSS ? UpscaleMethod::kDLSS : UpscaleMethod::kFSR;
	}
	if (!streamline->featureDLSS && resourceUpscaleMethodNoMenu == UpscaleMethod::kDLSS) {
		resourceUpscaleMethodNoMenu = UpscaleMethod::kFSR;
	}
	const bool dx12Ready = DX12SwapChain::GetSingleton()->IsReady();
	if ((resourceUpscaleMethodNoMenu == UpscaleMethod::kDLSS || resourceUpscaleMethodNoMenu == UpscaleMethod::kFSR) && !dx12Ready) {
		resourceUpscaleMethodNoMenu = UpscaleMethod::kDisabled;
	}

	// Detect configured upscaler changes and manage resources accordingly.
	// kSpatialFallback is a temporary retry-blocked runtime mode and must not
	// tear down the SDK resources it is standing in for.
	if (previousResourceUpscaleMethodNoMenu != resourceUpscaleMethodNoMenu) {
		if (dx12Ready) {
			// SDK feature destruction and shared-resource release are only safe
			// after all queued evaluations and intercepted Presents have drained.
			DX12SwapChain::GetSingleton()->WaitForGPUIdle();
		}
		for (std::size_t i = 0; i < dlssD3D12InputsReady.size(); ++i) {
			dlssgInputsReady[i] = false;
			fsrFrameGenerationInputsReady[i] = false;
			fsrD3D12InputsReady[i] = false;
			dlssD3D12InputsReady[i] = false;
		}

		// Clean up resources from the previous upscaling method
		if (previousResourceUpscaleMethodNoMenu == UpscaleMethod::kDisabled)
			CreateUpscalingResources();  // Transitioning from disabled to enabled
		else if (previousResourceUpscaleMethodNoMenu == UpscaleMethod::kFSR)
			fidelityFX->DestroyFSRResources();  // Switching away from FSR
		else if (previousResourceUpscaleMethodNoMenu == UpscaleMethod::kDLSS)
			streamline->DestroyDLSSResources();  // Switching away from DLSS

		// Create resources for the new upscaling method
		if (resourceUpscaleMethodNoMenu == UpscaleMethod::kDisabled)
			DestroyUpscalingResources();  // Transitioning to disabled
		else if (resourceUpscaleMethodNoMenu == UpscaleMethod::kFSR)
			fidelityFX->CreateFSRResources();  // Switching to FSR
		else if (resourceUpscaleMethodNoMenu == UpscaleMethod::kDLSS)
			CreateUpscalingResources();  // Switching to local upscaling resources

		previousResourceUpscaleMethodNoMenu = resourceUpscaleMethodNoMenu;
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

ID3D11ComputeShader* Upscaling::GetGenerateDLSSTransparencyMaskCS()
{
	if (!generateDLSSTransparencyMaskCS) {
		logger::debug("Compiling GenerateDLSSTransparencyMaskCS.hlsl");
		generateDLSSTransparencyMaskCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/F4SE/Plugins/Upscaling/GenerateDLSSTransparencyMaskCS.hlsl", {}, "cs_5_0"));
	}
	return generateDLSSTransparencyMaskCS.get();
}

ID3D11ComputeShader* Upscaling::GetSpatialFallbackUpscaleCS()
{
	if (!spatialFallbackUpscaleCS) {
		static constexpr const char* shaderSource = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer UpscalingCB : register(b0)
{
	uint2 ScreenSize;
	uint2 RenderSize;
	float4 CameraData;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x >= ScreenSize.x || dispatchThreadID.y >= ScreenSize.y) {
		return;
	}

	const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(ScreenSize);
	const float2 sourcePos = uv * float2(RenderSize) - 0.5f;
	const int2 sourceBase = int2(floor(sourcePos));
	const float2 fraction = frac(sourcePos);
	const int2 maxCoord = int2(RenderSize) - 1;
	const int2 p00 = clamp(sourceBase, int2(0, 0), maxCoord);
	const int2 p10 = clamp(sourceBase + int2(1, 0), int2(0, 0), maxCoord);
	const int2 p01 = clamp(sourceBase + int2(0, 1), int2(0, 0), maxCoord);
	const int2 p11 = clamp(sourceBase + int2(1, 1), int2(0, 0), maxCoord);

	const float4 c00 = InputColor.Load(int3(p00, 0));
	const float4 c10 = InputColor.Load(int3(p10, 0));
	const float4 c01 = InputColor.Load(int3(p01, 0));
	const float4 c11 = InputColor.Load(int3(p11, 0));
	OutputColor[dispatchThreadID.xy] = lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}
)";

		winrt::com_ptr<ID3DBlob> shaderBlob;
		shaderBlob.attach(CompileInlineShader(shaderSource, "main", "cs_5_0"));
		if (!shaderBlob) {
			return nullptr;
		}

		static auto rendererData = RE::BSGraphics::GetRendererData();
		auto device = reinterpret_cast<ID3D11Device*>(rendererData->device);
		DX::ThrowIfFailed(device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, spatialFallbackUpscaleCS.put()));
	}

	return spatialFallbackUpscaleCS.get();
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
	const auto temporalFeaturesWereBlocked = temporalFeaturesBlocked;
	temporalFeaturesBlocked = ShouldBlockTemporalFeatures();
	if (temporalFeaturesWereBlocked != temporalFeaturesBlocked) {
		streamline->RequestTemporalReset();
	}
	const bool dx12Ready = DX12SwapChain::GetSingleton()->IsReady();

	upscaleMethodNoMenu = static_cast<UpscaleMethod>(settings.upscaleMethodPreference);
	if ((upscaleMethodNoMenu == UpscaleMethod::kDLSS || upscaleMethodNoMenu == UpscaleMethod::kFSR) && !dx12Ready) {
		upscaleMethodNoMenu = UpscaleMethod::kDisabled;
	}
	if (!streamline->featureDLSS && upscaleMethodNoMenu == UpscaleMethod::kDLSS) {
		upscaleMethodNoMenu = UpscaleMethod::kFSR;
	}
	if ((upscaleMethodNoMenu == UpscaleMethod::kDLSS && IsFeatureRequestBlocked(FeatureRequest::kDLSS)) ||
		(upscaleMethodNoMenu == UpscaleMethod::kFSR && IsFeatureRequestBlocked(FeatureRequest::kFSR))) {
		upscaleMethodNoMenu = UpscaleMethod::kSpatialFallback;
	}

	const bool frameGenerationSettingEnabled = settings.frameGenerationMode != 0 || settings.dynamicMFGEnabled != 0;
	const bool upscalerSelected = upscaleMethodNoMenu != UpscaleMethod::kDisabled;
	const bool menuBlocksTemporal = temporalFeaturesBlocked;
	const bool customRenderingMenu = IsCustomRenderingMenuOpen();
	const bool upscalingBlocked = ShouldBlockUpscaling();
	const bool windowUnavailable = dx12Ready && DX12SwapChain::GetSingleton()->IsWindowUnavailable();

	// Ordinary pause/overlay menus keep SR/NR active. Custom-rendering menus use
	// the native game path with SR, NR, and FG all disabled.
	upscaleMethod = upscalingBlocked ? UpscaleMethod::kDisabled : upscaleMethodNoMenu;

	const bool menuBlocksUpscaling = upscalerSelected && upscalingBlocked;
	const bool dlssgHeldThroughMenu =
		menuBlocksUpscaling &&
		!customRenderingMenu &&
		!windowUnavailable &&
		frameGenerationSettingEnabled &&
		streamline->featureDLSSG;
	const bool menuSuspendsD3D12DLSS =
		menuBlocksUpscaling &&
		!dlssgHeldThroughMenu &&
		upscaleMethodNoMenu == UpscaleMethod::kDLSS &&
		streamline->UsesD3D12() &&
		streamline->featureDLSS;
	const bool resumeD3D12DLSSFromMenu = d3d12DLSSMenuSuspended && !menuBlocksUpscaling;

	if (!frameGenerationSettingEnabled) {
		dlssgMenuResumeReady = true;
		dlssgStableGameplayFrames = kDLSSGResumeStableFrames;
	} else if (menuBlocksUpscaling && !dlssgHeldThroughMenu) {
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

	const bool dlssgAllowed = frameGenerationSettingEnabled &&
		streamline->featureDLSSG &&
		!customRenderingMenu &&
		!IsFeatureRequestBlocked(FeatureRequest::kDLSSG) &&
		(!menuBlocksTemporal || dlssgHeldThroughMenu) &&
		(dlssgHeldThroughMenu || dlssgMenuResumeReady);
	fsrFrameGenerationActive =
		static_cast<UpscaleMethod>(settings.upscaleMethodPreference) != UpscaleMethod::kDisabled &&
		frameGenerationSettingEnabled &&
		dx12Ready &&
		(kForceFSRFrameGenerationForTesting || !streamline->featureDLSSG) &&
		!IsFeatureRequestBlocked(FeatureRequest::kFSRFrameGeneration) &&
		!customRenderingMenu &&
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

	// SR/NR remain in their render-resolution domain through ordinary pause /
	// overlay menus. Custom-rendering menus use the native game path and skip
	// SR, NR, and FG entirely.
	const auto effectiveQualityMode = GetEffectiveQualityMode(upscaleMethod, settings.qualityMode);
	float resolutionScale = upscaleMethod == UpscaleMethod::kDisabled ? 1.0f : 1.0f / GetUpscaleRatioFromQualityMode(effectiveQualityMode);

	// Calculate mipmap LOD bias
	// Example: 0.67 scale -> log2(0.67) = -0.58
	float currentMipBias = std::log2f(resolutionScale);

	if (upscaleMethodNoMenu == UpscaleMethod::kDLSS ||
		upscaleMethodNoMenu == UpscaleMethod::kFSR ||
		upscaleMethodNoMenu == UpscaleMethod::kSpatialFallback) {
		currentMipBias -= 1.0f;
	}

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
	const auto upscaleInputName = std::format("upscale_target_P{}", a_renderTargetIndex);

	static auto gameViewport = Util::State_GetSingleton();

	D3D11_TEXTURE2D_DESC upscaleDesc{};
	upscalingTexture->resource->GetDesc(&upscaleDesc);

	auto displaySize = float2(float(upscaleDesc.Width), float(upscaleDesc.Height));
	auto renderSize = float2(displaySize.x * originalDynamicWidthRatio, displaySize.y * originalDynamicHeightRatio);
	osdRenderSize = renderSize;
	osdNativeSize = displaySize;

	bool preparedENBInput = false;
	const bool requiresENBInputConversion =
		IsENBSRCompatibilityActive(upscaleMethod) &&
		(originalDynamicWidthRatio != 1.0f || originalDynamicHeightRatio != 1.0f);
	if (requiresENBInputConversion) {
		auto* device = reinterpret_cast<ID3D11Device*>(rendererData->device);
		preparedENBInput = PrepareENBSuperResolutionInput(
			device,
			context,
			frameBufferSRV,
			upscalingTexture.get(),
			std::max(1u, static_cast<uint32_t>(renderSize.x)),
			std::max(1u, static_cast<uint32_t>(renderSize.y)));
	}

	if (!preparedENBInput) {
		// Non-ENB and native-AA paths already satisfy the ordinary render-rect
		// contract. ENB+DRS reaches here only if the guarded conversion failed.
		context->CopyResource(upscalingTexture->resource.get(), frameBufferResource);
	}

	for (auto* rtv : currentRTVs) {
		if (rtv) {
			rtv->Release();
		}
	}
	if (currentDSV) {
		currentDSV->Release();
	}

	// Dilate motion vectors and strip projection jitter before temporal upscalers or DLSS-G consume them.
	if (upscaleMethod == UpscaleMethod::kDLSS || upscaleMethod == UpscaleMethod::kFSR || frameGenerationActive || fsrFrameGenerationActive) {
		if ((upscaleMethod == UpscaleMethod::kDLSS && !dlssOutputTexture) || !dilatedMotionVectorTexture) {
			try {
				CreateUpscalingResources();
			} catch (const std::exception& e) {
				if (upscaleMethod == UpscaleMethod::kDLSS) {
					ReportFeatureRequestFailure(FeatureRequest::kDLSS, e.what());
				} else if (upscaleMethod == UpscaleMethod::kFSR) {
					ReportFeatureRequestFailure(FeatureRequest::kFSR, e.what());
				} else if (frameGenerationActive) {
					ReportFeatureRequestFailure(FeatureRequest::kDLSSG, e.what());
				} else if (fsrFrameGenerationActive) {
					ReportFeatureRequestFailure(FeatureRequest::kFSRFrameGeneration, e.what());
				}
				frameBufferResource->Release();
				return;
			}
		}

		if ((upscaleMethod == UpscaleMethod::kDLSS && !dlssOutputTexture) || !dilatedMotionVectorTexture) {
			logger::warn("[Upscaling] motion vector dilation resources unavailable, skipping upscale");
			if (upscaleMethod == UpscaleMethod::kDLSS) {
				ReportFeatureRequestFailure(FeatureRequest::kDLSS, "upscaling resources");
			} else if (upscaleMethod == UpscaleMethod::kFSR) {
				ReportFeatureRequestFailure(FeatureRequest::kFSR, "upscaling resources");
			} else if (frameGenerationActive) {
				ReportFeatureRequestFailure(FeatureRequest::kDLSSG, "upscaling resources");
			} else if (fsrFrameGenerationActive) {
				ReportFeatureRequestFailure(FeatureRequest::kFSRFrameGeneration, "upscaling resources");
			}
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
		if (frameIndex < dlssSharpenedD3D12.size() &&
			dlssD3D12Sharpened[frameIndex] &&
			dlssSharpenedD3D12[frameIndex]) {
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
			return true;
		}
		return false;
	};
	auto copyD3D12DLSSOutputToD3D11 = [&]() {
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		if (frameIndex < dlssSharpenedSharedTextures.size() && dlssD3D12Sharpened[frameIndex] && dlssSharpenedSharedTextures[frameIndex]) {
			context->CopyResource(frameBufferResource, dlssSharpenedSharedTextures[frameIndex]->resource.get());
			return true;
		} else if (frameIndex < dlssgHUDLessSharedTextures.size() && dlssgHUDLessSharedTextures[frameIndex]) {
			context->CopyResource(frameBufferResource, dlssgHUDLessSharedTextures[frameIndex]->resource.get());
			return true;
		}
		return false;
	};
	auto runSpatialFallbackUpscale = [&]() {
		if (!upscalingTexture || !upscalingTexture->srv) {
			return false;
		}

		D3D11_TEXTURE2D_DESC fallbackDesc{};
		upscalingTexture->resource->GetDesc(&fallbackDesc);
		fallbackDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		fallbackDesc.MiscFlags = 0;
		EnsureTexture2D(fallbackDesc, spatialFallbackTexture, false, true);

		auto shader = GetSpatialFallbackUpscaleCS();
		if (!shader || !spatialFallbackTexture || !spatialFallbackTexture->uav) {
			return false;
		}

		UpdateAndBindUpscalingCB(context, displaySize, renderSize);

		ID3D11ShaderResourceView* views[] = { upscalingTexture->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[] = { spatialFallbackTexture->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch(
			static_cast<UINT>(std::ceil(displaySize.x / 8.0f)),
			static_cast<UINT>(std::ceil(displaySize.y / 8.0f)),
			1);

		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		ID3D11ShaderResourceView* nullViews[] = { nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
		ID3D11UnorderedAccessView* nullUavs[] = { nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
		ID3D11ComputeShader* nullShader = nullptr;
		context->CSSetShader(nullShader, nullptr, 0);

		context->CopyResource(frameBufferResource, spatialFallbackTexture->resource.get());
		return true;
	};
	bool spatialFallbackApplied = false;
	auto runSpatialFallbackNow = [&](std::string_view a_context) {
		if (spatialFallbackApplied) {
			return true;
		}
		if (!runSpatialFallbackUpscale()) {
			logger::warn("[Upscaling] Spatial fallback upscale failed after {}; preserving scaled render target state without SDK evaluation", a_context);
			return false;
		}
		spatialFallbackApplied = true;
		return true;
	};
	if (upscaleMethod == UpscaleMethod::kDLSS && !useD3D12DLSS) {
		static bool loggedDeprecatedD3D11DLSS = false;
		if (!loggedDeprecatedD3D11DLSS) {
			logger::warn("[Upscaling] D3D11 DLSS SR path is deprecated; using local spatial fallback for this frame");
			loggedDeprecatedD3D11DLSS = true;
		}
		runSpatialFallbackNow("deprecated D3D11 DLSS path");
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
		bool d3d12FSRFailureReported = false;
		try {
			d3d12FSRInputsReady = CaptureD3D12FSRInputs(a_renderTargetIndex, motionVectorTexture, fsrJitter, renderSize, displaySize);
		} catch (const std::exception& e) {
			ReportFeatureRequestFailure(FeatureRequest::kFSR, e.what());
			d3d12FSRFailureReported = true;
			d3d12FSRInputsReady = false;
		}
		if (!d3d12FSRInputsReady) {
			if (!d3d12FSRFailureReported) {
				ReportFeatureRequestFailure(FeatureRequest::kFSR, "D3D12 FSR inputs");
			}
			runSpatialFallbackNow("D3D12 FSR input failure");
		} else if (!fsrFrameGenerationActive) {
			const auto usePresentOverride = getD3D12FSROutput() != nullptr && !scopeMenuOpen;
			const auto d3d12Result = dx12SwapChain->EvaluateD3D12WorkForCurrentFrame(false, true, false, !usePresentOverride);
			if (d3d12Result.fsr) {
				if (usePresentOverride && prepareD3D12PresentOverrideUI() && setD3D12PresentOverride(getD3D12FSROutput())) {
					// The upscaled scene stays on D3D12; late D3D11 rendering is UI-only.
				} else if (!copyD3D12FSROutputToD3D11()) {
					runSpatialFallbackNow("D3D12 FSR copyback failure");
				}
			} else {
				runSpatialFallbackNow("D3D12 FSR evaluate failure");
			}
		}
	}
	else if (upscaleMethod == UpscaleMethod::kFSR) {
		static bool loggedDeprecatedD3D11FSR = false;
		if (!loggedDeprecatedD3D11FSR) {
			logger::warn("[Upscaling] D3D11 FSR path is unavailable; using local spatial fallback for this frame");
			loggedDeprecatedD3D11FSR = true;
		}
		runSpatialFallbackNow("unavailable D3D11 FSR path");
	}
	else if (upscaleMethod == UpscaleMethod::kSpatialFallback) {
		if (!runSpatialFallbackUpscale()) {
			logger::warn("[Upscaling] Spatial fallback upscale failed; preserving scaled render target state without SDK evaluation");
		}
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
			if (!dx12SwapChain->WaitForFrameSlot(debugFrameIndex)) {
				return;
			}
			D3D11_TEXTURE2D_DESC debugMotionVectorDesc{};
			motionVectorTexture->GetDesc(&debugMotionVectorDesc);
			debugMotionVectorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			debugMotionVectorDesc.MiscFlags = 0;
			EnsureSharedD3D12Texture(debugMotionVectorDesc, debugMotionVectorSharedTextures[debugFrameIndex], debugMotionVectorD3D12[debugFrameIndex], false);
			context->CopyResource(debugMotionVectorSharedTextures[debugFrameIndex]->resource.get(), motionVectorTexture);
		}

		try {
			CaptureDLSSGInputs(a_renderTargetIndex, motionVectorTexture, renderSize, displaySize);
		} catch (const std::exception& e) {
			if (useD3D12DLSS) {
				ReportFeatureRequestFailure(FeatureRequest::kDLSS, e.what());
			} else if (frameGenerationActive) {
				ReportFeatureRequestFailure(FeatureRequest::kDLSSG, e.what());
			} else if (fsrFrameGenerationActive) {
				ReportFeatureRequestFailure(FeatureRequest::kFSRFrameGeneration, e.what());
			}
		}
		const bool requestedD3D12FSR = d3d12FSRInputsReady && fsrFrameGenerationActive;
		const bool requestedD3D12DLSS = useD3D12DLSS;
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		const bool dlssPresentOverrideReady =
			requestedD3D12DLSS &&
			frameIndex < dlssD3D12InputsReady.size() &&
			dlssD3D12InputsReady[frameIndex] &&
			getD3D12DLSSOutput();
		const bool hasPresentOverrideOutput =
			dlssPresentOverrideReady ||
			(requestedD3D12FSR && getD3D12FSROutput());
		const auto usePresentOverride = hasPresentOverrideOutput && !scopeMenuOpen;
		const auto d3d12Result = dx12SwapChain->EvaluateD3D12WorkForCurrentFrame(
			requestedD3D12DLSS,
			requestedD3D12FSR,
			fsrFrameGenerationActive && fsrFrameGenerationSwapChainReady,
			!usePresentOverride);
		if (d3d12Result.fsr) {
			if (usePresentOverride && prepareD3D12PresentOverrideUI() && setD3D12PresentOverride(getD3D12FSROutput())) {
				// The upscaled scene stays on D3D12; late D3D11 rendering is UI-only.
			} else if (!copyD3D12FSROutputToD3D11()) {
				runSpatialFallbackNow("D3D12 FSR copyback failure");
			}
		} else if (requestedD3D12FSR) {
			runSpatialFallbackNow("D3D12 FSR evaluate failure");
		}
		if (d3d12Result.dlss) {
			auto* dlssOutput = getD3D12DLSSOutput();
			setD3D12DLSSPresentFinal(dlssOutput);
			if (usePresentOverride && prepareD3D12PresentOverrideUI() && setD3D12PresentOverride(dlssOutput)) {
				// The upscaled scene stays on D3D12; late D3D11 rendering is UI-only.
			} else if (!copyD3D12DLSSOutputToD3D11()) {
				runSpatialFallbackNow("D3D12 DLSS copyback failure");
			}
		} else if (requestedD3D12DLSS) {
			runSpatialFallbackNow("D3D12 DLSS evaluate failure");
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
	auto* dx12SwapChain = DX12SwapChain::GetSingleton();
	const auto frameIndex = dx12SwapChain->GetFrameIndex();
	if (frameIndex >= fsrD3D12InputsReady.size()) {
		return false;
	}
	if (!dx12SwapChain->WaitForFrameSlot(frameIndex)) {
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
	inputDesc.Width = std::min<UINT>(
		inputDesc.Width,
		std::max(1u, static_cast<UINT>(a_renderSize.x)));
	inputDesc.Height = std::min<UINT>(
		inputDesc.Height,
		std::max(1u, static_cast<UINT>(a_renderSize.y)));
	inputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	inputDesc.MiscFlags = 0;
	EnsureSharedD3D12Texture(inputDesc, fsrInputSharedTextures[frameIndex], fsrInputD3D12[frameIndex], false);
	const D3D11_BOX colorSourceBox{ 0, 0, 0, inputDesc.Width, inputDesc.Height, 1 };
	context->CopySubresourceRegion(
		fsrInputSharedTextures[frameIndex]->resource.get(),
		0,
		0,
		0,
		0,
		upscalingTexture->resource.get(),
		0,
		&colorSourceBox);

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
		if (frameIndex >= dlssgInputsReady.size()) {
			frameBufferResource->Release();
			return;
		}
		if (!dx12SwapChain->WaitForFrameSlot(frameIndex)) {
			frameBufferResource->Release();
			return;
		}
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
			dlssInputDesc.Width = std::min<UINT>(
				dlssInputDesc.Width,
				std::max(1u, static_cast<UINT>(a_renderSize.x)));
			dlssInputDesc.Height = std::min<UINT>(
				dlssInputDesc.Height,
				std::max(1u, static_cast<UINT>(a_renderSize.y)));
			dlssInputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			dlssInputDesc.MiscFlags = 0;
			EnsureSharedD3D12Texture(dlssInputDesc, dlssInputSharedTextures[frameIndex], dlssInputD3D12[frameIndex], false);
			const D3D11_BOX colorSourceBox{
				0, 0, 0,
				dlssInputDesc.Width,
				dlssInputDesc.Height,
				1
			};
			context->CopySubresourceRegion(
				dlssInputSharedTextures[frameIndex]->resource.get(),
				0,
				0,
				0,
				0,
				upscalingTexture->resource.get(),
				0,
				&colorSourceBox);

			if (settings.sharpness > 0.0f || settings.dlssNREnabled != 0) {
				auto sharpenedDesc = frameBufferDesc;
				sharpenedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				sharpenedDesc.MiscFlags = 0;
				EnsureSharedD3D12Texture(sharpenedDesc, dlssSharpenedSharedTextures[frameIndex], dlssSharpenedD3D12[frameIndex], true);
			}

			dlssTransparencyMaskReady = false;
			dlssD3D12TransparencyMaskReady[frameIndex] = false;
			auto* opaqueOnly = FidelityFX::GetSingleton()->colorOpaqueOnlyTexture.get();
			if (opaqueOnly && opaqueOnly->srv && upscalingTexture->srv) {
				D3D11_TEXTURE2D_DESC maskDesc{};
				maskDesc.Width = static_cast<UINT>(a_renderSize.x);
				maskDesc.Height = static_cast<UINT>(a_renderSize.y);
				maskDesc.MipLevels = 1;
				maskDesc.ArraySize = 1;
				maskDesc.Format = DXGI_FORMAT_R32_FLOAT;
				maskDesc.SampleDesc.Count = 1;
				maskDesc.Usage = D3D11_USAGE_DEFAULT;
				maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				EnsureTexture2D(maskDesc, dlssTransparencyMaskTexture, true, true);

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

		// The pre/post-alpha hook surrounds the first-person accumulator's
		// RenderAlphaGeometry call. Its difference is 3D scene content in the
		// render-resolution domain, not display-space UI. Keep using it to repair
		// motion/depth, but never remove it from HUD-less color or tag it for UI
		// recomposition: doing so produces a reduced duplicate at the upper-left.
		if (reuseFSRResourcesForFrameGeneration && !useD3D12DLSS) {
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
			// DLSS writes the complete display-sized output before this resource is
			// consumed by present or frame generation. Initializing it from D3D11 is
			// a redundant full-resolution copy on the SR path.
			if (!useD3D12DLSS) {
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

		bool useDLSSGThisFrame = useFrameGeneration;
		if (useDLSSGThisFrame) {
			streamline->UpdateReflex(settings.reflexMode, true);
			if (!streamline->UpdateConstants(jitter)) {
				ReportFeatureRequestFailure(FeatureRequest::kDLSSG, "Streamline common constants");
				useDLSSGThisFrame = false;
			}
		}
		if (useD3D12DLSS && !useDLSSGThisFrame) {
			streamline->UpdateReflex(settings.reflexMode, false);
			if (!streamline->UpdateConstants(jitter)) {
				ReportFeatureRequestFailure(FeatureRequest::kDLSS, "Streamline SR common constants");
				frameBufferResource->Release();
				return;
			}
		} else if (!useD3D12DLSS && useFrameGeneration && !useDLSSGThisFrame) {
			frameBufferResource->Release();
			return;
		}
		const auto dlssgInputSize = float2(static_cast<float>(sharedMotionVectorDesc.Width), static_cast<float>(sharedMotionVectorDesc.Height));
		if (useDLSSGThisFrame) {
			if (!streamline->UpdateDLSSG(true, settings.frameGenerationMode, settings.dlssgGeneratedFrames + 1, settings.dynamicMFGEnabled != 0, settings.dynamicMFGTargetFPS, dlssgInputSize, a_displaySize, frameBufferDesc.Format, sharedMotionVectorDesc.Format, sharedDepthDesc.Format, DXGI_FORMAT_UNKNOWN)) {
				ReportFeatureRequestFailure(FeatureRequest::kDLSSG, "DLSS-G options");
				useDLSSGThisFrame = false;
				if (!useD3D12DLSS) {
					frameBufferResource->Release();
					return;
				}
			}
		}

		static uint64_t fsrFrameGenerationFrameID = 0;
		dlssgInputRenderSizes[frameIndex] = dlssgInputSize;
		dlssgInputDisplaySizes[frameIndex] = a_displaySize;
		dlssgInputFrameTokenIndices[frameIndex] = streamline->GetCurrentFrameTokenIndex();
		dlssgInputsReady[frameIndex] = useDLSSGThisFrame;
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

	auto hudlessDesc = frameBufferDesc;
	hudlessDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	hudlessDesc.CPUAccessFlags = 0;
	hudlessDesc.MiscFlags = 0;
	EnsureTexture2D(hudlessDesc, dlssgHUDLessTexture, false, false);

	context->CopyResource(dlssgHUDLessTexture->resource.get(), frameBufferResource);

	streamline->UpdateReflex(settings.reflexMode, true);
	if (!streamline->UpdateConstants(jitter)) {
		ReportFeatureRequestFailure(FeatureRequest::kDLSSG, "Streamline common constants");
		frameBufferResource->Release();
		return;
	}
	if (!streamline->UpdateDLSSG(true, settings.frameGenerationMode, settings.dlssgGeneratedFrames + 1, settings.dynamicMFGEnabled != 0, settings.dynamicMFGTargetFPS, a_renderSize, a_displaySize, frameBufferDesc.Format, motionVectorDesc.Format, depthDesc.Format)) {
		ReportFeatureRequestFailure(FeatureRequest::kDLSSG, "DLSS-G options");
		frameBufferResource->Release();
		return;
	}
	streamline->TagDLSSGResources(dlssgHUDLessTexture->resource.get(), motionVectorTexture, depthTexture, a_renderSize, a_displaySize);

	frameBufferResource->Release();
}

bool Upscaling::EvaluateD3D12DLSS(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex)
{
	if (a_frameIndex >= dlssD3D12InputsReady.size() || !dlssD3D12InputsReady[a_frameIndex]) {
		logger::warn(
			"[Upscaling] D3D12 DLSS evaluate skipped frameIndex={} ready={} commandList={}",
			a_frameIndex,
			a_frameIndex < dlssD3D12InputsReady.size() ? dlssD3D12InputsReady[a_frameIndex] : false,
			static_cast<void*>(a_commandList));
		return false;
	}

	auto* dlssInput = dlssInputD3D12[a_frameIndex].get();
	auto* dlssOutput = dlssgHUDLessD3D12[a_frameIndex].get();
	auto* dlssSharpenedOutput = dlssSharpenedD3D12[a_frameIndex].get();
	auto* motionVectors = dlssgMotionVectorD3D12[a_frameIndex].get();
	auto* depth = dlssgDepthD3D12[a_frameIndex].get();
	auto* transparencyMask = dlssD3D12TransparencyMaskReady[a_frameIndex] ? dlssTransparencyMaskD3D12[a_frameIndex].get() : nullptr;
	if (!dlssInput || !dlssOutput || !motionVectors || !depth || !a_commandList) {
		logger::warn(
			"[Upscaling] D3D12 DLSS inputs missing frameIndex={} input={} output={} mvec={} depth={} commandList={} transparencyReady={} transparency={} tokenIndex={} render={}x{} display={}x{} formats color={} mvec={} depth={}",
			a_frameIndex,
			static_cast<void*>(dlssInput),
			static_cast<void*>(dlssOutput),
			static_cast<void*>(motionVectors),
			static_cast<void*>(depth),
			static_cast<void*>(a_commandList),
			dlssD3D12TransparencyMaskReady[a_frameIndex],
			static_cast<void*>(transparencyMask),
			dlssgInputFrameTokenIndices[a_frameIndex],
			dlssgInputRenderSizes[a_frameIndex].x,
			dlssgInputRenderSizes[a_frameIndex].y,
			dlssgInputDisplaySizes[a_frameIndex].x,
			dlssgInputDisplaySizes[a_frameIndex].y,
			magic_enum::enum_name(dlssD3D12ColorFormats[a_frameIndex]),
			magic_enum::enum_name(dlssD3D12MotionVectorFormats[a_frameIndex]),
			magic_enum::enum_name(dlssD3D12DepthFormats[a_frameIndex]));
		ReportFeatureRequestFailure(FeatureRequest::kDLSS, "D3D12 DLSS inputs");
		return false;
	}

	auto streamline = Streamline::GetSingleton();
	const auto frameIndex = dlssgInputFrameTokenIndices[a_frameIndex];
	auto* frameToken = streamline->GetFrameTokenForFrame(frameIndex);
	if (!frameToken) {
		logger::warn(
			"[Upscaling] D3D12 DLSS frame token missing frameIndex={} requestedTokenIndex={} input={} output={} mvec={} depth={} render={}x{} display={}x{}",
			a_frameIndex,
			frameIndex,
			static_cast<void*>(dlssInput),
			static_cast<void*>(dlssOutput),
			static_cast<void*>(motionVectors),
			static_cast<void*>(depth),
			dlssgInputRenderSizes[a_frameIndex].x,
			dlssgInputRenderSizes[a_frameIndex].y,
			dlssgInputDisplaySizes[a_frameIndex].x,
			dlssgInputDisplaySizes[a_frameIndex].y);
		ReportFeatureRequestFailure(FeatureRequest::kDLSS, "D3D12 DLSS frame token");
		return false;
	}

	const bool useSharpenedOutput = settings.sharpness > 0.0f && dlssSharpenedOutput;
	const bool useNeuralOutput = settings.dlssNREnabled != 0 && dlssSharpenedOutput;
	sl::DLSSNROptions dlssNROptions{};
	dlssNROptions.mode = settings.dlssNREnabled != 0 ? sl::DLSSNRMode::eOn : sl::DLSSNRMode::eOff;
	dlssNROptions.performanceMode = settings.dlssNRPerformanceMode == 5 ? 6 : settings.dlssNRPerformanceMode;
	dlssNROptions.preset = settings.dlssNRPreset;
	dlssNROptions.style = settings.dlssNRStyle;
	dlssNROptions.intensity = settings.dlssNRIntensity;
	dlssNROptions.localToneStrength = settings.dlssNRLocalToneStrength;
	dlssNROptions.localStructureStrength = settings.dlssNRLocalStructureStrength;
	dlssNROptions.globalToneStrength = settings.dlssNRGlobalToneStrength;
	dlssNROptions.useAutoMask = settings.dlssNRUseAutoMask != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	dlssNROptions.skinStructureStrength = settings.dlssNRSkinStructureStrength;
	const auto succeeded = Streamline::GetSingleton()->UpscaleD3D12(
		dlssInput,
		dlssOutput,
		useSharpenedOutput || useNeuralOutput ? dlssSharpenedOutput : nullptr,
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
		dlssNROptions,
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
	if (succeeded) {
		ClearFeatureRequestFailure(FeatureRequest::kDLSS);
	} else {
		logger::warn(
			"[Upscaling] D3D12 DLSS evaluate returned false frameIndex={} token={} input={} output={} mvec={} depth={} transparency={} render={}x{} display={}x{} formats color={} mvec={} depth={}",
			a_frameIndex,
			static_cast<uint32_t>(*frameToken),
			static_cast<void*>(dlssInput),
			static_cast<void*>(dlssOutput),
			static_cast<void*>(motionVectors),
			static_cast<void*>(depth),
			static_cast<void*>(transparencyMask),
			dlssgInputRenderSizes[a_frameIndex].x,
			dlssgInputRenderSizes[a_frameIndex].y,
			dlssgInputDisplaySizes[a_frameIndex].x,
			dlssgInputDisplaySizes[a_frameIndex].y,
			magic_enum::enum_name(dlssD3D12ColorFormats[a_frameIndex]),
			magic_enum::enum_name(dlssD3D12MotionVectorFormats[a_frameIndex]),
			magic_enum::enum_name(dlssD3D12DepthFormats[a_frameIndex]));
		ReportFeatureRequestFailure(FeatureRequest::kDLSS, "D3D12 DLSS evaluate");
	}
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
		ReportFeatureRequestFailure(FeatureRequest::kFSR, "D3D12 FSR inputs");
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
	if (succeeded) {
		ClearFeatureRequestFailure(FeatureRequest::kFSR);
	} else {
		ReportFeatureRequestFailure(FeatureRequest::kFSR, "D3D12 FSR evaluate");
	}
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
	if (!color || !motionVectors || !depth || !a_commandList) {
		fsrFrameGenerationInputsReady[a_frameIndex] = false;
		ReportFeatureRequestFailure(FeatureRequest::kFSRFrameGeneration, "FSR frame generation inputs");
		return false;
	}

	const auto shaderReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	D3D12_RESOURCE_BARRIER beforeDispatch[3]{};
	UINT beforeDispatchCount = 0;
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(color, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
	beforeDispatch[beforeDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, shaderReadState);
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
		nullptr,
		jitter,
		dlssgInputRenderSizes[a_frameIndex],
		dlssgInputDisplaySizes[a_frameIndex],
		fsrFrameGenerationColorFormats[a_frameIndex],
		fsrFrameGenerationFrameIDs[a_frameIndex],
		true);

	D3D12_RESOURCE_BARRIER afterDispatch[3]{};
	UINT afterDispatchCount = 0;
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(color, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	afterDispatch[afterDispatchCount++] = CD3DX12_RESOURCE_BARRIER::Transition(depth, shaderReadState, D3D12_RESOURCE_STATE_COMMON);
	a_commandList->ResourceBarrier(afterDispatchCount, afterDispatch);

	fsrFrameGenerationInputsReady[a_frameIndex] = false;
	if (succeeded) {
		ClearFeatureRequestFailure(FeatureRequest::kFSRFrameGeneration);
	} else {
		ReportFeatureRequestFailure(FeatureRequest::kFSRFrameGeneration, "FSR frame generation evaluate");
	}
	return succeeded;
}

void Upscaling::TagDLSSGInputs(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex)
{
	if (a_frameIndex >= dlssgInputsReady.size() || !dlssgInputsReady[a_frameIndex]) {
		auto streamline = Streamline::GetSingleton();
		if (streamline->NeedsDLSSGPresentSafety()) {
			if (!frameGenerationActive && streamline->dlssgActive) {
				streamline->RequestDLSSGDisable();
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
		nullptr,
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

	if (Streamline::GetSingleton()->featureDLSS) {
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		EnsureTexture2D(texDesc, dlssOutputTexture, false, true);
	}

	// Create dilated motion vector texture for DLSS and FSR.
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
	EnsureTexture2D(texDesc, dilatedMotionVectorTexture, false, true);
}

void Upscaling::DestroyUpscalingResources()
{
	dlssOutputTexture = nullptr;
	spatialFallbackTexture = nullptr;
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
		dlssTransparencyMaskSharedTextures[i] = nullptr;
		debugMotionVectorSharedTextures[i] = nullptr;
		fsrInputSharedTextures[i] = nullptr;
		fsrOutputSharedTextures[i] = nullptr;
		fsrOpaqueOnlySharedTextures[i] = nullptr;
		fsrReactiveMaskSharedTextures[i] = nullptr;
		fsrMotionVectorSharedTextures[i] = nullptr;
		fsrDepthSharedTextures[i] = nullptr;
		dlssInputD3D12[i] = nullptr;
		dlssSharpenedD3D12[i] = nullptr;
		dlssD3D12PresentFinal[i] = nullptr;
		dlssgHUDLessD3D12[i] = nullptr;
		dlssgMotionVectorD3D12[i] = nullptr;
		dlssgDepthD3D12[i] = nullptr;
		dlssTransparencyMaskD3D12[i] = nullptr;
		debugMotionVectorD3D12[i] = nullptr;
		fsrInputD3D12[i] = nullptr;
		fsrOutputD3D12[i] = nullptr;
		fsrOpaqueOnlyD3D12[i] = nullptr;
		fsrReactiveMaskD3D12[i] = nullptr;
		fsrMotionVectorD3D12[i] = nullptr;
		fsrDepthD3D12[i] = nullptr;
		dlssgInputsReady[i] = false;
		fsrFrameGenerationInputsReady[i] = false;
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

namespace
{
	struct alignas(16) SSLRCoordinateCB
	{
		float2 allocationUVScale;
		float2 inverseAllocationUVScale;
	};
	static_assert(sizeof(SSLRCoordinateCB) == 16);

	ID3D11Buffer* GetSSLRCoordinateBuffer(float a_widthScale, float a_heightScale)
	{
		static std::unique_ptr<ConstantBuffer> coordinateBuffer;
		static SSLRCoordinateCB currentData{};
		static bool initialized = false;

		if (!coordinateBuffer) {
			coordinateBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SSLRCoordinateCB>(true));
		}

		const SSLRCoordinateCB data{
			{ a_widthScale, a_heightScale },
			{ 1.0f / std::max(a_widthScale, std::numeric_limits<float>::epsilon()),
				1.0f / std::max(a_heightScale, std::numeric_limits<float>::epsilon()) }
		};
		if (!initialized || std::memcmp(&currentData, &data, sizeof(data)) != 0) {
			coordinateBuffer->Update(data);
			currentData = data;
			initialized = true;
		}

		return coordinateBuffer->CB();
	}

	std::string_view DescribeSSLRInput(
		const Upscaling* a_upscaling,
		ID3D11ShaderResourceView* a_view,
		std::array<char, 32>& a_storage)
	{
		if (!a_view) {
			return "null";
		}
		if (a_upscaling) {
			for (std::size_t target = 0; target < std::size(a_upscaling->originalRenderTargets); ++target) {
				if (a_view == reinterpret_cast<ID3D11ShaderResourceView*>(
						a_upscaling->originalRenderTargets[target].srView)) {
					const auto result = std::format_to_n(
						a_storage.data(), a_storage.size() - 1, "original[{}]", target);
					const auto length = std::min<std::size_t>(
						static_cast<std::size_t>(result.size), a_storage.size() - 1);
					a_storage[length] = '\0';
					return { a_storage.data(), length };
				}
				if (a_view == reinterpret_cast<ID3D11ShaderResourceView*>(
						a_upscaling->proxyRenderTargets[target].srView)) {
					const auto result = std::format_to_n(
						a_storage.data(), a_storage.size() - 1, "proxy[{}]", target);
					const auto length = std::min<std::size_t>(
						static_cast<std::size_t>(result.size), a_storage.size() - 1);
					a_storage[length] = '\0';
					return { a_storage.data(), length };
				}
			}
		}
		return "external";
	}

	void LogSSLRBindings(
		ID3D11DeviceContext* a_context,
		float a_widthScale,
		float a_heightScale)
	{
		auto* upscaling = Upscaling::GetSingleton();
		if (!a_context || !upscaling || upscaling->settings.imageSpaceEffectLog == 0) {
			return;
		}

		std::array<ID3D11ShaderResourceView*, 4> inputs{};
		a_context->PSGetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
		std::array<std::array<char, 32>, 4> inputNames{};
		std::array<UINT, 4> widths{};
		std::array<UINT, 4> heights{};
		for (std::size_t index = 0; index < inputs.size(); ++index) {
			GetENBShaderResourceDimensions(inputs[index], widths[index], heights[index]);
			const auto name = DescribeSSLRInput(upscaling, inputs[index], inputNames[index]);
			if (name.data() != inputNames[index].data()) {
				const auto length = std::min(name.size(), inputNames[index].size() - 1);
				std::memcpy(inputNames[index].data(), name.data(), length);
				inputNames[index][length] = '\0';
			}
		}

		winrt::com_ptr<ID3D11RenderTargetView> output;
		a_context->OMGetRenderTargets(1, output.put(), nullptr);
		UINT outputWidth = 0;
		UINT outputHeight = 0;
		GetENBRenderTargetDimensions(output.get(), outputWidth, outputHeight);

		D3D11_VIEWPORT viewport{};
		UINT viewportCount = 1;
		a_context->RSGetViewports(&viewportCount, &viewport);

		static std::array<ID3D11ShaderResourceView*, 4> previousInputs{};
		static std::array<UINT, 4> previousWidths{};
		static std::array<UINT, 4> previousHeights{};
		static UINT previousOutputWidth = 0;
		static UINT previousOutputHeight = 0;
		static D3D11_VIEWPORT previousViewport{};
		const bool unchanged = inputs == previousInputs && widths == previousWidths &&
			heights == previousHeights && outputWidth == previousOutputWidth &&
			outputHeight == previousOutputHeight &&
			std::memcmp(&viewport, &previousViewport, sizeof(viewport)) == 0;
		if (!unchanged) {
			logger::info(
				"[ENB SSLR] ratio={:.6f}x{:.6f} viewport={:.3f}x{:.3f} output={}x{} "
				"t0={} {}x{} t1={} {}x{} t2={} {}x{} t3={} {}x{}",
				a_widthScale,
				a_heightScale,
				viewport.Width,
				viewport.Height,
				outputWidth,
				outputHeight,
				inputNames[0].data(), widths[0], heights[0],
				inputNames[1].data(), widths[1], heights[1],
				inputNames[2].data(), widths[2], heights[2],
				inputNames[3].data(), widths[3], heights[3]);
			previousInputs = inputs;
			previousWidths = widths;
			previousHeights = heights;
			previousOutputWidth = outputWidth;
			previousOutputHeight = outputHeight;
			previousViewport = viewport;
		}

		for (auto* input : inputs) {
			if (input) {
				input->Release();
			}
		}
	}
}

void Upscaling::PatchSSRShader()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	const auto ratios = GetDynamicResolutionRatios();
	LogSSLRBindings(
		context,
		ratios.width,
		ratios.height);

	// The ray equation and depth-mip DDA stay in allocation UV. The scale
	// supplies the active allocation bounds and converts confidence distances
	// back to logical UV without perturbing integer mip-cell traversal.
	auto buffer = GetSSLRCoordinateBuffer(
		ratios.width,
		ratios.height);
	context->PSSetConstantBuffers(13, 1, &buffer);

	// Replace the game's SSR pixel shader with our custom one that fixes scaled render targets
	context->PSSetShader(GetBSImagespaceShaderSSLRRaytracing(), nullptr, 0);
}
