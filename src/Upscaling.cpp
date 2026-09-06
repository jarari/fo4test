#include "Upscaling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <Psapi.h>
#include <SimpleIni.h>
#include <utility>
#include <vector>

#include "DX12SwapChain.h"
#include "ENBRenderDomain.h"
#include "NativeInterfaceUI.h"
#include "Screenshot.h"

extern bool enbLoaded;

static RE::BSGraphics::Renderer* g_nativeUIRenderer = nullptr;
static thread_local bool g_nativeScreenSpaceUI = false;
static uint32_t g_upscalingUpdateFrame = UINT32_MAX;
static thread_local uint32_t g_simulationFrame = 0;
static thread_local bool g_simulationOpen = false;

namespace
{
	bool EngineOwnsFrozenBackground()
	{
		// Main::DrawWorld_And_UI saves and disables TAA before capturing RT1
		// into RT15. This stays set while Render_PreUI is skipped, including
		// engine-forced freezes that have no menu on the stack.
		static REL::Relocation<const bool*> taaSaved{ REL::ID{ 1256383, 2698092 } };
		const auto* ui = RE::UI::GetSingleton();
		return *taaSaved || (ui && ui->freezeFramePause != 0);
	}
}

struct Main_Run_OnIdle
{
	static void thunk(RE::Main* a_main)
	{
		const auto nextFrame = Util::State_GetSingleton()->frameCount + 1;
		g_simulationFrame = nextFrame;
		DX12SwapChain::GetSingleton()->WaitForFrameStart();
		g_simulationOpen = Streamline::GetSingleton()->BeginSimulationFrame(nextFrame);
		func(a_main);
		if (g_simulationOpen) {
			// Alt-Tab can skip Swap after OnIdle has already begun.
			Streamline::GetSingleton()->EndSimulationFrame(g_simulationFrame);
			g_simulationOpen = false;
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct Main_OnIdle_Swap
{
	static void thunk(RE::Main* a_main)
	{
		// OnIdle has waited for OkToRender (stage 0). UI jobs and
		// post-render housekeeping can still overlap rendering.
		if (g_simulationOpen) {
			Streamline::GetSingleton()->EndSimulationFrame(g_simulationFrame);
			g_simulationOpen = false;
		}
		func(a_main);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct Scaleform_SetNativeScreenTarget
{
	static void thunk(void* a_renderer)
	{
		if (g_nativeScreenSpaceUI && g_nativeUIRenderer) {
			auto* manager = Util::RenderTargetManager_GetSingleton();
			using SetTarget = void (*)(RE::BSGraphics::RenderTargetManager*, int, int, RE::BSGraphics::SetRenderTargetMode);
			static REL::Relocation<SetTarget> setTarget{ REL::ID{ 1502425, 2277188 } };
			using Viewport = void (*)(RE::BSGraphics::RenderTargetManager*);
			static REL::Relocation<Viewport> forceViewport{ REL::ID{ 1208720, 2277193 } };
			using Flush = void (*)(RE::BSGraphics::Renderer*);
			static REL::Relocation<Flush> flush{ REL::ID{ 952687, 2276835 } };
			setTarget(manager, 0, 0, RE::BSGraphics::SetRenderTargetMode::kNoClear);
			forceViewport(manager);
			flush(g_nativeUIRenderer);
		}
		if (g_nativeScreenSpaceUI || NativeInterfaceUI::IsRendering()) {
			// Scaleform caches a surface per platform render-target ID. RT0 was
			// cached while it still referred to the scene-sized proxy, so rebuild
			// that cache only when the native UI allocation changes.
			static uint64_t cachedGeneration = 0;
			const auto generation = DX12SwapChain::GetSingleton()->NativeUIGeneration();
			if (cachedGeneration != generation) {
				using ClearTargets = void (*)(void*);
				static REL::Relocation<ClearTargets> clearTargets{ REL::ID{ 989116, 2284945 } };
				clearTargets(a_renderer);
				cachedGeneration = generation;
				logger::info("[ENB UI] Rebuilt Scaleform render-target cache for native generation {}", generation);
			}
		}
		func(a_renderer);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct Renderer_Begin_ENBDomains
{
	static void thunk(RE::BSGraphics::Renderer* a_renderer, uint32_t a_window)
	{
		if (a_window == 0) {
			// OG and AE increment State::frameCount inside Begin. Acquire that
			// upcoming token before rendering; late constants reuse it without Sleep.
			Streamline::GetSingleton()->BeginRenderFrame(Util::State_GetSingleton()->frameCount + 1);
		}
		g_nativeUIRenderer = a_renderer;
		auto* swap = DX12SwapChain::GetSingleton();
		swap->EndNativeUI();
		if (a_window == 0 && ENBRenderDomain::Get().Active() && swap->IsReady() && !swap->IsWindowUnavailable()) {
			auto& domain = ENBRenderDomain::Get();
			const auto& settings = Upscaling::GetSingleton()->settings;
			const auto requested = settings.upscaleMethodPreference == 0 ? 0u : std::min(settings.qualityMode, 4u);
			domain.qualityChangePending = requested != domain.Quality();
			if (Util::CaptureInitialRenderTargetBindings() && domain.qualityChangePending &&
				!RE::BSGraphics::GetRendererData()->requestWindowSizeChange) {
				auto* sl = Streamline::GetSingleton();
				if (sl->dlssgActive) {
					sl->RequestDLSSGDisable();
				} else if (!sl->NeedsDLSSGPresentSafety() && !FidelityFX::GetSingleton()->IsFrameGenerationEnabled()) {
					static uint32_t nextAttempt = 0;
					const auto frame = Util::State_GetSingleton()->frameCount;
					if (frame >= nextAttempt) {
						const auto result = swap->ResizeENBScene(requested);
						if (SUCCEEDED(result)) {
							domain.qualityChangePending = false;
							nextAttempt = 0;
						} else {
							nextAttempt = frame + 120;
							logger::error("[ENB scene resize] Quality {} not applied hr=0x{:08X}; retaining active quality {}", requested,
								static_cast<uint32_t>(result), domain.Quality());
						}
					}
				}
			}
		}
		func(a_renderer, a_window);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct UI_ScreenSpace_RenderMenus_Native
{
	static void thunk(void* a_ui)
	{
		struct RestoreScreenSpaceFlag
		{
			bool previous = g_nativeScreenSpaceUI;
			~RestoreScreenSpaceFlag() { g_nativeScreenSpaceUI = previous; }
		} restore;
		auto& domain = ENBRenderDomain::Get();
		auto* swap = DX12SwapChain::GetSingleton();
		g_nativeScreenSpaceUI = domain.Active() && swap->BeginNativeUI();
		func(a_ui);
		swap->PublishNativeUIForOverlays();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Keep Begin's framebuffer metadata in the actual D3D11 allocation domain. */
struct Renderer_Begin_SetFrameBufferProperties
{
	static void thunk(RE::BSGraphics::RenderTargetManager* a_manager,
		const RE::BSGraphics::RenderTargetProperties& a_properties)
	{
		const auto& domain = ENBRenderDomain::Get();
		if (domain.Active()) {
			// Begin has already selected this window's RT0 views, but constructs
			// these properties from GetClientRect (D), not GetBuffer (R).
			// Inspect RT0 rather than changing the HWND or all logical RT sizes.
			const auto* renderer = RE::BSGraphics::GetRendererData();
			if (renderer && renderer->renderTargets[0].srView) {
				winrt::com_ptr<ID3D11Resource> resource;
				reinterpret_cast<ID3D11ShaderResourceView*>(renderer->renderTargets[0].srView)->GetResource(resource.put());
				const auto texture = resource.try_as<ID3D11Texture2D>();
				if (texture) {
					D3D11_TEXTURE2D_DESC desc{};
					texture->GetDesc(&desc);
					// Leave other renderer windows on their original path.
					if (desc.Width == domain.Width() && desc.Height == domain.Height()) {
						auto properties = a_properties;
						properties.width = desc.Width;
						properties.height = desc.Height;
						func(a_manager, properties);
						static bool logged = false;
						if (!logged) {
							logger::info("[ENB domain] Renderer::Begin framebuffer metadata {}x{} -> {}x{} (actual RT0); HWND/display unchanged",
								a_properties.width, a_properties.height, properties.width, properties.height);
							logged = true;
						}
						return;
					}
				}
			}
		}
		func(a_manager, a_properties);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

/** @brief Hook for updating jitter, dynamic resolution, and resources */
struct BSGraphics_State_UpdateDynamicResolution
{
	static void thunk(RE::BSGraphics::RenderTargetManager* This,
		RE::NiPoint3* a2,
		RE::NiPoint3* a3,
		RE::NiPoint3* a4,
		RE::NiPoint3* a5)
	{
		ENBRenderDomain::Get().ApplySceneDimensions();
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
		if (EngineOwnsFrozenBackground()) {
			return false;
		}
		const auto method = Upscaling::GetSingleton()->upscaleMethod;
		return (method == Upscaling::UpscaleMethod::kDisabled ||
			(ENBRenderDomain::Get().Active() && method == Upscaling::UpscaleMethod::kSpatialFallback)) && func(This);
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
	FILETIME g_lastSettingsWriteTime{};
	bool g_hasLastSettingsWriteTime = false;
	bool g_lastSettingsFileExists = false;
	bool g_textureMemoryReserveApplied = false;
	constexpr std::size_t kHFPFAELoadingLoopTrampolineSize = 64;
	REL::Trampoline g_hfpfAELoadingLoopTrampoline{ "HFPF AE Loading Loop"sv };
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
		if (ENBRenderDomain::Get().Active()) {
			return false;
		}
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
		return ENBRenderDomain::Get().Active() ? ENBRenderDomain::Get().Quality() : a_qualityMode;
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
		Upscaling* a_upscaling,
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

		a_upscaling->RetireSharedD3D12Texture(a_texture, a_d3d12Resource);
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

		if (requiresOverride) {
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

		const auto ratios = GetDynamicResolutionRatios();
		originalDynamicHeightRatio = ratios.height;
		originalDynamicWidthRatio = ratios.width;
		const auto frameDynamicHeightRatio = originalDynamicHeightRatio;
		const auto frameDynamicWidthRatio = originalDynamicWidthRatio;

		if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDisabled &&
			IsDynamicResolutionScaled()) {
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
		if (ENBRenderDomain::Get().Active()) {
			// Both scene and late D3D11 UI stay in the same allocation domain.
			// Do not apply the legacy post-SR viewport override: it bypasses the
			// engine's cached viewport and is only needed by the split-size path.
			return;
		}

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

/** @brief Match deferred-composite targets and depth to the non-ENB DRS viewport. */
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
		if (result && !ENBRenderDomain::Get().Active()) {
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
		// Frozen backgrounds restore RT15 without Render_PreUI, so its
		// UpdateDynamicResolution hook does not run. Do not reuse gameplay's
		// temporal requests/jitter throughout a frozen menu.
		if (g_upscalingUpdateFrame != Util::State_GetSingleton()->frameCount) {
			Upscaling::GetSingleton()->UpdateUpscaling();
		}
		func(This);

		static auto renderTargetManager = Util::RenderTargetManager_GetSingleton();

		SetDynamicResolutionRatio(renderTargetManager, originalDynamicWidthRatio, originalDynamicHeightRatio);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

namespace
{
	using ENBOverlay_t = void (*)(void*);
	ENBOverlay_t g_nativeENBOverlay = nullptr;
	using TwWindowSize_t = int (*)(int, int);
	TwWindowSize_t g_nativeTwWindowSize = nullptr;
	uint32_t* g_enbOverlayDimensions = nullptr;

	void NativeENBOverlay(void* a_this)
	{
		auto* swap = DX12SwapChain::GetSingleton();
		if (!ENBRenderDomain::Get().Active() || !swap->BeginNativeUI()) {
			g_nativeENBOverlay(a_this);
			return;
		}
		struct RestoreOverlayDimensions
		{
			uint32_t width = g_enbOverlayDimensions[0];
			uint32_t height = g_enbOverlayDimensions[1];
			~RestoreOverlayDimensions()
			{
				g_enbOverlayDimensions[0] = width;
				g_enbOverlayDimensions[1] = height;
			}
		} restore;
		g_enbOverlayDimensions[0] = swap->swapChainDesc.Width;
		g_enbOverlayDimensions[1] = swap->swapChainDesc.Height;
		// ENB's scene-only ResizeBuffers resets AntTweakBar dimensions without
		// recreating nativeUITexture. Track both lifetimes so GUI clipping and
		// input coordinates return to display size after each resize transaction.
		// TwWindowSize recreates font/GUI objects: never call it every Present.
		static uint64_t configuredGeneration = 0;
		static uint64_t configuredSceneResizeGeneration = 0;
		if ((configuredGeneration != swap->NativeUIGeneration() ||
			configuredSceneResizeGeneration != swap->ENBSceneResizeGeneration()) &&
			g_nativeTwWindowSize(static_cast<int>(swap->swapChainDesc.Width), static_cast<int>(swap->swapChainDesc.Height))) {
			configuredGeneration = swap->NativeUIGeneration();
			configuredSceneResizeGeneration = swap->ENBSceneResizeGeneration();
			logger::info("[ENB UI] Restored overlay coordinates {}x{}; native-generation={} scene-resize-generation={}",
				swap->swapChainDesc.Width, swap->swapChainDesc.Height, configuredGeneration, configuredSceneResizeGeneration);
		}
		g_nativeENBOverlay(a_this);
		// Only Present-time overlays see D; ENB effects/allocations still see R.
	}

	void InstallNativeENBOverlay()
	{
		const auto module = FindENBModule();
		if (!module || g_nativeENBOverlay) { return; }
		const auto base = reinterpret_cast<std::uintptr_t>(module);
		const auto create = reinterpret_cast<std::uintptr_t>(GetProcAddress(module, "D3D11CreateDeviceAndSwapChain"));
		struct Layout { std::uintptr_t create, overlay, dimensions; };
		constexpr Layout layouts[]{
			{ 0x2D220, 0x22E30, 0x1A6EA0 }, // ENB 0.501
			{ 0x275C0, 0x1D7E0, 0x15BD70 }, // ENB 0.496
			{ 0x20530, 0x17110, 0x14CA60 }  // ENB 0.420
		};
		constexpr std::array<std::uint8_t, 8> prefix{ 0x40, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x70 };
		for (const auto& layout : layouts) {
			if (create != base + layout.create ||
				std::memcmp(reinterpret_cast<void*>(base + layout.overlay), prefix.data(), prefix.size()) != 0) { continue; }
			g_nativeTwWindowSize = reinterpret_cast<TwWindowSize_t>(GetProcAddress(module, "TwWindowSize"));
			if (!g_nativeTwWindowSize) { break; }
			g_enbOverlayDimensions = reinterpret_cast<uint32_t*>(base + layout.dimensions);
			g_nativeENBOverlay = reinterpret_cast<ENBOverlay_t>(Detours::X64::DetourFunction(
				base + layout.overlay, reinterpret_cast<std::uintptr_t>(&NativeENBOverlay)));
			if (g_nativeENBOverlay) {
				logger::info("[ENB UI] Installed native overlay and AntTweakBar coordinate domain");
				return;
			}
		}
		logger::warn("[ENB UI] Unsupported overlay layout; ENB menu native coordinates unavailable");
	}
}

void Upscaling::InstallHooks()
{
	Screenshot::InstallHooks();
	// Disable TAA shader if using alternative scaling method
	stl::write_vfunc<0x8, ImageSpaceEffectTemporalAA_IsActive>(RE::VTABLE::ImageSpaceEffectTemporalAA[0]);
	// Fixed Fallout 4 entry points use explicit gateway prologues. These lengths
	// are instruction-boundary sizes verified in both OG 1.10.163 and AE 1.11.221.
	stl::detour_thunk_gateway<Interface3D_Renderer_Create>(
		REL::ID{ 88488, 2222519 },
		5,
		"Interface3D::Renderer::Create");

	const auto isOG = REX::FModule::IsRuntimeOG();
	// Normal simulation pacing before input/jobs, then the scene handoff.
	// The existing CALL5 helper retains each previous callee in func.
	stl::write_thunk_call<Main_Run_OnIdle>(
		REL::ID{ 1125396, 4484191 }.address() + (isOG ? 0xBB : 0xCB));
	stl::write_thunk_call<Main_OnIdle_Swap>(
		REL::ID{ 633524, 2228917 }.address() + (isOG ? 0x6EC : 0xCDC));
	logger::info("[Reflex] Installed Main simulation hooks; previous OnIdle={:x}, Swap={:x}",
		Main_Run_OnIdle::func.address(), Main_OnIdle_Swap::func.address());

	stl::detour_thunk_gateway<Renderer_Begin_ENBDomains>(
		REL::ID{ 288964, 2276833 }, isOG ? 8 : 6, "Renderer::Begin frame pacing and ENB domains");

	stl::detour_thunk_gateway<UI_ScreenSpace_RenderMenus_Native>(
		REL::ID{ 230711, 2284762 }, 5, "UI::ScreenSpace_RenderMenus overlay composition");
	if (enbLoaded) {
		NativeInterfaceUI::InstallHooks();
		InstallNativeENBOverlay();
		stl::detour_thunk_gateway<Scaleform_SetNativeScreenTarget>(
			REL::ID{ 1175949, 2284944 }, 6, "BSScaleformRenderer::SetCurrentRenderTarget native UI");
		// OG Begin +0x1F0 / AE Begin +0x1EC, immediately before per-frame
		// constants. Validate the call target before touching a runtime call site.
		const auto callSite = REL::ID{ 288964, 2276833 }.address() + (isOG ? 0x1F0 : 0x1EC);
		const auto* instruction = reinterpret_cast<const std::uint8_t*>(callSite);
		std::int32_t displacement = 0;
		std::memcpy(&displacement, instruction + 1, sizeof(displacement));
		const auto target = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(callSite + 5) + displacement);
		if (instruction[0] == 0xE8 && target == REL::ID{ 1453219, 2721521 }.address()) {
			stl::write_thunk_call<Renderer_Begin_SetFrameBufferProperties>(callSite);
			logger::info("[ENB domain] Installed Renderer::Begin framebuffer metadata hook");
		} else {
			logger::error("[ENB domain] Renderer::Begin framebuffer call did not match; metadata hook NOT installed");
		}
	}

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
	// ENB owns a full low-resolution frame from device creation onward. Do not
	// install private projection / DeferMix / prepass compensation hooks from
	// the old native-allocation bridge.
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
	const auto legacySharpness = ini.GetDoubleValue("Settings", "fRCASSharpness", 0.2);
	settings.sharpness = std::clamp(static_cast<float>(ini.GetDoubleValue("Settings", "fSharpness", legacySharpness)), 0.0f, 1.0f);

	auto streamline = Streamline::GetSingleton();
	const auto currentUpscaleMethodPreference = static_cast<UpscaleMethod>(settings.upscaleMethodPreference);
	if (ENBRenderDomain::Get().Active() && previousQualityMode != settings.qualityMode) {
		logger::info("[ENB domain] Requested quality {}; active quality {} ({}x{}); scene-only resize queued, HWND/display unchanged",
			settings.qualityMode, ENBRenderDomain::Get().Quality(), ENBRenderDomain::Get().Width(), ENBRenderDomain::Get().Height());
	}
	if (previousUpscaleMethodPreference != currentUpscaleMethodPreference ||
		(!ENBRenderDomain::Get().Active() && previousQualityMode != settings.qualityMode) ||
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
	ini.Delete("Settings", "bENBGPUTiming");
	ini.Delete("Settings", "bImageSpaceEffectLog");
	ini.Delete("Settings", "bTaggedTextureDebug");

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
	if (ENBRenderDomain::Get().Active()) {
		// All game and ENB allocations already cover a full low-res frame.
		// No top-left subrect proxies, native promotion, or depth enlargement.
		auto* renderer = RE::BSGraphics::GetRendererData();
		if (auto* view = renderer->renderTargets[0].srView) {
			winrt::com_ptr<ID3D11Resource> resource;
			reinterpret_cast<ID3D11ShaderResourceView*>(view)->GetResource(resource.put());
			const auto texture = resource.try_as<ID3D11Texture2D>();
			if (!texture) {
				return;
			}
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
			desc.MiscFlags = 0;
			EnsureTexture2D(desc, upscalingTexture, true, true, true);
		}
		return;
	}
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

	// Recreate render targets with new dimensions
	for (int i = 0; i < ARRAYSIZE(renderTargetsPatch); i++)
		UpdateRenderTarget(renderTargetsPatch[i], a_currentWidthRatio, a_currentHeightRatio);

	// Keep render-size dependent local textures allocated across menu
	// suspends. They are descriptor-checked below and in
	// CreateUpscalingResources(), so only real resolution/format changes
	// recreate GPU resources.
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
	auto frameBufferSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kFrameBuffer)].srView);

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

void Upscaling::ResetRenderTargets(std::initializer_list<int> a_indicesToCopy)
{
	// Restore all original full-resolution render targets
	for (int i = 0; i < ARRAYSIZE(renderTargetsPatch); i++) {
		int targetIndex = renderTargetsPatch[i];
		// If indices array is empty, copy all. Otherwise, only copy if in the array
		bool shouldCopy = (a_indicesToCopy.size() == 0) ||
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

void Upscaling::OverrideDepth(bool a_doCopy)
{
	static auto rendererData = RE::BSGraphics::GetRendererData();

	// Save the original depth SRV (with dynamic resolution)
	originalDepthView = reinterpret_cast<ID3D11ShaderResourceView*>(
		rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].srViewDepth);

	// Optionally perform expensive copy operation
	if (a_doCopy) {
		static auto gameViewport = Util::State_GetSingleton();

		// Only copy depth once per frame
		static auto previousFrame = gameViewport->frameCount;
		if (previousFrame != gameViewport->frameCount)
			CopyDepth();
		previousFrame = gameViewport->frameCount;
	}

	rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].srViewDepth =
		reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(depthOverrideTexture->srv.get());
}

void Upscaling::ResetDepth()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();

	rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].srViewDepth =
		reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(originalDepthView);

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
	if (upscaleMethod == UpscaleMethod::kDisabled ||
		(ENBRenderDomain::Get().Active() && (upscaleMethodNoMenu == UpscaleMethod::kDisabled || ShouldBlockUpscaling())))
		return;

	static auto samplerStates = SamplerStates::GetSingleton();
	for (int a = 0; a < 320; a++)
		samplerStates->a[a] = biasedSamplerStates[a];
}

void Upscaling::ResetSamplerStates()
{
	if (upscaleMethod == UpscaleMethod::kDisabled ||
		(ENBRenderDomain::Get().Active() && (upscaleMethodNoMenu == UpscaleMethod::kDisabled || ShouldBlockUpscaling())))
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
	auto depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].srViewDepth);

	// Get the dynamic resolution depth output UAV
	auto depthUAV = depthOverrideTexture->uav.get();

	// Also update the linearized depth used by other effects
	auto linearDepthUAV = reinterpret_cast<ID3D11UnorderedAccessView*>(rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMainDepthMips)].uaView);

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

void Upscaling::RetireD3D11Texture(std::unique_ptr<Texture2D>& a_texture)
{
	if (!a_texture) {
		return;
	}

	deferredResourceReleases.push_back({ 0, std::move(a_texture), {}, 0,
		completedPresentCount + kDeferredResourceReleasePresents });
}

void Upscaling::RetireSharedD3D12Texture(
	std::unique_ptr<Texture2D>& a_texture,
	winrt::com_ptr<ID3D12Resource>& a_d3d12Resource)
{
	if (!a_texture && !a_d3d12Resource) {
		return;
	}

	deferredResourceReleases.push_back({
		0,
		std::move(a_texture),
		std::move(a_d3d12Resource),
		0,
		completedPresentCount + kDeferredResourceReleasePresents });
}

void Upscaling::RetireD3D12Resource(winrt::com_ptr<ID3D12Resource>& a_resource)
{
	if (!a_resource) {
		return;
	}

	deferredResourceReleases.push_back({ 0, {}, std::move(a_resource), 0,
		completedPresentCount + kDeferredResourceReleasePresents });
}

void Upscaling::AdvanceDeferredResourceReleases()
{
	++completedPresentCount;
	auto* swap = DX12SwapChain::GetSingleton();
	// Called after Present submission: retired objects may have been referenced
	// by commands recorded earlier in this frame, so do not stamp them at retire time.
	if (!deferredResourceReleases.empty() && deferredResourceReleases.back().d3d11Fence == 0) {
		uint64_t fence11 = 0, fence12 = 0;
		if (swap->GetRetirementFences(fence11, fence12)) {
			for (auto it = deferredResourceReleases.rbegin(); it != deferredResourceReleases.rend() && it->d3d11Fence == 0; ++it) {
				it->d3d11Fence = fence11;
				it->d3d12Fence = fence12;
			}
		}
	}
	while (!deferredResourceReleases.empty() && deferredResourceReleases.front().d3d11Fence != 0 &&
		deferredResourceReleases.front().releasePresent <= completedPresentCount &&
		swap->AreRetirementFencesComplete(deferredResourceReleases.front().d3d11Fence,
			deferredResourceReleases.front().d3d12Fence)) {
		deferredResourceReleases.pop_front();
	}
}

void Upscaling::FlushDeferredResourceReleases()
{
	deferredResourceReleases.clear();
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
		RetireD3D11Texture(frameGenerationPreAlphaTexture);
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
		RetireD3D11Texture(frameGenerationMotionVectorTexture);
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
		RetireD3D11Texture(frameGenerationDepthTexture);
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

	return frameGenerationPreAlphaTexture &&
		frameGenerationMotionVectorTexture &&
		frameGenerationDepthTexture &&
		frameGenerationPreAlphaTexture->srv &&
		frameGenerationMotionVectorTexture->srv &&
		frameGenerationMotionVectorTexture->uav &&
		frameGenerationDepthTexture->srv &&
		frameGenerationDepthTexture->uav;
}

void Upscaling::PreFrameGenerationAlpha()
{
	static auto gameViewport = Util::State_GetSingleton();
	frameGenerationBuffersReady = false;
	frameGenerationPreAlphaReady = false;
	if (!WantsFrameGenerationInputs()) {
		return;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto& colorPostAlpha = rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMainTemp)];
	auto& motionVector = rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMotionVectors)];
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
	static auto gameViewport = Util::State_GetSingleton();
	if (!WantsFrameGenerationInputs() ||
		!frameGenerationPreAlphaTexture ||
		!frameGenerationPreAlphaReady ||
		frameGenerationPreAlphaFrame != gameViewport->frameCount) {
		return false;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto& colorPostAlpha = rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMainTemp)];
	auto& motionVector = rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMotionVectors)];
	auto& depth = rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)];

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
	if (!WantsFrameGenerationInputs()) {
		return;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto& colorPostAlpha = rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMainTemp)];
	auto& motionVector = rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMotionVectors)];
	auto& depth = rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)];
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
		RetireD3D12Resource(dlssD3D12PresentFinal[i]);
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
	if (ENBRenderDomain::Get().Active() && resourceUpscaleMethodNoMenu == UpscaleMethod::kDisabled) {
		// The immutable low-res scene still needs a display-sized spatial resolve.
		resourceUpscaleMethodNoMenu = UpscaleMethod::kSpatialFallback;
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

	// Preserve Main's temporary TAA disable for frozen backgrounds. Re-enabling
	// it here makes native/spatial menu frames read stale gameplay TAA history.
	static REL::Relocation<uint32_t*> enableTAA{ REL::ID{ 460417, 2704658 } };
	if (!EngineOwnsFrozenBackground() && *enableTAA == 0) {
		*enableTAA = 1;
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
	const bool virtualENB = ENBRenderDomain::Get().Active();
	if (virtualENB && upscaleMethod == UpscaleMethod::kDisabled && !windowUnavailable) {
		upscaleMethod = UpscaleMethod::kSpatialFallback;
	}

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
	if (ENBRenderDomain::Get().qualityChangePending) {
		frameGenerationActive = false;
		fsrFrameGenerationActive = false;
	}
	if (virtualENB && upscaleMethod != UpscaleMethod::kDLSS && upscaleMethod != UpscaleMethod::kFSR) {
		frameGenerationActive = false;
		fsrFrameGenerationActive = false;
	}
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
	if (virtualENB) {
		resolutionScale = static_cast<float>(ENBRenderDomain::Get().Width()) / DX12SwapChain::GetSingleton()->swapChainDesc.Width;
	}

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
	UpdateRenderTargets(virtualENB ? 1.0f : resolutionScale, virtualENB ? 1.0f : resolutionScale);
	UpdateGameSettings();

	auto displayWidth = gameViewport->screenWidth;
	auto displayHeight = gameViewport->screenHeight;
	if (upscalingTexture) {
		D3D11_TEXTURE2D_DESC desc{};
		upscalingTexture->resource->GetDesc(&desc);
		displayWidth = desc.Width;
		displayHeight = desc.Height;
	}
	if (virtualENB) {
		displayWidth = DX12SwapChain::GetSingleton()->swapChainDesc.Width;
		displayHeight = DX12SwapChain::GetSingleton()->swapChainDesc.Height;
	}

	const bool spatialENB = virtualENB && upscaleMethod == UpscaleMethod::kSpatialFallback;
	if (upscaleMethod == UpscaleMethod::kDisabled || spatialENB) {
		jitter = { 0.0f, 0.0f };
		osdRenderSize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
		osdNativeSize = osdRenderSize;
		gameViewport->offsetX = 0.0f;
		gameViewport->offsetY = 0.0f;
	}

	// Apply TAA jitter (shifts projection matrix sub-pixel per frame)
	if (upscaleMethod != UpscaleMethod::kDisabled && !spatialENB) {
		auto renderWidth = virtualENB ? ENBRenderDomain::Get().Width() : static_cast<uint>(static_cast<float>(displayWidth) * resolutionScale);
		auto renderHeight = virtualENB ? ENBRenderDomain::Get().Height() : static_cast<uint>(static_cast<float>(displayHeight) * resolutionScale);
		auto phaseCount = GetJitterPhaseCount(renderWidth, displayWidth);
		GetJitterOffset(&jitter.x, &jitter.y, gameViewport->frameCount, phaseCount);

		// Convert to NDC (X negated for DirectX)
		gameViewport->offsetX = 2.0f * -jitter.x / static_cast<float>(renderWidth);
		gameViewport->offsetY = 2.0f * jitter.y / static_cast<float>(renderHeight);
		osdRenderSize = { static_cast<float>(renderWidth), static_cast<float>(renderHeight) };
		osdNativeSize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
	}

	originalDynamicHeightRatio = virtualENB ? 1.0f : resolutionScale;
	originalDynamicWidthRatio = virtualENB ? 1.0f : resolutionScale;

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
	g_upscalingUpdateFrame = gameViewport->frameCount;
}

void Upscaling::Upscale(int a_renderTargetIndex)
{
	if (upscaleMethod == UpscaleMethod::kDisabled)
		return;
	if (!upscalingTexture) {
		return;
	}
	NativeInterfaceUI::RenderModelsBeforeUpscale(static_cast<uint32_t>(a_renderTargetIndex));

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
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, currentRTVs, currentDSV);
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
	if (ENBRenderDomain::Get().Active()) {
		D3D11_TEXTURE2D_DESC sceneDesc{};
		static_cast<ID3D11Texture2D*>(frameBufferResource)->GetDesc(&sceneDesc);
		const auto& domain = ENBRenderDomain::Get();
		if (sceneDesc.Width != domain.Width() || sceneDesc.Height != domain.Height()) {
			OnD3D12TemporalSuspend();
			static bool loggedMismatch = false;
			if (!loggedMismatch) {
				logger::error("[ENB domain] RT{} is {}x{}, expected {}x{}; skipping SDK evaluation (present uses spatial resolve)",
					a_renderTargetIndex, sceneDesc.Width, sceneDesc.Height, domain.Width(), domain.Height());
				loggedMismatch = true;
			}
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, currentRTVs, currentDSV);
			for (auto* rtv : currentRTVs) {
				if (rtv) {
					rtv->Release();
				}
			}
			if (currentDSV) {
				currentDSV->Release();
			}
			frameBufferResource->Release();
			return;
		}
	}

	static auto gameViewport = Util::State_GetSingleton();

	D3D11_TEXTURE2D_DESC upscaleDesc{};
	upscalingTexture->resource->GetDesc(&upscaleDesc);

	auto displaySize = float2(float(upscaleDesc.Width), float(upscaleDesc.Height));
	auto renderSize = float2(displaySize.x * originalDynamicWidthRatio, displaySize.y * originalDynamicHeightRatio);
	const bool virtualENB = ENBRenderDomain::Get().Active();
	if (virtualENB) {
		const auto& display = DX12SwapChain::GetSingleton()->swapChainDesc;
		renderSize = { static_cast<float>(ENBRenderDomain::Get().Width()), static_cast<float>(ENBRenderDomain::Get().Height()) };
		displaySize = { static_cast<float>(display.Width), static_cast<float>(display.Height) };
	}
	osdRenderSize = renderSize;
	osdNativeSize = displaySize;

	if (!(virtualENB &&
		(d3d12DLSSActive || upscaleMethod == UpscaleMethod::kSpatialFallback))) {
		// Non-ENB and native-AA paths already satisfy the ordinary render-rect
		// contract.
		// ENB spatial fallback snapshots frameBufferResource directly below;
		// its full-frame copy to upscalingTexture was never consumed.
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

	// Allocate temporal resources, then dilate only when the result is consumed.
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

		// CaptureDLSSGInputs receives the preserved/patched motion resource when
		// it is current. FSR SR receives engine motion directly. Neither reads
		// the dilation output in those cases; keep their existing inputs intact.
		const bool usePreservedMotionVectors =
			frameGenerationBuffersReady &&
			frameGenerationBuffersFrame == gameViewport->frameCount &&
			frameGenerationMotionVectorTexture &&
			frameGenerationMotionVectorTexture->resource;
		const bool needsDilatedMotionVectors =
			(d3d12DLSSActive || frameGenerationActive || fsrFrameGenerationActive) && !usePreservedMotionVectors;
		if (needsDilatedMotionVectors) {
			UpdateAndBindUpscalingCB(context, displaySize, renderSize);

			auto motionVectorSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMotionVectors)].srView);
			auto depthTextureSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].srViewDepth);

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
		if (virtualENB) {
			// FSR + DLSS-G still captures after evaluation. Do not erase the
			// scene until all inputs have been captured below.
			presentOverrideUIPrepared = true;
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
		RetireD3D12Resource(dlssD3D12PresentFinal[frameIndex]);
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
		if (virtualENB) {
			return false;
		}
		const auto frameIndex = dx12SwapChain->GetFrameIndex();
		if (frameIndex < fsrOutputSharedTextures.size() && fsrOutputSharedTextures[frameIndex]) {
			context->CopyResource(frameBufferResource, fsrOutputSharedTextures[frameIndex]->resource.get());
			return true;
		}
		return false;
	};
	auto copyD3D12DLSSOutputToD3D11 = [&]() {
		if (virtualENB) {
			return false;
		}
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
		if (virtualENB) {
			const auto frameIndex = dx12SwapChain->GetFrameIndex();
			if (frameIndex >= enbFallbackD3D12.size()) {
				return false;
			}
			dlssgInputsReady[frameIndex] = false;
			fsrFrameGenerationInputsReady[frameIndex] = false;
			frameGenerationActive = fsrFrameGenerationActive = false;
			Streamline::GetSingleton()->RequestDLSSGDisable();
			// At native size the engine can keep the world in its backbuffer.
			// No separate scene snapshot, slot wait or transparent UI clear is needed.
			if (upscaleMethod == UpscaleMethod::kSpatialFallback &&
				renderSize.x == displaySize.x && renderSize.y == displaySize.y) {
				dx12SwapChain->SetPresentOverride(nullptr);
				return true;
			}
			if (!dx12SwapChain->WaitForFrameSlot(frameIndex)) {
				return false;
			}
			// No temporal result this frame: snapshot R and let the present shader
			// resolve R -> D with late UI. Never CopyResource between R and D.
			D3D11_TEXTURE2D_DESC desc{};
			static_cast<ID3D11Texture2D*>(frameBufferResource)->GetDesc(&desc);
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			desc.MiscFlags = 0;
			EnsureSharedD3D12Texture(this, desc, enbFallbackSharedTextures[frameIndex], enbFallbackD3D12[frameIndex], false);
			context->CopyResource(enbFallbackSharedTextures[frameIndex]->resource.get(), frameBufferResource);
			return prepareD3D12PresentOverrideUI() && setD3D12PresentOverride(enbFallbackD3D12[frameIndex].get());
		}
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
		auto motionVectorTexture = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMotionVectors)].texture);
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
			const auto usePresentOverride = getD3D12FSROutput() != nullptr && (virtualENB || !scopeMenuOpen);
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
			motionVectorTexture = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMotionVectors)].texture);
		}
		const bool usePatchedFrameGenerationBuffers =
			frameGenerationBuffersReady &&
			frameGenerationBuffersFrame == gameViewport->frameCount &&
			frameGenerationMotionVectorTexture &&
			frameGenerationMotionVectorTexture->resource;
		if (usePatchedFrameGenerationBuffers) {
			motionVectorTexture = frameGenerationMotionVectorTexture->resource.get();
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
		const auto usePresentOverride = hasPresentOverrideOutput && (virtualENB || !scopeMenuOpen);
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

	if (virtualENB && presentOverrideUIPrepared) {
		if (auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(rendererData->renderTargets[a_renderTargetIndex].rtView)) {
			const float transparent[4]{};
			context->ClearRenderTargetView(rtv, transparent);
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
	if (!dx12SwapChain->WaitForFrameSlot(frameIndex, true)) {
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
	EnsureSharedD3D12Texture(this, inputDesc, fsrInputSharedTextures[frameIndex], fsrInputD3D12[frameIndex], false);
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
	EnsureSharedD3D12Texture(this, outputDesc, fsrOutputSharedTextures[frameIndex], fsrOutputD3D12[frameIndex], true);

	if (auto* opaqueOnly = FidelityFX::GetSingleton()->colorOpaqueOnlyTexture.get(); opaqueOnly && opaqueOnly->resource) {
		D3D11_TEXTURE2D_DESC opaqueDesc{};
		opaqueOnly->resource->GetDesc(&opaqueDesc);
		opaqueDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		opaqueDesc.MiscFlags = 0;
		EnsureSharedD3D12Texture(this, opaqueDesc, fsrOpaqueOnlySharedTextures[frameIndex], fsrOpaqueOnlyD3D12[frameIndex], false);
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
	EnsureSharedD3D12Texture(this, reactiveDesc, fsrReactiveMaskSharedTextures[frameIndex], fsrReactiveMaskD3D12[frameIndex], true);

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
	EnsureSharedD3D12Texture(this, motionVectorDesc, fsrMotionVectorSharedTextures[frameIndex], fsrMotionVectorD3D12[frameIndex], false);
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
	EnsureSharedD3D12Texture(this, depthDesc, fsrDepthSharedTextures[frameIndex], fsrDepthD3D12[frameIndex], true);

	auto depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].srViewDepth);
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
	// Invalidate this slot before any early return (including a guide mismatch).
	// A resource remaining alive does not mean it contains this frame's inputs.
	const auto captureIndex = DX12SwapChain::GetSingleton()->GetFrameIndex();
	if (captureIndex < dlssD3D12InputsReady.size()) {
		dlssD3D12InputsReady[captureIndex] = false;
		dlssgInputsReady[captureIndex] = false;
		fsrFrameGenerationInputsReady[captureIndex] = false;
	}
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

	auto depthTexture = reinterpret_cast<ID3D11Texture2D*>(rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].texture);
	auto motionVectorTexture = a_motionVectorTexture ? a_motionVectorTexture : reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMotionVectors)].texture);
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
	if (ENBRenderDomain::Get().Active()) {
		const auto& domain = ENBRenderDomain::Get();
		const bool matchingGuides = motionVectorDesc.Width == domain.Width() &&
			motionVectorDesc.Height == domain.Height() && depthDesc.Width == domain.Width() && depthDesc.Height == domain.Height();
		if (!matchingGuides) {
			static bool loggedGuideMismatch = false;
			if (!loggedGuideMismatch) {
				logger::error("[ENB domain] Depth/motion allocations do not match scene {}x{}; temporal evaluation skipped", domain.Width(), domain.Height());
				loggedGuideMismatch = true;
			}
			frameBufferResource->Release();
			return;
		}
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
		if (!dx12SwapChain->WaitForFrameSlot(frameIndex, useD3D12DLSS)) {
			frameBufferResource->Release();
			return;
		}
		dlssgInputsReady[frameIndex] = false;
		fsrFrameGenerationInputsReady[frameIndex] = false;
		dlssD3D12InputsReady[frameIndex] = false;
		dlssD3D12Sharpened[frameIndex] = false;
		dlssD3D12TransparencyMaskReady[frameIndex] = false;
		RetireD3D12Resource(dlssD3D12PresentFinal[frameIndex]);
		const bool reuseFSRResourcesForFrameGeneration =
			(useFSRFrameGeneration || (ENBRenderDomain::Get().Active() && useFrameGeneration)) &&
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
			EnsureSharedD3D12Texture(this, dlssInputDesc, dlssInputSharedTextures[frameIndex], dlssInputD3D12[frameIndex], false);
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
				ENBRenderDomain::Get().Active() ? frameBufferTexture : upscalingTexture->resource.get(),
				0,
				&colorSourceBox);

			if (settings.sharpness > 0.0f || settings.dlssNREnabled != 0) {
				auto sharpenedDesc = frameBufferDesc;
				sharpenedDesc.Width = static_cast<UINT>(a_displaySize.x);
				sharpenedDesc.Height = static_cast<UINT>(a_displaySize.y);
				sharpenedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				sharpenedDesc.MiscFlags = 0;
				EnsureSharedD3D12Texture(this, sharpenedDesc, dlssSharpenedSharedTextures[frameIndex], dlssSharpenedD3D12[frameIndex], true);
			}

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
				// WaitForFrameSlot above protects this slot from its previous D3D12
				// reader. Generate directly into the shared input; the input-ready
				// signal below orders these UAV writes before DLSS reads them.
				EnsureSharedD3D12Texture(this, maskDesc, dlssTransparencyMaskSharedTextures[frameIndex], dlssTransparencyMaskD3D12[frameIndex], true);
				auto* mask = dlssTransparencyMaskSharedTextures[frameIndex].get();

				auto shader = GetGenerateDLSSTransparencyMaskCS();
				if (shader && mask && mask->uav) {
					ID3D11ShaderResourceView* views[] = {
						opaqueOnly->srv.get(),
						ENBRenderDomain::Get().Active() ? frameBufferSRV : upscalingTexture->srv.get()
					};
					context->CSSetShaderResources(0, ARRAYSIZE(views), views);

					ID3D11UnorderedAccessView* uavs[] = {
						mask->uav.get()
					};
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
					context->CSSetShader(shader, nullptr, 0);

					context->Dispatch((maskDesc.Width + 7) / 8, (maskDesc.Height + 7) / 8, 1);
					ID3D11ShaderResourceView* nullViews[2] = {};
					context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
					ClearDLSSGComputeBindings(context);

					dlssD3D12TransparencyMaskReady[frameIndex] = true;
				}
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
			RetireD3D11Texture(dlssgHUDLessSharedTextures[frameIndex]);
			RetireD3D12Resource(dlssgHUDLessD3D12[frameIndex]);
			dlssgHUDLessD3D12[frameIndex].copy_from(fsrOutputD3D12[frameIndex].get());
		} else {
			auto hudlessDesc = frameBufferDesc;
			hudlessDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (useD3D12DLSS) {
				hudlessDesc.Width = static_cast<UINT>(a_displaySize.x);
				hudlessDesc.Height = static_cast<UINT>(a_displaySize.y);
				hudlessDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
			}
			hudlessDesc.MiscFlags = 0;
			EnsureSharedD3D12Texture(this, hudlessDesc, dlssgHUDLessSharedTextures[frameIndex], dlssgHUDLessD3D12[frameIndex], false);
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
			RetireD3D11Texture(dlssgMotionVectorSharedTextures[frameIndex]);
			RetireD3D12Resource(dlssgMotionVectorD3D12[frameIndex]);
			dlssgMotionVectorD3D12[frameIndex].copy_from(fsrMotionVectorD3D12[frameIndex].get());
		} else {
			EnsureSharedD3D12Texture(this, sharedMotionVectorDesc, dlssgMotionVectorSharedTextures[frameIndex], dlssgMotionVectorD3D12[frameIndex], false);
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
			RetireD3D11Texture(dlssgDepthSharedTextures[frameIndex]);
			RetireD3D12Resource(dlssgDepthD3D12[frameIndex]);
			dlssgDepthD3D12[frameIndex].copy_from(fsrDepthD3D12[frameIndex].get());
		} else {
			EnsureSharedD3D12Texture(this, sharedDepthDesc, dlssgDepthSharedTextures[frameIndex], dlssgDepthD3D12[frameIndex], true);

			auto depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[Util::ResolveDepthStencilTarget(Util::DepthStencilTarget::kMain)].srViewDepth);
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
		auto& main = renderer->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMain)];
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
	RetireD3D11Texture(upscalingTexture);
	RetireD3D11Texture(dlssOutputTexture);
	RetireD3D11Texture(spatialFallbackTexture);
	RetireD3D11Texture(dilatedMotionVectorTexture);
	RetireD3D11Texture(dlssgHUDLessTexture);
	RetireD3D11Texture(frameGenerationPreAlphaTexture);
	RetireD3D11Texture(frameGenerationMotionVectorTexture);
	RetireD3D11Texture(frameGenerationDepthTexture);
	frameGenerationPreAlphaReady = false;
	frameGenerationPreAlphaFrame = 0;
	frameGenerationBuffersReady = false;
	for (std::size_t i = 0; i < dlssgInputsReady.size(); ++i) {
		RetireSharedD3D12Texture(dlssInputSharedTextures[i], dlssInputD3D12[i]);
		RetireSharedD3D12Texture(dlssSharpenedSharedTextures[i], dlssSharpenedD3D12[i]);
		RetireSharedD3D12Texture(dlssgHUDLessSharedTextures[i], dlssgHUDLessD3D12[i]);
		RetireSharedD3D12Texture(dlssgMotionVectorSharedTextures[i], dlssgMotionVectorD3D12[i]);
		RetireSharedD3D12Texture(dlssgDepthSharedTextures[i], dlssgDepthD3D12[i]);
		RetireSharedD3D12Texture(dlssTransparencyMaskSharedTextures[i], dlssTransparencyMaskD3D12[i]);
		RetireSharedD3D12Texture(enbFallbackSharedTextures[i], enbFallbackD3D12[i]);
		RetireSharedD3D12Texture(fsrInputSharedTextures[i], fsrInputD3D12[i]);
		RetireSharedD3D12Texture(fsrOutputSharedTextures[i], fsrOutputD3D12[i]);
		RetireSharedD3D12Texture(fsrOpaqueOnlySharedTextures[i], fsrOpaqueOnlyD3D12[i]);
		RetireSharedD3D12Texture(fsrReactiveMaskSharedTextures[i], fsrReactiveMaskD3D12[i]);
		RetireSharedD3D12Texture(fsrMotionVectorSharedTextures[i], fsrMotionVectorD3D12[i]);
		RetireSharedD3D12Texture(fsrDepthSharedTextures[i], fsrDepthD3D12[i]);
		RetireD3D12Resource(dlssD3D12PresentFinal[i]);
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

}

void Upscaling::PatchSSRShader()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	const auto ratios = GetDynamicResolutionRatios();

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
