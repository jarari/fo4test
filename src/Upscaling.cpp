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
#include <fstream>
#include <intrin.h>
#include <limits>
#include <memory>
#include <optional>
#include <SimpleIni.h>
#include <unordered_set>
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
	winrt::com_ptr<ID3D11Texture2D> g_enbNativeDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> g_enbNativeDepthSRV;
	winrt::com_ptr<ID3D11RenderTargetView> g_enbNativeDepthRTV;
	std::uintptr_t g_enbTextureOriginalSRVAddress = 0;
	std::array<std::uintptr_t, 2> g_enbPrepassDepthSRVAddresses{};
	std::uint32_t* g_enbFullWidth = nullptr;
	std::uint32_t* g_enbFullHeight = nullptr;
	using ENBScreenEffectRender_t = int (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t);
	using D3D11Draw_t = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
	using D3D11DrawIndexed_t = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	using D3D11DrawIndexedInstanced_t = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	using D3D11PSSetShaderResources_t = void (STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
	ENBScreenEffectRender_t g_enbDetailedShadowRender = nullptr;
	bool g_enbDetailedShadowUsesFirstPersonModelPath = false;
	struct ENBDetailedShadowBufferLayout
	{
		std::size_t cpuDataOffset;
		std::size_t gpuBufferOffset;
	};
	std::optional<ENBDetailedShadowBufferLayout> g_enbDetailedShadowBufferLayout;
	ENBScreenEffectRender_t g_enbDepthOfFieldRender = nullptr;
	ENBScreenEffectRender_t g_enbReflectionRender = nullptr;
	ENBScreenEffectRender_t g_enbPuddleWaterRender = nullptr;
	std::optional<std::size_t> g_enbReflectionConstantBufferOffset;
	std::optional<std::size_t> g_enbPuddleWaterConstantBufferOffset;
	std::optional<std::size_t> g_enbPuddleWaterTexture11Offset;
	struct ENBConstantBufferPatchResources
	{
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11Buffer> proxy;
		winrt::com_ptr<ID3D11Buffer> patch;
		UINT byteWidth{ 0 };
	};
	ENBConstantBufferPatchResources g_enbReflectionConstantBufferPatch;
	ENBConstantBufferPatchResources g_enbPuddleWaterConstantBufferPatch;
	constexpr std::size_t kENBDeferMixTrampolineSize = 64;
	REL::Trampoline g_enbDeferMixTrampoline{ "ENB DeferMix"sv };
	bool g_enbDeferMixInstalled = false;
	constexpr std::uint32_t kReflectionCaptureVanillaSSLR = 1u << 0;
	constexpr std::uint32_t kReflectionCaptureENBPuddle = 1u << 1;
	constexpr std::uint32_t kReflectionCaptureENBReflection = 1u << 2;
	// Armed after Shader_418BF7EB writes its material MRTs.  This must remain a
	// separate bit from the material capture: the final puddle reflection is
	// produced by a later pass, not by Shader_418BF7EB itself.
	constexpr std::uint32_t kReflectionCaptureENBPuddleConsumer = 1u << 3;
	std::atomic_uint32_t g_reflectionCapturePending{ 0 };
	std::atomic_uint32_t g_reflectionCaptureSerial{ 0 };
	std::atomic_uint32_t g_activeReflectionCaptureSerial{ 0 };
	constexpr std::size_t kD3D11PSSetShaderResourcesVTableIndex = 8;
	constexpr std::size_t kD3D11DrawIndexedVTableIndex = 12;
	constexpr std::size_t kD3D11DrawVTableIndex = 13;
	constexpr std::size_t kD3D11DrawIndexedInstancedVTableIndex = 20;
	// The D3D12 proxy queries this same object as ID3D11DeviceContext4 and calls
	// Signal/Wait (slots 147/148).  A 115-entry base ID3D11DeviceContext clone
	// leaves those calls beyond the cloned allocation and corrupts synchronization.
	constexpr std::size_t kD3D11DeviceContext4SignalVTableIndex = 147;
	constexpr std::size_t kD3D11DeviceContext4WaitVTableIndex = kD3D11DeviceContext4SignalVTableIndex + 1;
	constexpr std::size_t kD3D11ContextVTableSize = kD3D11DeviceContext4WaitVTableIndex + 1;
	std::unique_ptr<std::array<void*, kD3D11ContextVTableSize>> g_reflectionCaptureVTable;
	void** g_reflectionCaptureOriginalVTable = nullptr;
	ID3D11DeviceContext* g_reflectionCaptureContext = nullptr;
	D3D11Draw_t g_reflectionCaptureOriginalDraw = nullptr;
	D3D11DrawIndexed_t g_reflectionCaptureOriginalDrawIndexed = nullptr;
	D3D11DrawIndexedInstanced_t g_reflectionCaptureOriginalDrawIndexedInstanced = nullptr;
	D3D11PSSetShaderResources_t g_reflectionCaptureOriginalPSSetShaderResources = nullptr;
	std::uintptr_t g_enbPuddleWaterDrawReturnAddress = 0;
	struct ENBPuddleMaterialConsumerTrace
	{
		ID3D11DeviceContext* context{ nullptr };
		std::array<winrt::com_ptr<ID3D11Resource>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputs{};
		std::uint32_t outputCount{ 0 };
		std::uint16_t boundSlots{ 0 };
		std::unordered_set<std::uint64_t> consumerKeys;
		std::uint32_t drawsRemaining{ 0 };
	};
	ENBPuddleMaterialConsumerTrace g_enbPuddleMaterialConsumerTrace;
	constexpr std::size_t kENBPuddleCaptureTrampolineSize = 64;
	REL::Trampoline g_enbPuddleCaptureTrampoline{ "ENB Puddle Capture"sv };
	bool g_enbPuddleCaptureInstalled = false;
	ID3D11ShaderResourceView** g_enbDepthTextureSRVSlot = nullptr;
	thread_local int g_enbPrimaryCompositeScopeDepth = 0;
	thread_local int g_enbNativeImageSpaceParamScopeDepth = 0;
	thread_local int g_enbReflectionCaptureScopeDepth = 0;
	thread_local std::uint32_t g_enbReflectionCaptureTechnique = 0;
	thread_local int g_enbPrepassDepthBridgeScopeDepth = 0;
	float* GetGlobalDynamicWidthRatio();
	float* GetGlobalDynamicHeightRatio();
	void InstallENBScreenEffectRenderHooks();
	bool InstallReflectionDrawCaptureHook(ID3D11DeviceContext* a_context);
	thread_local std::array<void*, 2> g_enbHDRFinalCompositeEffects{};
	thread_local void* g_enbRefractionCompositeEffect = nullptr;
	thread_local std::array<void*, 0x20> g_enbNativeImageSpaceShaders{};
	thread_local std::size_t g_enbNativeImageSpaceShaderCount = 0;
	std::array<std::atomic_bool, 0x48> g_loggedENBNativeImageSpaceEffects{};

	constexpr std::ptrdiff_t kImageSpaceEffectUseDynamicResolutionOffset = 0xA8;
	constexpr std::ptrdiff_t kImageSpaceEffectListOffset = 0x18;
	constexpr std::ptrdiff_t kImageSpaceEffectCountOffset = 0x22;
	constexpr std::ptrdiff_t kImageSpaceManagerNativeGeometryOffset = 0x28;
	constexpr std::ptrdiff_t kServingThreadStateOffset = 0x68;
	constexpr std::ptrdiff_t kHFPFDisableLoadingAnimationPatchOffset = 0x19D;
	constexpr std::array<std::uint8_t, 4> kHFPFDisableLoadingAnimationPatch{ 0x0F, 0x1F, 0x40, 0x00 };
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

	bool HasHFPFDisableLoadingAnimationPatch(std::uintptr_t a_displayLoadingScreen)
	{
		std::array<std::uint8_t, kHFPFDisableLoadingAnimationPatch.size()> bytes{};
		std::memcpy(
			bytes.data(),
			reinterpret_cast<const void*>(a_displayLoadingScreen + kHFPFDisableLoadingAnimationPatchOffset),
			bytes.size());
		return bytes == kHFPFDisableLoadingAnimationPatch;
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
		ScopedENBHDRFinalCompositeEffects(void* a_effect, int32_t a_effectIndex) :
			previousEffects_(g_enbHDRFinalCompositeEffects)
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
		ScopedENBNativeImageSpaceShaders(void* a_effect, int32_t a_effectIndex) :
			previousShaders_(g_enbNativeImageSpaceShaders),
			previousCount_(g_enbNativeImageSpaceShaderCount)
		{
			if (!a_effect || g_enbNativeImageSpaceParamScopeDepth <= 0 ||
				!IsENBNativeImageSpaceEffectIndex(a_effectIndex)) {
				return;
			}

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
		ScopedENBRefractionCompositeEffect(void* a_effect, int32_t a_effectIndex) :
			previousEffect_(g_enbRefractionCompositeEffect)
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

	uint32_t GetRenderPassFlags(const RE::BSRenderPass* a_renderPass)
	{
		if (!a_renderPass) {
			return 0;
		}

		return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const std::byte*>(a_renderPass) + 0x48);
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
		kReflections,
		kPuddleReflections,
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
		case ENBBoolSetting::kReflections:
			entry.value = QueryENBBool("EFFECT", "EnableReflections");
			break;
		case ENBBoolSetting::kPuddleReflections:
			entry.value = QueryENBBool("EFFECT", "EnablePuddleReflections");
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

	bool IsENBReflectionCompositePass(const RE::BSRenderPass* a_renderPass)
	{
		constexpr uint32_t kBSDFCompositeEnvmap = 1u << 5;
		constexpr uint32_t kBSDFCompositeDecal = 1u << 8;
		constexpr uint32_t kBSDFCompositeSSLR = 1u << 11;
		constexpr uint32_t kReflectionFlags =
			kBSDFCompositeEnvmap |
			kBSDFCompositeDecal |
			kBSDFCompositeSSLR;
		return (GetRenderPassFlags(a_renderPass) & kReflectionFlags) == kReflectionFlags;
	}

	bool ShouldLeaveENBDeferredCompositeFeaturePassNative(
		const RE::BSRenderPass* a_renderPass,
		Upscaling::UpscaleMethod a_upscaleMethod)
	{
		if (!IsENBSRCompatibilityActive(a_upscaleMethod)) {
			return false;
		}

		// SSS shares this deferred-composite interception but is independent of
		// EnableReflections. Preserve its established native path first.
		if (TryGetENBBool(ENBBoolSetting::kSubSurfaceScattering).value_or(false)) {
			return true;
		}

		return TryGetENBBool(ENBBoolSetting::kReflections).value_or(true) &&
			IsENBReflectionCompositePass(a_renderPass);
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

	void SetDynamicResolutionRatio(RE::BSGraphics::RenderTargetManager* a_renderTargetManager, float a_widthRatio, float a_heightRatio)
	{
		a_renderTargetManager->dynamicWidthRatio = a_widthRatio;
		a_renderTargetManager->dynamicHeightRatio = a_heightRatio;
		a_renderTargetManager->isDynamicResolutionCurrentlyActivated = a_widthRatio != 1.0f || a_heightRatio != 1.0f;

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

		auto*** const object = reinterpret_cast<void***>(g_enbNativeContext.get());
		if (!object || !*object || !**object) {
			return false;
		}

		MEMORY_BASIC_INFORMATION memory{};
		if (VirtualQuery(**object, &memory, sizeof(memory)) != sizeof(memory)) {
			return false;
		}
		const auto enbModule = FindENBModule();
		return enbModule && memory.AllocationBase != enbModule;
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
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
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

	struct ENBScaleCopyJob
	{
		ID3D11Texture2D* sourceTexture{ nullptr };
		ID3D11ShaderResourceView* sourceSRV{ nullptr };
		ID3D11RenderTargetView* destinationRTV{ nullptr };
		ID3D11Texture2D* destinationTexture{ nullptr };
		D3D11_TEXTURE2D_DESC destinationDesc{};
		DXGI_FORMAT sourceViewFormat{ DXGI_FORMAT_UNKNOWN };
		DXGI_FORMAT destinationViewFormat{ DXGI_FORMAT_UNKNOWN };
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
				D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
				savedPSResources_.data());

			context_->PSSetShaderResources(
				0,
				D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
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
				D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
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
		std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> savedPSResources_{};
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
			active_ = true;
		}

		~ScopedENBScaleCopyState()
		{
			if (!active_) {
				return;
			}

			context_->PSSetShaderResources(0, savedPSResourceCount_, kNullD3D11ShaderResources.data());
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
					job.destinationDesc.SampleDesc.Quality != first.destinationDesc.SampleDesc.Quality ||
					job.sourceViewFormat != first.sourceViewFormat ||
					job.destinationViewFormat != first.destinationViewFormat) {
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
		UINT savedPSResourceCount_{ 1 };
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
		if (EnsureENBNativeD3D11Context(a_destinationTexture) &&
			CanBypassENBD3D11Context(a_context)) {
			// Create helper shaders/states on the same real device used by the
			// native fast path. ENB's context wrapper can forward those native
			// objects, while the inverse is not guaranteed for wrapper-created ones.
			resourceDevice = g_enbNativeDevice.get();
		}
		ScopedENBScaleCopyState state(resourceDevice, a_context);
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

		const auto widthRatio = renderTargetManager->dynamicWidthRatio;
		const auto heightRatio = renderTargetManager->dynamicHeightRatio;
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
		MEMORY_BASIC_INFORMATION memory{};
		if (!a_address || a_size == 0 ||
			VirtualQuery(a_address, &memory, sizeof(memory)) != sizeof(memory) ||
			memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
			return false;
		}

		const auto begin = reinterpret_cast<std::uintptr_t>(a_address);
		const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
		const auto regionEnd = regionBegin + memory.RegionSize;
		return begin >= regionBegin && begin <= regionEnd && a_size <= regionEnd - begin;
	}

	struct ENBFunctionRange
	{
		std::uintptr_t begin;
		std::uintptr_t end;
	};

	std::optional<ENBFunctionRange> FindENBContainingFunction(
		HMODULE a_module,
		std::uintptr_t a_address)
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
		if (nt->Signature != IMAGE_NT_SIGNATURE ||
			a_address < base || a_address >= base + nt->OptionalHeader.SizeOfImage) {
			return std::nullopt;
		}

		const auto& directory =
			nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		if (!directory.VirtualAddress || directory.Size < sizeof(RUNTIME_FUNCTION)) {
			return std::nullopt;
		}

		const auto* functions = reinterpret_cast<const RUNTIME_FUNCTION*>(
			base + directory.VirtualAddress);
		const auto count = directory.Size / sizeof(RUNTIME_FUNCTION);
		const auto rva = static_cast<DWORD>(a_address - base);
		for (DWORD i = 0; i < count; ++i) {
			if (rva >= functions[i].BeginAddress && rva < functions[i].EndAddress) {
				return ENBFunctionRange{
					base + functions[i].BeginAddress,
					base + functions[i].EndAddress
				};
			}
		}
		return std::nullopt;
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

	bool ENBFunctionReferencesString(
		HMODULE a_module,
		const ENBFunctionRange& a_function,
		std::string_view a_value)
	{
		for (auto address = a_function.begin; address + 7 <= a_function.end; ++address) {
			std::uintptr_t target = 0;
			if (TryResolveENBRIPRelativeLEA(
					reinterpret_cast<const std::uint8_t*>(address), target) &&
				IsENBModuleStringAt(a_module, target, a_value)) {
				return true;
			}
		}
		return false;
	}

	std::optional<std::size_t> FindENBConstantBufferSlotOffset(
		HMODULE a_module,
		const ENBFunctionRange& a_function,
		std::string_view a_constantBufferName)
	{
		for (auto address = a_function.begin; address + 7 <= a_function.end; ++address) {
			std::uintptr_t target = 0;
			if (!TryResolveENBRIPRelativeLEA(
					reinterpret_cast<const std::uint8_t*>(address), target) ||
				!IsENBModuleStringAt(a_module, target, a_constantBufferName)) {
				continue;
			}

			const auto readSlotOffset = [](std::uintptr_t a_cursor) -> std::optional<std::size_t> {
				const auto* load = reinterpret_cast<const std::uint8_t*>(a_cursor);
				// mov r8,[nonvolatile+disp32], used as SetConstantBuffer's third argument.
				if ((load[0] & 0xFC) != 0x4C || load[1] != 0x8B ||
					(load[2] & 0xF8) != 0x80 || (load[2] & 7) == 4) {
					return std::nullopt;
				}

				std::int32_t displacement = 0;
				std::memcpy(&displacement, load + 3, sizeof(displacement));
				if (displacement >= 0x100 && displacement <= 0x2000) {
					return static_cast<std::size_t>(displacement);
				}
				return std::nullopt;
			};

			// MSVC normally materializes r8 immediately before the name's LEA.
			// Select the nearest argument load so adjacent EConstantList1/List2
			// bindings cannot be confused with one another.
			const auto backwardLimit = std::min<std::uintptr_t>(24, address - a_function.begin);
			for (std::uintptr_t distance = 1; distance <= backwardLimit; ++distance) {
				if (const auto result = readSlotOffset(address - distance)) {
					return result;
				}
			}
			const auto forwardLimit = std::min(a_function.end, address + 7 + 24);
			for (auto cursor = address + 7; cursor + 7 <= forwardLimit; ++cursor) {
				if (const auto result = readSlotOffset(cursor)) {
					return result;
				}
			}
		}
		return std::nullopt;
	}

	struct ENBTechniqueHandler
	{
		std::uintptr_t entry;
		std::size_t constantBufferOffset;
	};

	struct ENBPuddleWaterHandler
	{
		std::uintptr_t entry;
		std::size_t constantBufferOffset;
		std::size_t texture11Offset;
	};

	std::optional<ENBTechniqueHandler> FindENBTechniqueHandler(
		HMODULE a_module,
		const ENBCodeRange& a_code,
		std::string_view a_technique,
		std::string_view a_constantBuffer,
		std::string_view a_requiredSiblingTechnique = {})
	{
		for (std::size_t offset = 0; offset + 7 <= a_code.size; ++offset) {
			const auto* instruction = a_code.begin + offset;
			std::uintptr_t target = 0;
			if (!TryResolveENBRIPRelativeLEA(instruction, target) ||
				!IsENBModuleStringAt(a_module, target, a_technique)) {
				continue;
			}

			const auto function = FindENBContainingFunction(
				a_module, reinterpret_cast<std::uintptr_t>(instruction));
			if (!function ||
				(!a_requiredSiblingTechnique.empty() &&
					!ENBFunctionReferencesString(a_module, *function, a_requiredSiblingTechnique))) {
				continue;
			}

			const auto constantBufferOffset =
				FindENBConstantBufferSlotOffset(a_module, *function, a_constantBuffer);
			if (constantBufferOffset) {
				return ENBTechniqueHandler{ function->begin, *constantBufferOffset };
			}
		}
		return std::nullopt;
	}

	std::optional<ENBPuddleWaterHandler> FindENBPuddleWaterHandler(
		HMODULE a_module,
		const ENBCodeRange& a_code)
	{
		const auto technique = FindENBTechniqueHandler(
			a_module,
			a_code,
			"Shader_AEFCDE5E",
			"EConstantList0",
			"Shader_332B4FCF");
		if (!technique) {
			return std::nullopt;
		}
		const auto function = FindENBContainingFunction(a_module, technique->entry);
		if (!function) {
			return std::nullopt;
		}
		const auto texture11Offset =
			FindENBConstantBufferSlotOffset(a_module, *function, "Texture11");
		if (!texture11Offset) {
			return std::nullopt;
		}
		return ENBPuddleWaterHandler{
			technique->entry,
			technique->constantBufferOffset,
			*texture11Offset
		};
	}

	std::uintptr_t FindENBPuddleWaterDrawCallAddress(
		HMODULE a_module,
		const ENBCodeRange& a_code)
	{
		const auto handler = FindENBPuddleWaterHandler(a_module, a_code);
		if (!handler) {
			return 0;
		}
		const auto function = FindENBContainingFunction(a_module, handler->entry);
		if (!function) {
			return 0;
		}

		// The primary technique draw is inside an effect-pass loop and is followed
		// by `inc r32`. The later fallback DrawIndexed in the same handler is not.
		for (auto cursor = function->begin; cursor + 5 <= function->end; ++cursor) {
			const auto* instruction = reinterpret_cast<const std::uint8_t*>(cursor);
			if (instruction[0] == 0xFF && instruction[1] == 0x50 && instruction[2] == 0x60 &&
				instruction[3] == 0xFF && (instruction[4] & 0xF8) == 0xC0) {
				return cursor;
			}
		}
		return 0;
	}

	bool IsENBPuddleWaterDrawReturnAddress(
		HMODULE a_module,
		const ENBCodeRange& a_code,
		std::uintptr_t a_returnAddress)
	{
		if (!a_module || !a_returnAddress ||
			a_returnAddress < reinterpret_cast<std::uintptr_t>(a_code.begin) + 3 ||
			a_returnAddress + 2 > reinterpret_cast<std::uintptr_t>(a_code.begin) + a_code.size) {
			return false;
		}
		if (a_returnAddress == g_enbPuddleWaterDrawReturnAddress) {
			return true;
		}

		// ID3D11DeviceContext::DrawIndexed is vtable slot 12 (offset 0x60).
		// In ENB's primary puddle/water technique loop the call is immediately
		// followed by `inc r32`. Validate the observed return address itself so a
		// startup scan cannot silently classify a different, similarly shaped call.
		const auto* call = reinterpret_cast<const std::uint8_t*>(a_returnAddress - 3);
		const auto* continuation = reinterpret_cast<const std::uint8_t*>(a_returnAddress);
		if (call[0] != 0xFF || call[1] != 0x50 || call[2] != 0x60 ||
			continuation[0] != 0xFF || (continuation[1] & 0xF8) != 0xC0) {
			return false;
		}

		const auto function = FindENBContainingFunction(a_module, a_returnAddress - 3);
		if (!function) {
			return false;
		}
		const auto containsImmediate = [&](std::uint32_t a_value) {
			std::array<std::uint8_t, sizeof(a_value)> bytes{};
			std::memcpy(bytes.data(), &a_value, sizeof(a_value));
			return std::search(
				reinterpret_cast<const std::uint8_t*>(function->begin),
				reinterpret_cast<const std::uint8_t*>(function->end),
				bytes.begin(), bytes.end()) != reinterpret_cast<const std::uint8_t*>(function->end);
		};
		// This handler selects its water techniques by hash at runtime. Requiring
		// direct xrefs to their diagnostic names rejected the verified 0.501 draw.
		if (!containsImmediate(0xAEFCDE5Eu) || !containsImmediate(0x332B4FCFu)) {
			return false;
		}

		const auto previous = g_enbPuddleWaterDrawReturnAddress;
		g_enbPuddleWaterDrawReturnAddress = a_returnAddress;
		logger::info(
			"[Reflection Capture] Validated ENB primary puddle/water draw at RVA 0x{:X}{}",
			a_returnAddress - 3 - reinterpret_cast<std::uintptr_t>(a_module),
			previous && previous != a_returnAddress ? " (corrected startup candidate)" : "");
		return true;
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
		return upscaling && renderTargetManager &&
			(renderTargetManager->dynamicHeightRatio != 1.0f || renderTargetManager->dynamicWidthRatio != 1.0f) &&
			IsENBSRCompatibilityActive(upscaling->upscaleMethod);
	}

	constexpr std::ptrdiff_t kENBDeviceContextOffset = 0x6C20;
	constexpr std::ptrdiff_t kENBTexture0SRVOffset = 0xD20;
	constexpr std::uint32_t kENBReflectionBlurTechnique = 0xB1B77A4D;
	constexpr std::uint32_t kENBReflectionSSRTechnique = 0x1355B81A;

	std::optional<std::uint32_t> GetENBTechniqueHash(void* a_this)
	{
		if (!a_this) {
			return std::nullopt;
		}

		auto** stateSlot = reinterpret_cast<std::byte**>(
			reinterpret_cast<std::byte*>(a_this) + sizeof(void*));
		if (!IsReadableProcessRange(stateSlot, sizeof(*stateSlot)) || !*stateSlot) {
			return std::nullopt;
		}

		auto* hash = reinterpret_cast<const std::uint32_t*>(*stateSlot + sizeof(void*));
		return IsReadableProcessRange(hash, sizeof(*hash)) ?
			std::optional{ *hash } : std::nullopt;
	}

	ID3D11DeviceContext* GetENBDeviceContext(void* a_this)
	{
		if (!a_this) {
			return nullptr;
		}

		auto** contextSlot = reinterpret_cast<ID3D11DeviceContext**>(
			reinterpret_cast<std::byte*>(a_this) + kENBDeviceContextOffset);
		return IsReadableProcessRange(contextSlot, sizeof(*contextSlot)) ? *contextSlot : nullptr;
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

	class ScopedENBRenderDomainViewport
	{
	public:
		ScopedENBRenderDomainViewport(
			void* a_this,
			bool a_enable,
			std::ptrdiff_t a_fallbackSRVOffset)
		{
			if (!a_enable || !(context_ = GetENBDeviceContext(a_this))) {
				context_ = nullptr;
				return;
			}

			winrt::com_ptr<ID3D11RenderTargetView> output;
			context_->OMGetRenderTargets(1, output.put(), nullptr);
			UINT width = 0;
			UINT height = 0;
			if (!GetENBRenderTargetDimensions(output.get(), width, height)) {
				auto** fallbackSlot = reinterpret_cast<ID3D11ShaderResourceView**>(
					reinterpret_cast<std::byte*>(a_this) + a_fallbackSRVOffset);
				if (!IsReadableProcessRange(fallbackSlot, sizeof(*fallbackSlot)) ||
					!GetENBShaderResourceDimensions(*fallbackSlot, width, height)) {
					context_ = nullptr;
					return;
				}
			}

			viewportCount_ = static_cast<UINT>(viewports_.size());
			context_->RSGetViewports(&viewportCount_, viewports_.data());
			if (viewportCount_ == 0) {
				context_ = nullptr;
				return;
			}

			width_ = static_cast<float>(width);
			height_ = static_cast<float>(height);
			auto viewport = viewports_[0];
			if (viewport.TopLeftX != 0.0f || viewport.TopLeftY != 0.0f ||
				viewport.Width != width_ || viewport.Height != height_) {
				viewport.TopLeftX = 0.0f;
				viewport.TopLeftY = 0.0f;
				viewport.Width = width_;
				viewport.Height = height_;
				context_->RSSetViewports(1, &viewport);
				changed_ = true;
			}
			active_ = true;
		}

		~ScopedENBRenderDomainViewport()
		{
			if (changed_) {
				context_->RSSetViewports(viewportCount_, viewports_.data());
			}
		}

		ScopedENBRenderDomainViewport(const ScopedENBRenderDomainViewport&) = delete;
		ScopedENBRenderDomainViewport& operator=(const ScopedENBRenderDomainViewport&) = delete;

		bool IsActive() const { return active_; }
		std::array<float, 2> Size() const { return { width_, height_ }; }

	private:
		ID3D11DeviceContext* context_{ nullptr };
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
		UINT viewportCount_{ 0 };
		float width_{ 0.0f };
		float height_{ 0.0f };
		bool active_{ false };
		bool changed_{ false };
	};

	bool EnsureENBConstantBufferPatchResources(
		ENBConstantBufferPatchResources& a_resources,
		ID3D11Buffer* a_source)
	{
		if (!a_source) {
			return false;
		}

		D3D11_BUFFER_DESC sourceDesc{};
		a_source->GetDesc(&sourceDesc);
		if ((sourceDesc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0 ||
			sourceDesc.ByteWidth < 16 || (sourceDesc.ByteWidth & 15) != 0 ||
			sourceDesc.ByteWidth > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16 ||
			sourceDesc.MiscFlags != 0 || sourceDesc.StructureByteStride != 0) {
			return false;
		}

		winrt::com_ptr<ID3D11Device> device;
		a_source->GetDevice(device.put());
		if (!device) {
			return false;
		}

		const bool recreate =
			!a_resources.device || a_resources.device.get() != device.get() ||
			!a_resources.proxy || !a_resources.patch ||
			a_resources.byteWidth != sourceDesc.ByteWidth;
		if (!recreate) {
			return true;
		}

		a_resources.patch = nullptr;
		a_resources.proxy = nullptr;
		a_resources.device = nullptr;
		a_resources.byteWidth = 0;

		auto proxyDesc = sourceDesc;
		proxyDesc.Usage = D3D11_USAGE_DEFAULT;
		proxyDesc.CPUAccessFlags = 0;

		D3D11_BUFFER_DESC patchDesc{};
		patchDesc.ByteWidth = 16;
		patchDesc.Usage = D3D11_USAGE_DEFAULT;
		if (FAILED(device->CreateBuffer(&proxyDesc, nullptr, a_resources.proxy.put())) ||
			FAILED(device->CreateBuffer(&patchDesc, nullptr, a_resources.patch.put()))) {
			a_resources.patch = nullptr;
			a_resources.proxy = nullptr;
			return false;
		}

		a_resources.device = std::move(device);
		a_resources.byteWidth = sourceDesc.ByteWidth;
		return true;
	}

	#pragma pack(push, 1)
	struct DDS_PIXELFORMAT
	{
		std::uint32_t size = 32;
		std::uint32_t flags = 0x4;  // DDPF_FOURCC
		std::uint32_t fourCC = 0x30315844;  // DX10
		std::uint32_t rgbBitCount = 0;
		std::uint32_t rBitMask = 0;
		std::uint32_t gBitMask = 0;
		std::uint32_t bBitMask = 0;
		std::uint32_t aBitMask = 0;
	};

	struct DDS_HEADER
	{
		std::uint32_t size = 124;
		std::uint32_t flags = 0x0002100F;  // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
		std::uint32_t height = 0;
		std::uint32_t width = 0;
		std::uint32_t pitchOrLinearSize = 0;
		std::uint32_t depth = 0;
		std::uint32_t mipMapCount = 1;
		std::array<std::uint32_t, 11> reserved1{};
		DDS_PIXELFORMAT pixelFormat{};
		std::uint32_t caps = 0x1000;  // DDSCAPS_TEXTURE
		std::uint32_t caps2 = 0;
		std::uint32_t caps3 = 0;
		std::uint32_t caps4 = 0;
		std::uint32_t reserved2 = 0;
	};

	struct DDS_HEADER_DXT10
	{
		std::uint32_t format = DXGI_FORMAT_UNKNOWN;
		std::uint32_t resourceDimension = 3;  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
		std::uint32_t miscFlag = 0;
		std::uint32_t arraySize = 1;
		std::uint32_t miscFlags2 = 0;
	};
	#pragma pack(pop)
	static_assert(sizeof(DDS_HEADER) == 124);
	static_assert(sizeof(DDS_HEADER_DXT10) == 20);

	std::optional<std::uint32_t> GetCaptureBytesPerPixel(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			return 16;
		case DXGI_FORMAT_R32G32B32_TYPELESS:
		case DXGI_FORMAT_R32G32B32_FLOAT:
		case DXGI_FORMAT_R32G32B32_UINT:
		case DXGI_FORMAT_R32G32B32_SINT:
			return 12;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
			return 8;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UINT:
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_R16G16_TYPELESS:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R16G16_SINT:
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT:
			return 4;
		case DXGI_FORMAT_R8G8_TYPELESS:
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R8G8_SINT:
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R16_SINT:
		case DXGI_FORMAT_B5G6R5_UNORM:
		case DXGI_FORMAT_B5G5R5A1_UNORM:
		case DXGI_FORMAT_B4G4R4A4_UNORM:
			return 2;
		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
		case DXGI_FORMAT_A8_UNORM:
			return 1;
		default:
			return std::nullopt;
		}
	}

	std::filesystem::path GetReflectionCaptureDirectory()
	{
		return std::filesystem::path("Data\\F4SE\\Plugins\\ShaderEngineDumps\\UpscalingReflection") /
			std::format("capture_{:04}", g_activeReflectionCaptureSerial.load());
	}

	bool SaveReflectionCaptureTexture(
		ID3D11DeviceContext* a_context,
		ID3D11Resource* a_resource,
		DXGI_FORMAT a_viewFormat,
		UINT a_mipSlice,
		const std::filesystem::path& a_path)
	{
		if (!a_context || !a_resource) {
			return false;
		}

		winrt::com_ptr<ID3D11Texture2D> source;
		if (FAILED(a_resource->QueryInterface(IID_PPV_ARGS(source.put())))) {
			logger::warn("[Reflection Capture] {} is not a Texture2D", a_path.string());
			return false;
		}
		D3D11_TEXTURE2D_DESC sourceDesc{};
		source->GetDesc(&sourceDesc);
		if (sourceDesc.SampleDesc.Count != 1 || sourceDesc.ArraySize != 1 || a_mipSlice >= sourceDesc.MipLevels) {
			logger::warn("[Reflection Capture] {} has unsupported samples/array ({}/{})", a_path.string(), sourceDesc.SampleDesc.Count, sourceDesc.ArraySize);
			return false;
		}
		const auto format = a_viewFormat == DXGI_FORMAT_UNKNOWN ? sourceDesc.Format : a_viewFormat;
		const auto bytesPerPixel = GetCaptureBytesPerPixel(format);
		if (!bytesPerPixel || sourceDesc.Format == DXGI_FORMAT_UNKNOWN) {
			logger::warn("[Reflection Capture] {} has unsupported format {}", a_path.string(), static_cast<int>(format));
			return false;
		}

		const UINT width = std::max(1u, sourceDesc.Width >> a_mipSlice);
		const UINT height = std::max(1u, sourceDesc.Height >> a_mipSlice);
		D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
		stagingDesc.Width = width;
		stagingDesc.Height = height;
		stagingDesc.MipLevels = 1;
		stagingDesc.BindFlags = 0;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags = 0;
		winrt::com_ptr<ID3D11Device> device;
		source->GetDevice(device.put());
		winrt::com_ptr<ID3D11Texture2D> staging;
		if (!device || FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.put()))) {
			logger::warn("[Reflection Capture] Cannot create staging texture for {}", a_path.string());
			return false;
		}

		a_context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, source.get(), a_mipSlice, nullptr);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
			logger::warn("[Reflection Capture] Cannot map {}", a_path.string());
			return false;
		}

		const auto rowBytes = static_cast<std::uint64_t>(width) * *bytesPerPixel;
		const bool validPitch = rowBytes <= mapped.RowPitch && rowBytes <= std::numeric_limits<std::uint32_t>::max();
		std::error_code error;
		std::filesystem::create_directories(a_path.parent_path(), error);
		std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
		if (!validPitch || !stream) {
			a_context->Unmap(staging.get(), 0);
			logger::warn("[Reflection Capture] Cannot write {}", a_path.string());
			return false;
		}

		const std::uint32_t magic = 0x20534444;  // DDS
		DDS_HEADER header{};
		header.height = height;
		header.width = width;
		header.pitchOrLinearSize = static_cast<std::uint32_t>(rowBytes);
		DDS_HEADER_DXT10 header10{};
		header10.format = static_cast<std::uint32_t>(format);
		stream.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
		stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
		stream.write(reinterpret_cast<const char*>(&header10), sizeof(header10));
		for (UINT row = 0; row < height; ++row) {
			stream.write(
				reinterpret_cast<const char*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
				static_cast<std::streamsize>(rowBytes));
		}
		a_context->Unmap(staging.get(), 0);
		return stream.good();
	}

	bool SaveReflectionCaptureSRV(
		ID3D11DeviceContext* a_context,
		ID3D11ShaderResourceView* a_view,
		const std::filesystem::path& a_path)
	{
		if (!a_view) {
			return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		a_view->GetDesc(&desc);
		UINT mipSlice = 0;
		if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
			mipSlice = desc.Texture2D.MostDetailedMip;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		return SaveReflectionCaptureTexture(a_context, resource.get(), desc.Format, mipSlice, a_path);
	}

	bool SaveReflectionCaptureRTV(
		ID3D11DeviceContext* a_context,
		ID3D11RenderTargetView* a_view,
		const std::filesystem::path& a_path)
	{
		if (!a_view) {
			return false;
		}
		D3D11_RENDER_TARGET_VIEW_DESC desc{};
		a_view->GetDesc(&desc);
		UINT mipSlice = 0;
		if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
			mipSlice = desc.Texture2D.MipSlice;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		return SaveReflectionCaptureTexture(a_context, resource.get(), desc.Format, mipSlice, a_path);
	}

	void CaptureReflectionDrawState(ID3D11DeviceContext* a_context, std::string_view a_pass)
	{
		if (!a_context) {
			return;
		}
		const auto directory = GetReflectionCaptureDirectory();
		std::array<ID3D11ShaderResourceView*, 16> inputs{};
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputs{};
		a_context->PSGetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
		a_context->OMGetRenderTargets(static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
		std::uint32_t savedInputs = 0;
		std::uint32_t savedOutputs = 0;
		for (std::size_t slot = 0; slot < inputs.size(); ++slot) {
			if (inputs[slot]) {
				savedInputs += SaveReflectionCaptureSRV(
					a_context, inputs[slot], directory / std::format("{}_srv{:02}.dds", a_pass, slot)) ? 1u : 0u;
				inputs[slot]->Release();
			}
		}
		for (std::size_t slot = 0; slot < outputs.size(); ++slot) {
			if (outputs[slot]) {
				savedOutputs += SaveReflectionCaptureRTV(
					a_context, outputs[slot], directory / std::format("{}_rt{:02}.dds", a_pass, slot)) ? 1u : 0u;
				outputs[slot]->Release();
			}
		}
		D3D11_VIEWPORT viewport{};
		UINT viewportCount = 1;
		a_context->RSGetViewports(&viewportCount, &viewport);
		logger::info(
			"[Reflection Capture] {} wrote {} SRVs and {} RTVs to {} (viewport {:.3f}x{:.3f})",
			a_pass, savedInputs, savedOutputs, directory.string(), viewport.Width, viewport.Height);
	}

	void ResetReflectionCaptureTrace()
	{
		if (g_reflectionCaptureContext && g_reflectionCaptureOriginalVTable) {
			REL::WriteSafeData(
				reinterpret_cast<std::uintptr_t>(g_reflectionCaptureContext),
				g_reflectionCaptureOriginalVTable);
		}
		g_reflectionCaptureContext = nullptr;
		g_reflectionCaptureOriginalVTable = nullptr;
		g_reflectionCaptureOriginalDraw = nullptr;
		g_reflectionCaptureOriginalDrawIndexed = nullptr;
		g_reflectionCaptureOriginalDrawIndexedInstanced = nullptr;
		g_reflectionCaptureOriginalPSSetShaderResources = nullptr;
		g_reflectionCaptureVTable.reset();
		g_enbPuddleMaterialConsumerTrace = {};
	}

	void CompleteReflectionCapture(std::uint32_t a_pass)
	{
		const auto remaining = g_reflectionCapturePending.fetch_and(~a_pass) & ~a_pass;
		if (remaining == 0) {
			ResetReflectionCaptureTrace();
			logger::info("[Reflection Capture] Complete. Toggle Reflection capture off and on to arm another one-shot capture.");
		}
	}

	bool IsVanillaSSLRShader(ID3D11DeviceContext* a_context)
	{
		if ((g_reflectionCapturePending.load() & kReflectionCaptureVanillaSSLR) == 0 || !a_context) {
			return false;
		}
		winrt::com_ptr<ID3D11PixelShader> shader;
		a_context->PSGetShader(shader.put(), nullptr, nullptr);
		return shader && shader.get() == Upscaling::GetSingleton()->GetBSImagespaceShaderSSLRRaytracing();
	}

	bool IsENBReflectionCaptureDraw()
	{
		return (g_reflectionCapturePending.load() & kReflectionCaptureENBReflection) != 0 &&
			g_enbReflectionCaptureScopeDepth > 0 &&
			g_enbReflectionCaptureTechnique == kENBReflectionSSRTechnique;
	}

	bool IsENBPuddleMaterialResource(ID3D11Resource* a_resource)
	{
		if (!a_resource) {
			return false;
		}
		return std::any_of(
			g_enbPuddleMaterialConsumerTrace.outputs.begin(),
			g_enbPuddleMaterialConsumerTrace.outputs.end(),
			[&](const auto& a_output) { return a_output.get() == a_resource; });
	}

	void ArmENBPuddleMaterialConsumerTrace(ID3D11DeviceContext* a_context)
	{
		if ((g_reflectionCapturePending.load() & kReflectionCaptureENBPuddleConsumer) == 0 || !a_context) {
			return;
		}
		// Latch one material output set for the one-shot interval. Replacing it on
		// every material draw can race past downstream readers and prevents a
		// bounded enumeration of the consumers of one concrete output set.
		if (g_enbPuddleMaterialConsumerTrace.context != nullptr ||
			g_enbPuddleMaterialConsumerTrace.outputCount != 0) {
			return;
		}

		g_enbPuddleMaterialConsumerTrace = {};
		g_enbPuddleMaterialConsumerTrace.context = a_context;
		g_enbPuddleMaterialConsumerTrace.drawsRemaining = 4096;
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputs{};
		a_context->OMGetRenderTargets(static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
		for (auto* output : outputs) {
			if (!output) {
				continue;
			}
			winrt::com_ptr<ID3D11Resource> resource;
			output->GetResource(resource.put());
			output->Release();
			if (!resource) {
				continue;
			}
			const bool duplicate = std::any_of(
				g_enbPuddleMaterialConsumerTrace.outputs.begin(),
				g_enbPuddleMaterialConsumerTrace.outputs.end(),
				[&](const auto& a_existing) { return a_existing.get() == resource.get(); });
			if (!duplicate && g_enbPuddleMaterialConsumerTrace.outputCount <
				g_enbPuddleMaterialConsumerTrace.outputs.size()) {
				g_enbPuddleMaterialConsumerTrace.outputs[g_enbPuddleMaterialConsumerTrace.outputCount++] =
					std::move(resource);
			}
		}

		D3D11_VIEWPORT viewport{};
		UINT viewportCount = 1;
		a_context->RSGetViewports(&viewportCount, &viewport);
		logger::info(
			"[Reflection Capture] Armed ENB puddle consumer trace for {} material resources (viewport {:.3f}x{:.3f})",
			g_enbPuddleMaterialConsumerTrace.outputCount,
			viewport.Width,
			viewport.Height);
	}

	void STDMETHODCALLTYPE ReflectionPSSetShaderResourcesCaptureThunk(
		ID3D11DeviceContext* a_context,
		UINT a_startSlot,
		UINT a_numViews,
		ID3D11ShaderResourceView* const* a_views)
	{
		const auto setShaderResources = g_reflectionCaptureOriginalPSSetShaderResources;
		if (!setShaderResources) {
			return;
		}
		setShaderResources(a_context, a_startSlot, a_numViews, a_views);

		auto& trace = g_enbPuddleMaterialConsumerTrace;
		if ((g_reflectionCapturePending.load(std::memory_order_relaxed) &
			kReflectionCaptureENBPuddleConsumer) == 0 ||
			a_context != trace.context || trace.outputCount == 0 || a_startSlot >= 16) {
			return;
		}

		const auto endSlot = std::min<UINT>(16, a_startSlot + std::min<UINT>(a_numViews, 16));
		for (UINT slot = a_startSlot; slot < endSlot; ++slot) {
			const auto bit = static_cast<std::uint16_t>(1u << slot);
			trace.boundSlots &= static_cast<std::uint16_t>(~bit);
			const auto viewIndex = slot - a_startSlot;
			if (!a_views || !a_views[viewIndex]) {
				continue;
			}
			winrt::com_ptr<ID3D11Resource> resource;
			a_views[viewIndex]->GetResource(resource.put());
			if (IsENBPuddleMaterialResource(resource.get())) {
				trace.boundSlots |= bit;
			}
		}
	}

	void CaptureENBPuddleMaterialConsumer(
		ID3D11DeviceContext* a_context,
		std::uintptr_t a_drawCaller)
	{
		auto& trace = g_enbPuddleMaterialConsumerTrace;
		if ((g_reflectionCapturePending.load(std::memory_order_relaxed) &
			kReflectionCaptureENBPuddleConsumer) == 0 ||
			!a_context || a_context != trace.context || trace.outputCount == 0 || trace.boundSlots == 0) {
			return;
		}

		winrt::com_ptr<ID3D11PixelShader> shader;
		a_context->PSGetShader(shader.put(), nullptr, nullptr);
		const auto shaderAddress = reinterpret_cast<std::uintptr_t>(shader.get());
		const auto consumerKey = static_cast<std::uint64_t>(a_drawCaller) ^
			(static_cast<std::uint64_t>(shaderAddress) + 0x9E3779B97F4A7C15ull +
				(static_cast<std::uint64_t>(a_drawCaller) << 6) +
				(static_cast<std::uint64_t>(a_drawCaller) >> 2));
		if (!trace.consumerKeys.insert(consumerKey).second) {
			return;
		}
		const auto consumerCount = trace.consumerKeys.size();

		D3D11_VIEWPORT viewport{};
		UINT viewportCount = 1;
		a_context->RSGetViewports(&viewportCount, &viewport);
		std::string slots;
		for (std::size_t slot = 0; slot < 16; ++slot) {
			if ((trace.boundSlots & (1u << slot)) != 0) {
				slots += slots.empty() ? std::format("t{}", slot) : std::format(",t{}", slot);
			}
		}

		DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
		UINT outputWidth = 0;
		UINT outputHeight = 0;
		winrt::com_ptr<ID3D11RenderTargetView> output;
		a_context->OMGetRenderTargets(1, output.put(), nullptr);
		if (output) {
			D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
			output->GetDesc(&viewDesc);
			outputFormat = viewDesc.Format;
			winrt::com_ptr<ID3D11Resource> outputResource;
			output->GetResource(outputResource.put());
			winrt::com_ptr<ID3D11Texture2D> outputTexture;
			if (outputResource && SUCCEEDED(outputResource->QueryInterface(IID_PPV_ARGS(outputTexture.put())))) {
				D3D11_TEXTURE2D_DESC textureDesc{};
				outputTexture->GetDesc(&textureDesc);
				outputWidth = textureDesc.Width;
				outputHeight = textureDesc.Height;
				if (outputFormat == DXGI_FORMAT_UNKNOWN) {
					outputFormat = textureDesc.Format;
				}
			}
		}

		const auto enbModule = reinterpret_cast<std::uintptr_t>(FindENBModule());
		MEMORY_BASIC_INFORMATION callerMemory{};
		const bool callerIsENB = a_drawCaller && enbModule &&
			VirtualQuery(reinterpret_cast<void*>(a_drawCaller), &callerMemory, sizeof(callerMemory)) == sizeof(callerMemory) &&
			reinterpret_cast<std::uintptr_t>(callerMemory.AllocationBase) == enbModule;
		const auto enbCode = callerIsENB ? GetENBExecutableCode(reinterpret_cast<HMODULE>(enbModule)) : std::nullopt;
		const bool primaryWaterPass = callerIsENB && enbCode &&
			IsENBPuddleWaterDrawReturnAddress(
				reinterpret_cast<HMODULE>(enbModule), *enbCode, a_drawCaller);
		if (callerIsENB) {
			logger::info(
				"[Reflection Capture] ENB puddle material consumer #{}: PS={} inputs={} viewport {:.3f}x{:.3f} output={}x{} format={} caller=ENB+0x{:X} waterPass={}",
				consumerCount,
				shaderAddress,
				slots,
				viewport.Width,
				viewport.Height,
				outputWidth,
				outputHeight,
				static_cast<UINT>(outputFormat),
				a_drawCaller - enbModule,
				primaryWaterPass);
		} else {
			logger::info(
				"[Reflection Capture] ENB puddle material consumer #{}: PS={} inputs={} viewport {:.3f}x{:.3f} output={}x{} format={} caller=0x{:X}",
				consumerCount,
				shaderAddress,
				slots,
				viewport.Width,
				viewport.Height,
				outputWidth,
				outputHeight,
				static_cast<UINT>(outputFormat),
				a_drawCaller);
		}
		// The broad identity trace finds many depth, lighting and post consumers.
		// Dump only each distinct effect pass at the statically resolved primary
		// water draw; this includes the stencil pass and the following color pass
		// without stalling the frame on dozens of irrelevant readbacks.
		if (primaryWaterPass) {
			CaptureReflectionDrawState(
				a_context, std::format("enb_puddle_water_pass_{:02}", consumerCount));
		}
	}

	void FinishReflectionDrawCapture(
		ID3D11DeviceContext* a_context,
		std::uintptr_t a_drawCaller)
	{
		if (IsVanillaSSLRShader(a_context)) {
			CaptureReflectionDrawState(a_context, "vanilla_sslr");
			CompleteReflectionCapture(kReflectionCaptureVanillaSSLR);
		} else if (IsENBReflectionCaptureDraw()) {
			CaptureReflectionDrawState(a_context, "enb_reflection_ssr");
			CompleteReflectionCapture(kReflectionCaptureENBReflection);
		}
		CaptureENBPuddleMaterialConsumer(a_context, a_drawCaller);
		auto& trace = g_enbPuddleMaterialConsumerTrace;
		if ((g_reflectionCapturePending.load(std::memory_order_relaxed) &
			kReflectionCaptureENBPuddleConsumer) != 0 &&
			a_context == trace.context && trace.drawsRemaining != 0 && --trace.drawsRemaining == 0) {
			logger::info(
				"[Reflection Capture] ENB puddle consumer enumeration complete after {} unique consumers",
				trace.consumerKeys.size());
			CompleteReflectionCapture(kReflectionCaptureENBPuddleConsumer);
		}
	}

	void STDMETHODCALLTYPE VanillaSSLRDrawCaptureThunk(
		ID3D11DeviceContext* a_context,
		UINT a_vertexCount,
		UINT a_startVertexLocation)
	{
		const auto draw = g_reflectionCaptureOriginalDraw;
		if (!draw) {
			return;
		}
		if (g_reflectionCapturePending.load(std::memory_order_relaxed) == 0) {
			draw(a_context, a_vertexCount, a_startVertexLocation);
			return;
		}
		const auto drawCaller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
		draw(a_context, a_vertexCount, a_startVertexLocation);
		FinishReflectionDrawCapture(a_context, drawCaller);
	}

	void STDMETHODCALLTYPE VanillaSSLRDrawIndexedCaptureThunk(
		ID3D11DeviceContext* a_context,
		UINT a_indexCount,
		UINT a_startIndexLocation,
		INT a_baseVertexLocation)
	{
		const auto draw = g_reflectionCaptureOriginalDrawIndexed;
		if (!draw) {
			return;
		}
		if (g_reflectionCapturePending.load(std::memory_order_relaxed) == 0) {
			draw(a_context, a_indexCount, a_startIndexLocation, a_baseVertexLocation);
			return;
		}
		const auto drawCaller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
		draw(a_context, a_indexCount, a_startIndexLocation, a_baseVertexLocation);
		FinishReflectionDrawCapture(a_context, drawCaller);
	}

	void STDMETHODCALLTYPE ReflectionDrawIndexedInstancedCaptureThunk(
		ID3D11DeviceContext* a_context,
		UINT a_indexCountPerInstance,
		UINT a_instanceCount,
		UINT a_startIndexLocation,
		INT a_baseVertexLocation,
		UINT a_startInstanceLocation)
	{
		const auto draw = g_reflectionCaptureOriginalDrawIndexedInstanced;
		if (!draw) {
			return;
		}
		if (g_reflectionCapturePending.load(std::memory_order_relaxed) == 0) {
			draw(
				a_context,
				a_indexCountPerInstance,
				a_instanceCount,
				a_startIndexLocation,
				a_baseVertexLocation,
				a_startInstanceLocation);
			return;
		}
		const auto drawCaller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
		draw(
			a_context,
			a_indexCountPerInstance,
			a_instanceCount,
			a_startIndexLocation,
			a_baseVertexLocation,
			a_startInstanceLocation);
		FinishReflectionDrawCapture(a_context, drawCaller);
	}

	bool InstallReflectionDrawCaptureHook(ID3D11DeviceContext* a_context)
	{
		if ((g_reflectionCapturePending.load() &
			(kReflectionCaptureVanillaSSLR | kReflectionCaptureENBPuddleConsumer)) == 0 ||
			!a_context) {
			return g_reflectionCaptureContext != nullptr;
		}
		if (g_reflectionCaptureContext) {
			return g_reflectionCaptureContext == a_context;
		}
		auto** originalVTable = *reinterpret_cast<void***>(a_context);
		if (!IsReadableProcessRange(
				originalVTable, sizeof(void*) * kD3D11ContextVTableSize) ||
			!originalVTable[kD3D11PSSetShaderResourcesVTableIndex] ||
			!originalVTable[kD3D11DrawVTableIndex] ||
			!originalVTable[kD3D11DrawIndexedVTableIndex] ||
			!originalVTable[kD3D11DrawIndexedInstancedVTableIndex]) {
			logger::warn("[Reflection Capture] Cannot inspect the ENB D3D11 context vtable");
			return false;
		}

		auto captureVTable = std::make_unique<std::array<void*, kD3D11ContextVTableSize>>();
		std::copy_n(originalVTable, captureVTable->size(), captureVTable->begin());
		g_reflectionCaptureOriginalPSSetShaderResources =
			reinterpret_cast<D3D11PSSetShaderResources_t>(
				(*captureVTable)[kD3D11PSSetShaderResourcesVTableIndex]);
		g_reflectionCaptureOriginalDraw =
			reinterpret_cast<D3D11Draw_t>((*captureVTable)[kD3D11DrawVTableIndex]);
		g_reflectionCaptureOriginalDrawIndexed =
			reinterpret_cast<D3D11DrawIndexed_t>((*captureVTable)[kD3D11DrawIndexedVTableIndex]);
		g_reflectionCaptureOriginalDrawIndexedInstanced =
				reinterpret_cast<D3D11DrawIndexedInstanced_t>(
				(*captureVTable)[kD3D11DrawIndexedInstancedVTableIndex]);
		(*captureVTable)[kD3D11PSSetShaderResourcesVTableIndex] =
			reinterpret_cast<void*>(&ReflectionPSSetShaderResourcesCaptureThunk);
		(*captureVTable)[kD3D11DrawVTableIndex] = reinterpret_cast<void*>(&VanillaSSLRDrawCaptureThunk);
		(*captureVTable)[kD3D11DrawIndexedVTableIndex] =
			reinterpret_cast<void*>(&VanillaSSLRDrawIndexedCaptureThunk);
		(*captureVTable)[kD3D11DrawIndexedInstancedVTableIndex] =
			reinterpret_cast<void*>(&ReflectionDrawIndexedInstancedCaptureThunk);
		if (!REL::WriteSafeData(reinterpret_cast<std::uintptr_t>(a_context), captureVTable->data())) {
			g_reflectionCaptureOriginalPSSetShaderResources = nullptr;
			g_reflectionCaptureOriginalDraw = nullptr;
			g_reflectionCaptureOriginalDrawIndexed = nullptr;
			g_reflectionCaptureOriginalDrawIndexedInstanced = nullptr;
			logger::warn("[Reflection Capture] Cannot install the ENB context capture vtable");
			return false;
		}
		g_reflectionCaptureOriginalVTable = originalVTable;
		g_reflectionCaptureContext = a_context;
		g_reflectionCaptureVTable = std::move(captureVTable);
		logger::info("[Reflection Capture] Armed scoped ENB context binding/draw capture");
		return true;
	}

	class ScopedENBConstantBufferDimensions
	{
	public:
		ScopedENBConstantBufferDimensions(
			void* a_this,
			std::size_t a_bufferOffset,
			const std::array<float, 2>& a_values,
			ENBConstantBufferPatchResources& a_resources)
		{
			if (!a_this || !std::isfinite(a_values[0]) || !std::isfinite(a_values[1]) ||
				a_values[0] <= 0.0f || a_values[1] <= 0.0f) {
				return;
			}

			slot_ = reinterpret_cast<ID3D11Buffer**>(
				reinterpret_cast<std::byte*>(a_this) + a_bufferOffset);
			if (!IsWritableProcessAddress(slot_) || !*slot_ ||
				!EnsureENBConstantBufferPatchResources(a_resources, *slot_) ||
				*slot_ == a_resources.proxy.get()) {
				slot_ = nullptr;
				return;
			}

			winrt::com_ptr<ID3D11DeviceContext> context;
			a_resources.device->GetImmediateContext(context.put());
			if (!context) {
				slot_ = nullptr;
				return;
			}

			const std::array<float, 4> patchData{
				a_values[0], a_values[1], 0.0f, 0.0f
			};
			context->UpdateSubresource(
				a_resources.patch.get(), 0, nullptr, patchData.data(), 0, 0);
			context->CopyResource(a_resources.proxy.get(), *slot_);
			const D3D11_BOX sourceBox{ 0, 0, 0, sizeof(float) * 2, 1, 1 };
			context->CopySubresourceRegion(
				a_resources.proxy.get(), 0, 0, 0, 0,
				a_resources.patch.get(), 0, &sourceBox);

			original_ = *slot_;
			*slot_ = a_resources.proxy.get();
			active_ = true;
		}

		~ScopedENBConstantBufferDimensions()
		{
			if (active_) {
				*slot_ = original_;
			}
		}

		ScopedENBConstantBufferDimensions(const ScopedENBConstantBufferDimensions&) = delete;
		ScopedENBConstantBufferDimensions& operator=(const ScopedENBConstantBufferDimensions&) = delete;

		bool IsActive() const { return active_; }

	private:
		ID3D11Buffer** slot_{ nullptr };
		ID3D11Buffer* original_{ nullptr };
		bool active_{ false };
	};

	int ENBPuddleWaterRenderThunk(
		void* a_this,
		std::uint32_t a2,
		std::uint32_t a3,
		std::uint32_t a4)
	{
		const bool shouldCorrect =
			ShouldUseENBProxyCompatibility() &&
			TryGetENBBool(ENBBoolSetting::kPuddleReflections).value_or(true) &&
			g_enbPuddleWaterConstantBufferOffset.has_value() &&
			g_enbPuddleWaterTexture11Offset.has_value();

		UINT width = 0;
		UINT height = 0;
		if (shouldCorrect) {
			auto** texture11Slot = reinterpret_cast<ID3D11ShaderResourceView**>(
				reinterpret_cast<std::byte*>(a_this) + *g_enbPuddleWaterTexture11Offset);
			if (!IsReadableProcessRange(texture11Slot, sizeof(*texture11Slot)) ||
				!GetENBShaderResourceDimensions(*texture11Slot, width, height)) {
				width = 0;
				height = 0;
			}
		}

		// Fallout's BSWaterShader writes EConstantList0[0].xy (VPOSOffset) as
		// reciprocal render-target dimensions. ENB's final water shaders address
		// Texture9/Texture10 with that value, and use Texture11.GetDimensions as
		// the allocation-domain anchor in their distorted variants. Rebase only
		// VPOSOffset.xy to that bound allocation; every other constant is preserved.
		const std::array<float, 2> reciprocalTexture11Size{
			width ? 1.0f / static_cast<float>(width) : 0.0f,
			height ? 1.0f / static_cast<float>(height) : 0.0f
		};
		const ScopedENBConstantBufferDimensions vposOffset(
			width && height ? a_this : nullptr,
			g_enbPuddleWaterConstantBufferOffset.value_or(0),
			reciprocalTexture11Size,
			g_enbPuddleWaterConstantBufferPatch);
		if (shouldCorrect && !vposOffset.IsActive()) {
			static bool loggedFailure = false;
			if (!loggedFailure) {
				logger::warn("[ENB SR] Puddle/water VPOSOffset allocation-domain correction is unavailable");
				loggedFailure = true;
			}
		}
		return g_enbPuddleWaterRender(a_this, a2, a3, a4);
	}

	class ScopedENBReflectionCapture
	{
	public:
		explicit ScopedENBReflectionCapture(std::uint32_t a_technique)
		{
			if ((g_reflectionCapturePending.load() & kReflectionCaptureENBReflection) == 0 ||
				a_technique != kENBReflectionSSRTechnique) {
				return;
			}
			g_enbReflectionCaptureTechnique = a_technique;
			++g_enbReflectionCaptureScopeDepth;
			active_ = true;
		}

		~ScopedENBReflectionCapture()
		{
			if (active_) {
				--g_enbReflectionCaptureScopeDepth;
				if (g_enbReflectionCaptureScopeDepth == 0) {
					g_enbReflectionCaptureTechnique = 0;
				}
			}
		}

		ScopedENBReflectionCapture(const ScopedENBReflectionCapture&) = delete;
		ScopedENBReflectionCapture& operator=(const ScopedENBReflectionCapture&) = delete;

	private:
		bool active_ = false;
	};

	int ENBReflectionRenderThunk(
		void* a_this,
		std::uint32_t a2,
		std::uint32_t a3,
		std::uint32_t a4)
	{
		const auto technique = GetENBTechniqueHash(a_this);
		const bool reflectionTechnique = technique == kENBReflectionBlurTechnique ||
			technique == kENBReflectionSSRTechnique;
		const bool shouldCorrect =
			reflectionTechnique && ShouldUseENBProxyCompatibility() &&
			TryGetENBBool(ENBBoolSetting::kReflections).value_or(true);
		const ScopedENBReflectionCapture reflectionCapture(technique.value_or(0));
		const ScopedENBRenderDomainViewport viewport(
			a_this, shouldCorrect, kENBTexture0SRVOffset);
		const bool shouldPatchConstants =
			technique == kENBReflectionSSRTechnique && viewport.IsActive() &&
			g_enbReflectionConstantBufferOffset.has_value();
		const ScopedENBConstantBufferDimensions dimensions(
			shouldPatchConstants ? a_this : nullptr,
			g_enbReflectionConstantBufferOffset.value_or(0),
			viewport.Size(),
			g_enbReflectionConstantBufferPatch);
		if (shouldPatchConstants && !dimensions.IsActive()) {
			static bool loggedFailure = false;
			if (!loggedFailure) {
				logger::warn("[ENB SR] Reflection pixel-grid constant correction is unavailable");
				loggedFailure = true;
			}
		}
		if (shouldCorrect && !viewport.IsActive()) {
			static bool loggedViewportFailure = false;
			if (!loggedViewportFailure) {
				logger::warn("[ENB SR] Reflection render-domain viewport correction is unavailable");
				loggedViewportFailure = true;
			}
		}
		return g_enbReflectionRender(a_this, a2, a3, a4);
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

	std::uintptr_t FindENBPuddleDrawCallAddress(
		HMODULE a_module,
		const ENBCodeRange& a_code)
	{
		const auto handler = FindENBTechniqueHandler(
			a_module, a_code, "Shader_418BF7EB", "EConstantList2");
		if (!handler) {
			return 0;
		}
		const auto function = FindENBContainingFunction(a_module, handler->entry);
		if (!function) {
			return 0;
		}
		constexpr std::array<std::uint8_t, 6> kDrawIndexedCall{
			0xFF, 0x90, 0xA0, 0x00, 0x00, 0x00  // call qword ptr [rax+A0h]
		};
		for (auto cursor = function->begin; cursor + kDrawIndexedCall.size() <= function->end; ++cursor) {
			if (std::equal(
				kDrawIndexedCall.begin(), kDrawIndexedCall.end(),
				reinterpret_cast<const std::uint8_t*>(cursor))) {
				return cursor;
			}
		}
		return 0;
	}

	void STDMETHODCALLTYPE ENBPuddleDrawCaptureThunk(
		ID3D11DeviceContext* a_context,
		UINT a_indexCount,
		UINT a_instanceCount,
		UINT a_startIndexLocation,
		INT a_baseVertexLocation,
		UINT a_startInstanceLocation,
		D3D11DrawIndexedInstanced_t a_draw)
	{
		// The original indexed draw is invoked directly by ENB and therefore does
		// not pass through the context vtable hook. Install the vtable first so
		// subsequent draws on this exact context can be matched against its MRTs.
		if ((g_reflectionCapturePending.load() & kReflectionCaptureENBPuddleConsumer) != 0) {
			InstallReflectionDrawCaptureHook(a_context);
		}
		a_draw(
			a_context, a_indexCount, a_instanceCount, a_startIndexLocation, a_baseVertexLocation, a_startInstanceLocation);
		if ((g_reflectionCapturePending.load() & kReflectionCaptureENBPuddle) != 0) {
			CaptureReflectionDrawState(a_context, "enb_puddle_material");
			CompleteReflectionCapture(kReflectionCaptureENBPuddle);
		}
		ArmENBPuddleMaterialConsumerTrace(a_context);
	}

	bool InstallENBPuddleDrawCaptureHook(std::uintptr_t a_drawCall)
	{
		constexpr std::array<std::uint8_t, 6> kExpected{
			0xFF, 0x90, 0xA0, 0x00, 0x00, 0x00
		};
		if (!a_drawCall || !std::equal(
				kExpected.begin(), kExpected.end(),
				reinterpret_cast<const std::uint8_t*>(a_drawCall))) {
			logger::warn("[Reflection Capture] ENB puddle DrawIndexedInstanced call bytes do not match");
			return false;
		}
		if (g_enbPuddleCaptureTrampoline.empty()) {
			g_enbPuddleCaptureTrampoline.create(
				kENBPuddleCaptureTrampolineSize, reinterpret_cast<void*>(a_drawCall));
		}
		auto* stub = static_cast<std::uint8_t*>(g_enbPuddleCaptureTrampoline.allocate(53));
		if (!stub) {
			logger::warn("[Reflection Capture] Cannot allocate ENB puddle capture trampoline");
			return false;
		}
		const std::array<std::uint8_t, 53> stubTemplate{
			0x48, 0x8B, 0x80, 0xA0, 0x00, 0x00, 0x00,             // mov rax,[rax+A0h]
			0x48, 0x83, 0xEC, 0x38,                               // sub rsp,38h
			0x48, 0x89, 0x44, 0x24, 0x30,                         // mov [rsp+30h],rax
			0x48, 0x8B, 0x44, 0x24, 0x60,                         // mov rax,[rsp+60h]
			0x48, 0x89, 0x44, 0x24, 0x20,                         // mov [rsp+20h],rax
			0x48, 0x8B, 0x44, 0x24, 0x68,                         // mov rax,[rsp+68h]
			0x48, 0x89, 0x44, 0x24, 0x28,                         // mov [rsp+28h],rax
			0x48, 0xB8,                                           // mov rax,imm64
			0, 0, 0, 0, 0, 0, 0, 0,
			0xFF, 0xD0,                                           // call rax
			0x48, 0x83, 0xC4, 0x38,                               // add rsp,38h
			0xC3                                                  // ret
		};
		std::memcpy(stub, stubTemplate.data(), stubTemplate.size());
		const auto helper = reinterpret_cast<std::uintptr_t>(&ENBPuddleDrawCaptureThunk);
		std::memcpy(stub + 38, &helper, sizeof(helper));
		const auto displacement64 =
			reinterpret_cast<std::int64_t>(stub) - static_cast<std::int64_t>(a_drawCall + 5);
		if (displacement64 < std::numeric_limits<std::int32_t>::min() ||
			displacement64 > std::numeric_limits<std::int32_t>::max()) {
			logger::warn("[Reflection Capture] ENB puddle trampoline is outside rel32 range");
			return false;
		}
		std::array<std::uint8_t, 6> patch{ 0xE8, 0, 0, 0, 0, 0x90 };
		const auto displacement = static_cast<std::int32_t>(displacement64);
		std::memcpy(patch.data() + 1, &displacement, sizeof(displacement));
		if (!REL::WriteSafeData(a_drawCall, patch)) {
			logger::warn("[Reflection Capture] Cannot patch ENB puddle DrawIndexedInstanced call site");
			return false;
		}
		logger::info("[Reflection Capture] ENB puddle material draw hook at RVA 0x{:X}",
			a_drawCall - reinterpret_cast<std::uintptr_t>(FindENBModule()));
		return true;
	}

	void UpdateReflectionCaptureRequest(bool a_enabled)
	{
		if (!a_enabled) {
			g_reflectionCapturePending.store(0);
			ResetReflectionCaptureTrace();
			return;
		}
		if (g_reflectionCapturePending.load() != 0) {
			return;
		}
		g_activeReflectionCaptureSerial.store(g_reflectionCaptureSerial.fetch_add(1) + 1);
		const auto passes = kReflectionCaptureVanillaSSLR |
			(enbLoaded ? (kReflectionCaptureENBPuddle | kReflectionCaptureENBReflection |
				kReflectionCaptureENBPuddleConsumer) : 0u);
		g_reflectionCapturePending.store(passes);
		logger::info(
			"[Reflection Capture] Armed capture {}. Vanilla SSLR{} draw bindings will be written as DDS.",
			g_activeReflectionCaptureSerial.load(),
			enbLoaded ? " plus ENB puddle material, its unique resource-identity consumers, and SSR reflection" : "");
		if (enbLoaded) {
			InstallENBScreenEffectRenderHooks();
		}
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

	bool CopyENBInputToRenderProxy(int a_target)
	{
		auto* rendererData = RE::BSGraphics::GetRendererData();
		return rendererData && CopyENBInputToRenderProxy(
			Upscaling::GetSingleton(),
			reinterpret_cast<ID3D11DeviceContext*>(rendererData->context),
			a_target);
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

	class ScopedENBDepthOfFieldDepth
	{
	public:
		ScopedENBDepthOfFieldDepth()
		{
			auto* upscaling = Upscaling::GetSingleton();
			if (g_enbNativeImageSpaceParamScopeDepth <= 0 ||
				(originalDynamicWidthRatio == 1.0f && originalDynamicHeightRatio == 1.0f) ||
				!upscaling || !IsENBSRCompatibilityActive(upscaling->upscaleMethod) ||
				!g_enbDepthTextureSRVSlot || !g_enbNativeDepthSRV) {
				return;
			}

			original_ = *g_enbDepthTextureSRVSlot;
			*g_enbDepthTextureSRVSlot = g_enbNativeDepthSRV.get();
			active_ = true;
		}

		~ScopedENBDepthOfFieldDepth()
		{
			if (active_) {
				*g_enbDepthTextureSRVSlot = original_;
			}
		}

		ScopedENBDepthOfFieldDepth(const ScopedENBDepthOfFieldDepth&) = delete;
		ScopedENBDepthOfFieldDepth& operator=(const ScopedENBDepthOfFieldDepth&) = delete;

	private:
		ID3D11ShaderResourceView* original_ = nullptr;
		bool active_ = false;
	};

	int ENBDepthOfFieldRenderThunk(void* a_this, std::uint32_t a2, std::uint32_t a3, std::uint32_t a4)
	{
		const ScopedENBDepthOfFieldDepth depthBridge;
		return g_enbDepthOfFieldRender(a_this, a2, a3, a4);
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
		const auto puddleWaterHandler = FindENBPuddleWaterHandler(module, *code);
		if (!g_enbPuddleWaterDrawReturnAddress) {
			const auto drawCall = FindENBPuddleWaterDrawCallAddress(module, *code);
			if (drawCall) {
				g_enbPuddleWaterDrawReturnAddress = drawCall + 3;
				logger::info(
					"[Reflection Capture] ENB primary puddle/water pass draw at RVA 0x{:X}",
					drawCall - reinterpret_cast<std::uintptr_t>(module));
			}
		}

		if (!g_enbPuddleWaterRender) {
			if (!puddleWaterHandler) {
				logger::warn("[ENB SR] Cannot resolve the puddle/water VPOSOffset handler");
			} else {
				const auto trampoline = Detours::X64::DetourFunction(
					puddleWaterHandler->entry,
					reinterpret_cast<std::uintptr_t>(&ENBPuddleWaterRenderThunk));
				if (trampoline) {
					g_enbPuddleWaterConstantBufferOffset =
						puddleWaterHandler->constantBufferOffset;
					g_enbPuddleWaterTexture11Offset = puddleWaterHandler->texture11Offset;
					g_enbPuddleWaterRender =
						reinterpret_cast<ENBScreenEffectRender_t>(trampoline);
					logger::info(
						"[ENB SR] Puddle/water handler RVA 0x{:X}, EConstantList0 +0x{:X}, Texture11 +0x{:X}",
						puddleWaterHandler->entry - reinterpret_cast<std::uintptr_t>(module),
						puddleWaterHandler->constantBufferOffset,
						puddleWaterHandler->texture11Offset);
				} else {
					logger::warn("[ENB SR] Failed to install the puddle/water VPOSOffset hook");
				}
			}
		}

		if (!g_enbReflectionRender) {
			const auto handler = FindENBTechniqueHandler(
				module,
				*code,
				"Shader_1355B81A",
				"EConstantList0",
				"Shader_B1B77A4D");
			if (!handler) {
				logger::warn("[ENB SR] Cannot resolve the reflection constant-buffer handler");
			} else {
				const auto trampoline = Detours::X64::DetourFunction(
					handler->entry,
					reinterpret_cast<std::uintptr_t>(&ENBReflectionRenderThunk));
				if (trampoline) {
					g_enbReflectionConstantBufferOffset = handler->constantBufferOffset;
					g_enbReflectionRender = reinterpret_cast<ENBScreenEffectRender_t>(trampoline);
					logger::info(
						"[ENB SR] Reflection handler RVA 0x{:X}, constant-buffer slot +0x{:X}",
						handler->entry - reinterpret_cast<std::uintptr_t>(module),
						handler->constantBufferOffset);
				} else {
					logger::warn("[ENB SR] Failed to install the reflection constant-buffer hook");
				}
			}
		}

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

		if (!g_enbPuddleCaptureInstalled &&
			(g_reflectionCapturePending.load() & kReflectionCaptureENBPuddle) != 0) {
			const auto puddleDrawCall = FindENBPuddleDrawCallAddress(module, *code);
			if (!puddleDrawCall) {
				logger::warn("[Reflection Capture] Cannot resolve ENB puddle material DrawIndexedInstanced handoff");
			} else if (InstallENBPuddleDrawCaptureHook(puddleDrawCall)) {
				g_enbPuddleCaptureInstalled = true;
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

		if (!g_enbDepthOfFieldRender && depthOfFieldSetting) {
			const auto depthOfFieldEntry = FindENBDepthOfFieldRenderEntry(*code, depthOfFieldSetting);
			const auto textureDepthName = FindENBModuleString(module, "TextureDepth");
			const auto depthTextureSRVSlot =
				FindENBDepthTextureSRVSlot(depthOfFieldEntry, textureDepthName);
			if (!depthOfFieldEntry || !depthTextureSRVSlot) {
				logger::warn("[ENB SR] Cannot resolve the DepthOfField depth bridge");
			} else {
				g_enbDepthTextureSRVSlot = depthTextureSRVSlot;
				const auto trampoline = Detours::X64::DetourFunction(
					depthOfFieldEntry,
					reinterpret_cast<std::uintptr_t>(&ENBDepthOfFieldRenderThunk));
				if (trampoline) {
					g_enbDepthOfFieldRender = reinterpret_cast<ENBScreenEffectRender_t>(trampoline);
				} else {
					g_enbDepthTextureSRVSlot = nullptr;
					logger::warn("[ENB SR] Failed to install DepthOfField depth bridge hook");
				}
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

		const float clearColor[4]{};
		workContext->ClearUnorderedAccessViewFloat(a_upscalingInput->uav.get(), clearColor);
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
			D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc{};
			D3D11_RENDER_TARGET_VIEW_DESC destinationViewDesc{};
			sourceSRV->GetDesc(&sourceViewDesc);
			destinationRTV->GetDesc(&destinationViewDesc);
			job.sourceViewFormat = sourceViewDesc.Format;
			job.destinationViewFormat = destinationViewDesc.Format;
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

		std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> boundOMUAVs{};
		g_enbNativeContext->OMGetRenderTargetsAndUnorderedAccessViews(
			0,
			nullptr,
			nullptr,
			0,
			static_cast<UINT>(boundOMUAVs.size()),
			boundOMUAVs.data());
		const bool hasBoundOMUAV = std::ranges::any_of(boundOMUAVs, [](const auto* a_view) {
			return a_view != nullptr;
		});
		for (auto* view : boundOMUAVs) {
			if (view) {
				view->Release();
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
					jobs[j].destinationDesc.SampleDesc.Quality != jobs[i].destinationDesc.SampleDesc.Quality ||
					jobs[j].sourceViewFormat != jobs[i].sourceViewFormat ||
					jobs[j].destinationViewFormat != jobs[i].destinationViewFormat) {
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
		const auto originalFrameBufferViewport = gameViewport->frameBufferViewport;

		// Disable removal of jitter in some passes
		if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDisabled){
			gameViewport->offsetX = originalOffsetX;
			gameViewport->offsetY = originalOffsetY;
		}

		originalDynamicHeightRatio = renderTargetManager->dynamicHeightRatio;
		originalDynamicWidthRatio = renderTargetManager->dynamicWidthRatio;
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
		static auto gameViewport = Util::State_GetSingleton();
		const auto originalFrameBufferViewport = gameViewport->frameBufferViewport;

		originalDynamicHeightRatio = renderTargetManager->dynamicHeightRatio;
		originalDynamicWidthRatio = renderTargetManager->dynamicWidthRatio;
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
			(renderTargetManager->dynamicHeightRatio != 1.0f || renderTargetManager->dynamicWidthRatio != 1.0f)) {
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
		const bool nativeENBScope = g_enbNativeImageSpaceParamScopeDepth > 0;
		const bool isHDRFinalComposite = IsENBHDRFinalCompositeEffect(This);
		const bool isRefractionComposite = This && This == g_enbRefractionCompositeEffect;
		const bool isNativeEffectShader = IsENBNativeImageSpaceShader(This);
		if (!nativeENBScope || (!isHDRFinalComposite && !isRefractionComposite && !isNativeEffectShader)) {
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

/** @brief Preserve ENB feature-composite passes in their established render domain. */
struct DrawWorld_DeferredComposite_RenderPassImmediately
{
	static void thunk(RE::BSRenderPass* This, uint a2, bool a3)
	{
		auto upscaling = Upscaling::GetSingleton();
		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		bool requiresOverride = renderTargetManager->dynamicHeightRatio != 1.0 || renderTargetManager->dynamicWidthRatio != 1.0;

		originalDynamicHeightRatio = renderTargetManager->dynamicHeightRatio;
		originalDynamicWidthRatio = renderTargetManager->dynamicWidthRatio;
		const bool enbCompatibilityActive = IsENBSRCompatibilityActive(upscaling->upscaleMethod);

		if (ShouldBypassDynamicResolutionHooksForInactiveENB()) {
			func(This, a2, a3);
			return;
		}
		if (requiresOverride &&
			ShouldLeaveENBDeferredCompositeFeaturePassNative(This, upscaling->upscaleMethod)) {
			func(This, a2, a3);
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
		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
		const bool enbCompatibilityActive =
			(renderTargetManager->dynamicHeightRatio != 1.0f || renderTargetManager->dynamicWidthRatio != 1.0f) &&
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

/** @brief Hook the SSLR projection producer. */
struct BSImagespaceShaderSSLRPrepass_SetupTechnique_BeginTechnique
{
	static bool thunk(RE::BSShader* This, uint a2, uint a3, uint a4, uint a5)
	{
		const bool result = func(This, a2, a3, a4, a5);
		if (result) {
			Upscaling::GetSingleton()->PatchSSRPrepassShader();
		}
		return result;
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
		stl::detour_thunk<ImageSpaceManager_RenderEffect>(REL::ID{ 325252, 2316597 });
		stl::detour_thunk<BSImagespaceShader_Render_ENBFinalComposite>(REL::ID{ 1388477, 2319297 });
	}
	// Fix dynamic resolution for Lens Flare visibility
	stl::detour_thunk<BSImagespaceShaderLensFlare_RenderLensFlare>(REL::ID{ 676108, 2317547 });

	// Fix dynamic resolution for Screenspace Reflections
	stl::write_thunk_call<BSImagespaceShaderSSLRPrepass_SetupTechnique_BeginTechnique>(
		REL::ID{ 395020, 2317301 }.address() + 0x1C);
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
	if (enbLoaded) {
		ResolveENBRenderResolutionGlobals();
		InstallENBScreenEffectRenderHooks();
		ResolveENBPrepassResourceSlots();
	}
}

void Upscaling::InstallHighFPSPhysicsFixCompatibility()
{
	static bool installed = false;
	if (installed || !REX::FModule::IsRuntimeOG()) {
		return;
	}

	const auto displayLoadingScreen = REL::ID{ 132841 }.address();
	if (!HasHFPFDisableLoadingAnimationPatch(displayLoadingScreen)) {
		return;
	}

	stl::detour_thunk<JobListManager_ServingThread_DisplayLoadingScreen>(REL::ID{ 132841 });
	installed = true;
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
	const bool previousImageSpaceEffectLog = settings.imageSpaceEffectLog != 0;
	const bool previousReflectionCapture = settings.reflectionCapture != 0;

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
	settings.osdMode = static_cast<uint>(std::clamp<long>(ini.GetLongValue("Settings", "iOnScreenDisplay", 0), 0, 2));
	settings.taggedTextureDebug = static_cast<uint>(ini.GetLongValue("Settings", "bTaggedTextureDebug", 0) == 1);
	settings.imageSpaceEffectLog = static_cast<uint>(ini.GetLongValue("Settings", "bImageSpaceEffectLog", 0) == 1);
	settings.reflectionCapture = static_cast<uint>(ini.GetLongValue("Settings", "bReflectionCapture", 0) == 1);
	if (!previousImageSpaceEffectLog && settings.imageSpaceEffectLog != 0) {
		ResetENBNativeImageSpaceEffectLog();
		logger::info("[ENB IS Log] Enabled; waiting for active effects in the ENB native scope");
	}
	if (previousReflectionCapture != (settings.reflectionCapture != 0)) {
		UpdateReflectionCaptureRequest(settings.reflectionCapture != 0);
	}
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

	// Reload settings after MCM writes, without doing file parse work on
	// every normal ESC close.
	if (a_event.menuName == "PauseMenu") {
		if (!a_event.opening && SettingsFileChangedSinceLastLoad()) {
			singleton->LoadSettings();
		}
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

void Upscaling::OverrideRenderTargetsSelective(std::initializer_list<int> a_targetIndices, std::initializer_list<int> a_indicesToCopy)
{
	for (const auto targetIndex : a_targetIndices) {
		const bool shouldCopy = std::find(a_indicesToCopy.begin(), a_indicesToCopy.end(), targetIndex) != a_indicesToCopy.end();
		OverrideRenderTarget(targetIndex, shouldCopy);
	}

	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
	for (const auto targetIndex : a_targetIndices) {
		originalRenderTargetData[targetIndex] = renderTargetManager->renderTargetData[targetIndex];
		renderTargetManager->renderTargetData[targetIndex].width = static_cast<uint>(static_cast<float>(renderTargetManager->renderTargetData[targetIndex].width) * renderTargetManager->dynamicWidthRatio);
		renderTargetManager->renderTargetData[targetIndex].height = static_cast<uint>(static_cast<float>(renderTargetManager->renderTargetData[targetIndex].height) * renderTargetManager->dynamicHeightRatio);
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
	if (const auto ui = RE::UI::GetSingleton()) {
		if (ui->menuMode > 0 || ui->freezeFramePause > 0) {
			return true;
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

	static auto gameViewport = Util::State_GetSingleton();
	const auto aspectRatio = gameViewport && gameViewport->screenHeight > 0 ?
		static_cast<float>(gameViewport->screenWidth) / static_cast<float>(gameViewport->screenHeight) :
		1.0f;
	const auto* cameraState = Util::GetWorldCameraStateData();
	const auto cameraProjection = Util::GetCameraProjection(aspectRatio);
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

ID3D11PixelShader* Upscaling::GetBSImagespaceShaderSSLRPrepass()
{
	if (!BSImagespaceShaderSSLRPrepass) {
		logger::debug("Compiling BSImagespaceShaderSSLRPrepass.hlsl");
		BSImagespaceShaderSSLRPrepass.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/F4SE/Plugins/Upscaling/BSImagespaceShaderSSLRPrepass.hlsl", {}, "ps_5_0"));
	}
	return BSImagespaceShaderSSLRPrepass.get();
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
		!IsFeatureRequestBlocked(FeatureRequest::kDLSSG) &&
		(!menuBlocksTemporal || dlssgHeldThroughMenu) &&
		(dlssgHeldThroughMenu || dlssgMenuResumeReady);
	fsrFrameGenerationActive =
		static_cast<UpscaleMethod>(settings.upscaleMethodPreference) != UpscaleMethod::kDisabled &&
		frameGenerationSettingEnabled &&
		dx12Ready &&
		(kForceFSRFrameGenerationForTesting || !streamline->featureDLSSG) &&
		!IsFeatureRequestBlocked(FeatureRequest::kFSRFrameGeneration) &&
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

	// Freeze-frame pause needs native render targets; overlay/dialogue menu flags alone are not enough to suspend SR.
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
			const auto usePresentOverride = getD3D12FSROutput() != nullptr;
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
		const auto usePresentOverride = hasPresentOverrideOutput;
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

		bool useDLSSGThisFrame = useFrameGeneration;
		if (useDLSSGThisFrame) {
			streamline->UpdateReflex(settings.reflexMode, true);
			if (!streamline->UpdateConstants(jitter, true)) {
				ReportFeatureRequestFailure(FeatureRequest::kDLSSG, "Streamline full common constants");
				useDLSSGThisFrame = false;
			}
		}
		if (useD3D12DLSS && !useDLSSGThisFrame) {
			streamline->UpdateReflex(settings.reflexMode, false);
			if (!streamline->UpdateConstants(jitter, false)) {
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
			if (!streamline->UpdateDLSSG(true, settings.frameGenerationMode, settings.dlssgGeneratedFrames + 1, settings.dynamicMFGEnabled != 0, settings.dynamicMFGTargetFPS, dlssgInputSize, a_displaySize, frameBufferDesc.Format, sharedMotionVectorDesc.Format, sharedDepthDesc.Format, uiFormat)) {
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
	if (!streamline->UpdateConstants(jitter, true)) {
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
	auto* uiColorAlpha = dlssgUIColorAlphaD3D12[a_frameIndex].get();
	if (!color || !motionVectors || !depth || !a_commandList) {
		fsrFrameGenerationInputsReady[a_frameIndex] = false;
		ReportFeatureRequestFailure(FeatureRequest::kFSRFrameGeneration, "FSR frame generation inputs");
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

void Upscaling::PatchSSRPrepassShader()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();
	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
	const float widthScale = renderTargetManager->dynamicWidthRatio;
	const float heightScale = renderTargetManager->dynamicHeightRatio;
	auto buffer = GetSSLRCoordinateBuffer(widthScale, heightScale);
	if (widthScale == 1.0f && heightScale == 1.0f) {
		return;
	}

	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	context->PSSetConstantBuffers(13, 1, &buffer);
	context->PSSetShader(GetBSImagespaceShaderSSLRPrepass(), nullptr, 0);
}

void Upscaling::PatchSSRShader()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();
	static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	LogSSLRBindings(
		context,
		renderTargetManager->dynamicWidthRatio,
		renderTargetManager->dynamicHeightRatio);

	// The ray equation and depth-mip DDA stay in allocation UV. The scale
	// supplies the active allocation bounds and converts confidence distances
	// back to logical UV without perturbing integer mip-cell traversal.
	auto buffer = GetSSLRCoordinateBuffer(
		renderTargetManager->dynamicWidthRatio,
		renderTargetManager->dynamicHeightRatio);
	context->PSSetConstantBuffers(13, 1, &buffer);

	// Replace the game's SSR pixel shader with our custom one that fixes scaled render targets
	context->PSSetShader(GetBSImagespaceShaderSSLRRaytracing(), nullptr, 0);
}
