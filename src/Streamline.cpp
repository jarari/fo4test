#include "Streamline.h"
#ifdef UPSCALING_NR_CAPTURE
#include "NRDiagnosticCapture.h"
#endif
#include "Upscaling.h"

#include "third_party/RTX40MFGUnlock/integration.h"

#include <algorithm>
#include <cmath>
#include <magic_enum/magic_enum.hpp>

#include "DX12SwapChain.h"
#include "ENBRenderDomain.h"
#include "Util.h"

namespace
{
	constexpr wchar_t kPCLStatsPingMessageName[] = L"PC_Latency_Stats_Ping";
	constexpr uint32_t kDLSSGStateQueryInterval = 15;

	void StreamlineLogCallback(sl::LogType a_type, const char* a_message)
	{
		if (!a_message) {
			return;
		}

		if (a_type == sl::LogType::eInfo) {
			return;
		}

		switch (a_type) {
		case sl::LogType::eWarn:
			logger::warn("[Streamline SDK] {}", a_message);
			break;
		case sl::LogType::eError:
			logger::error("[Streamline SDK] {}", a_message);
			break;
		default:
			logger::info("[Streamline SDK] {}", a_message);
			break;
		}
	}

	void DLSSGAPIErrorCallback(const sl::APIError& a_error)
	{
		logger::warn("[Streamline] DLSS-G present API error hres=0x{:08X}", static_cast<uint32_t>(a_error.hres));
	}

	uint32_t ResourceWidth(ID3D12Resource* a_resource)
	{
		return a_resource ? static_cast<uint32_t>(a_resource->GetDesc().Width) : 0;
	}

	uint32_t ResourceHeight(ID3D12Resource* a_resource)
	{
		return a_resource ? static_cast<uint32_t>(a_resource->GetDesc().Height) : 0;
	}

	uint32_t ResourceFormat(ID3D12Resource* a_resource)
	{
		return a_resource ? static_cast<uint32_t>(a_resource->GetDesc().Format) : 0;
	}

	void ApplyDLSSModelPreset(sl::DLSSOptions& a_options, uint a_preset)
	{
		switch (a_preset) {
		case 1:
			a_options.dlaaPreset = sl::DLSSPreset::eDefault;
			a_options.qualityPreset = sl::DLSSPreset::eDefault;
			a_options.balancedPreset = sl::DLSSPreset::eDefault;
			a_options.performancePreset = sl::DLSSPreset::eDefault;
			a_options.ultraPerformancePreset = sl::DLSSPreset::eDefault;
			break;
		case 2:
			a_options.dlaaPreset = a_options.qualityPreset = a_options.balancedPreset =
				a_options.performancePreset = a_options.ultraPerformancePreset = sl::DLSSPreset::ePresetK;
			break;
		case 3:
			a_options.dlaaPreset = a_options.qualityPreset = a_options.balancedPreset =
				a_options.performancePreset = a_options.ultraPerformancePreset = sl::DLSSPreset::ePresetM;
			break;
		case 4:
			a_options.dlaaPreset = a_options.qualityPreset = a_options.balancedPreset =
				a_options.performancePreset = a_options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
			break;
		case 0:
		default:
			a_options.dlaaPreset = sl::DLSSPreset::ePresetK;
			a_options.qualityPreset = sl::DLSSPreset::ePresetK;
			a_options.balancedPreset = sl::DLSSPreset::ePresetK;
			a_options.performancePreset = sl::DLSSPreset::ePresetM;
			a_options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
			break;
		}
	}

	bool SameDLSSNROptions(const sl::DLSSNROptions& a_lhs, const sl::DLSSNROptions& a_rhs)
	{
		return a_lhs.mode == a_rhs.mode &&
			a_lhs.intensity == a_rhs.intensity &&
			a_lhs.localToneStrength == a_rhs.localToneStrength &&
			a_lhs.localStructureStrength == a_rhs.localStructureStrength &&
			a_lhs.style == a_rhs.style &&
			a_lhs.preset == a_rhs.preset &&
			a_lhs.useAutoMask == a_rhs.useAutoMask &&
			a_lhs.skinStructureStrength == a_rhs.skinStructureStrength &&
			a_lhs.performanceMode == a_rhs.performanceMode;
	}

	std::string DLSSGStatusFlags(sl::DLSSGStatus a_status)
	{
		if (a_status == sl::DLSSGStatus::eOk) {
			return "eOk";
		}

		std::string flags;
		const auto append = [&](sl::DLSSGStatus a_flag, std::string_view a_name) {
			if (a_status & a_flag) {
				if (!flags.empty()) {
					flags += '|';
				}
				flags += a_name;
			}
		};

		append(sl::DLSSGStatus::eFailResolutionTooLow, "ResolutionTooLow");
		append(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime, "ReflexNotDetected");
		append(sl::DLSSGStatus::eFailHDRFormatNotSupported, "HDRFormatNotSupported");
		append(sl::DLSSGStatus::eFailCommonConstantsInvalid, "CommonConstantsInvalid");
		append(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled, "GetCurrentBackBufferIndexNotCalled");
		append(sl::DLSSGStatus::eReserved5, "Reserved5");
		return flags.empty() ? "unknown" : flags;
	}

	sl::float4x4 ToSLMatrix(const DirectX::XMMATRIX& a_matrix)
	{
		DirectX::XMFLOAT4X4 matrix{};
		DirectX::XMStoreFloat4x4(&matrix, a_matrix);

		sl::float4x4 result{};
		std::memcpy(&result, &matrix, sizeof(result));
		return result;
	}

	sl::float4x4 ToSLMatrix(const __m128* a_matrix)
	{
		return ToSLMatrix(Util::ToXMMatrix(a_matrix));
	}

	bool IsUsableMatrix(const DirectX::XMMATRIX& a_matrix)
	{
		DirectX::XMFLOAT4X4 matrix{};
		DirectX::XMStoreFloat4x4(&matrix, a_matrix);

		const float* values = &matrix._11;
		bool anyNonZero = false;
		for (uint32_t i = 0; i < 16; ++i) {
			if (!std::isfinite(values[i])) {
				return false;
			}
			anyNonZero |= values[i] != 0.0f;
		}
		return anyNonZero;
	}

	bool TryInvertMatrix(const DirectX::XMMATRIX& a_matrix, DirectX::XMMATRIX& a_inverse)
	{
		if (!IsUsableMatrix(a_matrix)) {
			return false;
		}

		DirectX::XMVECTOR determinant{};
		a_inverse = DirectX::XMMatrixInverse(&determinant, a_matrix);
		const auto det = DirectX::XMVectorGetX(determinant);
		return std::isfinite(det) && det != 0.0f && IsUsableMatrix(a_inverse);
	}

	bool IsFinitePosition(const RE::NiPoint3& a_position)
	{
		return
			std::isfinite(a_position.x) &&
			std::isfinite(a_position.y) &&
			std::isfinite(a_position.z);
	}
}

void Streamline::LoadInterposer()
{
	interposer = LoadLibraryW(L"Data/F4SE/Plugins/Upscaling/Streamline/sl.interposer.dll");
	if (interposer == nullptr) {
		DWORD errorCode = GetLastError();
		logger::info("[Streamline] Failed to load interposer: Error Code {0:x}", errorCode);
	} else {
		wchar_t modulePath[MAX_PATH]{};
		if (GetModuleFileNameW(interposer, modulePath, static_cast<DWORD>(std::size(modulePath))) > 0) {
			interposerDirectory = std::filesystem::path(modulePath).parent_path().wstring();
		}
		logger::info("[Streamline] Interposer loaded at address: {0:p}", static_cast<void*>(interposer));
		logger::info("[Streamline] Runtime path: {}", std::filesystem::path(interposerDirectory).string());
		directDLSSNR.SetRuntimeDirectory(interposerDirectory);
	}
}

void Streamline::Initialize(sl::RenderAPI a_renderAPI)
{
	if (initialized) {
		if (initializedRenderAPI != a_renderAPI) {
			logger::warn("[Streamline] Already initialized with render API {}, cannot switch to {}", static_cast<uint32_t>(initializedRenderAPI), static_cast<uint32_t>(a_renderAPI));
		}
		return;
	}

	logger::info("[Streamline] Initializing Streamline");

	sl::Preferences pref;

	sl::Feature d3d11FeaturesToLoad[] = { sl::kFeatureDLSS, sl::kFeatureNIS, sl::kFeatureReflex, sl::kFeaturePCL };
	sl::Feature d3d12FeaturesToLoad[] = { sl::kFeatureImGUI, sl::kFeatureDLSS, sl::kFeatureDLSS_NR, sl::kFeatureNIS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
	if (a_renderAPI == sl::RenderAPI::eD3D12) {
		pref.featuresToLoad = d3d12FeaturesToLoad;
		pref.numFeaturesToLoad = _countof(d3d12FeaturesToLoad);
	} else {
		pref.featuresToLoad = d3d11FeaturesToLoad;
		pref.numFeaturesToLoad = _countof(d3d11FeaturesToLoad);
	}

	pref.logLevel = a_renderAPI == sl::RenderAPI::eD3D12 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
	pref.logMessageCallback = StreamlineLogCallback;
	pref.showConsole = false;
	if (a_renderAPI == sl::RenderAPI::eD3D12) {
		pref.flags &= ~sl::PreferenceFlags::eDisableDebugText;
	}

	const wchar_t* pluginPaths[] = { interposerDirectory.c_str() };
	if (!interposerDirectory.empty()) {
		pref.pathsToPlugins = pluginPaths;
		pref.numPathsToPlugins = _countof(pluginPaths);

		for (const auto& runtimeDependency : { L"sl.imgui.dll", L"sl.dlss_nr.dll", L"nvngx_dlssnr.dll", L"sl.dlss_g.dll", L"nvngx_dlssg.dll", L"sl.nis.dll", L"sl.reflex.dll", L"sl.pcl.dll" }) {
			const auto dependencyPath = std::filesystem::path(interposerDirectory) / runtimeDependency;
			logger::info("[Streamline] Runtime dependency {} {}", dependencyPath.string(), std::filesystem::exists(dependencyPath) ? "found" : "missing");
		}
	}

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";
	pref.flags |= sl::PreferenceFlags::eUseManualHooking;
	pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.flags &= ~(sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins);
	logger::info("[Streamline] OTA downloads and downloaded plugin loading disabled");
	RTX40MFGUnlock::InstallLoaderDiscovery(interposer);

	pref.renderAPI = a_renderAPI;

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTag = (PFun_slSetTag2*)GetProcAddress(interposer, "slSetTag");
	slSetTagForFrame = (PFun_slSetTagForFrame*)GetProcAddress(interposer, "slSetTagForFrame");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	const auto dlssNRRuntimePresent =
		a_renderAPI == sl::RenderAPI::eD3D12 &&
		std::filesystem::exists(std::filesystem::path(interposerDirectory) / L"sl.dlss_nr.dll");
	const auto sdkVersion = dlssNRRuntimePresent ? sl::kSDKVersionDLSSNRPreview : sl::kSDKVersion;
	if (SL_FAILED(res, slInit(pref, sdkVersion))) {
		logger::critical("[Streamline] Failed to initialize Streamline: {}", magic_enum::enum_name(res));
	} else {
		initialized = true;
		initializedRenderAPI = a_renderAPI;
		logger::info("[Streamline] Successfully initialized Streamline sdkVersion=0x{:016X}", sdkVersion);
		if (a_renderAPI == sl::RenderAPI::eD3D12) {
			RTX40MFGUnlock::PatchLoadedModules();
		}
	}
}

void Streamline::Shutdown()
{
	directDLSSNR.Shutdown();
	dlssNRSuspended = false;
	if (initialized && slShutdown) {
		if (SL_FAILED(result, slShutdown())) {
			logger::warn("[Streamline] Shutdown failed: {}", magic_enum::enum_name(result));
		}
	}

	initialized = false;
	initializedRenderAPI = sl::RenderAPI::eD3D11;
	featureDLSS = false;
	featureDLSSNR = false;
	featureDLSSG = false;
	featureNIS = false;
	featureReflex = false;
	featurePCL = false;
	featureImGUI = false;
	dlssgActive = false;
	frameToken = nullptr;
	swapChain = nullptr;
	swapChainDesc = {};
	constantsFrameIndex = std::numeric_limits<uint32_t>::max();
	constantsTokenIndex = std::numeric_limits<uint32_t>::max();
	dlssgVSyncSupported = false;
	lastConstantsFrameIndex = std::numeric_limits<uint32_t>::max();
	lastTemporalResetFrameIndex = std::numeric_limits<uint32_t>::max();
	constantsReferenceCamera = nullptr;
	temporalResetPending = true;
	markerFrameIndex = std::numeric_limits<uint32_t>::max();
	reflexSleepFrame = std::numeric_limits<uint32_t>::max();
	simulationMarkerFrame = std::numeric_limits<uint32_t>::max();
	renderMarkerFrame = std::numeric_limits<uint32_t>::max();
	lastDLSSGStatus = std::numeric_limits<uint32_t>::max();
	lastDLSSGPresentedFrames = std::numeric_limits<uint32_t>::max();
	lastDLSSGStateQueryFrame = std::numeric_limits<uint32_t>::max();
	lastDLSSGPresentMultiplier = 1.0;
	maxFramesToGenerate = 1;
	dynamicMFGSupported = false;
	dlssgStateKnown = false;
	loggedDynamicMFGUnsupported = false;
	currentFrameTokenIndex = std::numeric_limits<uint32_t>::max();
	presentFrameToken = nullptr;
	presentFrameTokenIndex = std::numeric_limits<uint32_t>::max();
	lastPresentFrameTokenIndex = std::numeric_limits<uint32_t>::max();
	currentReflexMode = sl::ReflexMode::ReflexMode_eCount;
	currentReflexUseMarkersToOptimize = false;
	currentReflexThreadId = 0;
	currentDLSSGMode = sl::DLSSGMode::eCount;
	currentDLSSGGeneratedFrames = 0;
	currentDLSSGDynamicTargetFPS = 0;
	currentDLSSQualityMode = 1;
	currentDLSSModelPreset = 0;
	loggedDLSSNRFallback = false;
	ResetOptionCaches();
}

void Streamline::SetSwapChain(IDXGISwapChain* a_swapChain)
{
	swapChain = a_swapChain;
	swapChainDesc = {};

	if (swapChain) {
		if (FAILED(swapChain->GetDesc(&swapChainDesc))) {
			logger::warn("[Streamline] Could not query swap chain description");
		}
	}
}

void Streamline::CheckFeature(sl::Feature a_feature, IDXGIAdapter* a_adapter, bool& a_available, std::string_view a_name)
{
	a_available = false;

	if (!slIsFeatureLoaded || !slIsFeatureSupported || !a_adapter) {
		logger::warn("[Streamline] Cannot check {} feature: Streamline is not fully initialized", a_name);
		return;
	}

	DXGI_ADAPTER_DESC adapterDesc{};
	a_adapter->GetDesc(&adapterDesc);

	sl::AdapterInfo adapterInfo{};
	adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterDesc.AdapterLuid);
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

	// Check support before loaded state. slIsFeatureLoaded validates that the
	// plugin context exists and emits a misleading "context is missing" SDK
	// error when a requested optional plugin was rejected during discovery.
	// slIsFeatureSupported is the quiet query and preserves the actual reason.
	const auto support = slIsFeatureSupported(a_feature, adapterInfo);
	if (support != sl::Result::eOk) {
		logger::info("[Streamline] {} feature is not available ({})", a_name, magic_enum::enum_name(support));
		return;
	}

	bool loaded = false;
	if (SL_FAILED(result, slIsFeatureLoaded(a_feature, loaded))) {
		logger::info("[Streamline] {} feature loaded check failed: {}", a_name, magic_enum::enum_name(result));
		return;
	}

	a_available = loaded;
	logger::info("[Streamline] {} feature {} available ({})", a_name, loaded ? "is" : "is not", magic_enum::enum_name(support));
}

void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
{
	logger::info("[Streamline] Checking features");
	if (UsesD3D12()) {
		CheckFeature(sl::kFeatureDLSS, a_adapter, featureDLSS, "DLSS");
		CheckFeature(sl::kFeatureDLSS_NR, a_adapter, featureDLSSNR, "DLSS-NR");
		if (!featureDLSSNR) {
			PrepareDirectDLSSNR();
		}
	} else {
		featureDLSS = false;
		featureDLSSNR = false;
		logger::info("[Streamline] DLSS skipped: D3D11 DLSS SR is deprecated; use the D3D12 proxy path");
	}
	if (UsesD3D12()) {
		CheckFeature(sl::kFeatureDLSS_G, a_adapter, featureDLSSG, "DLSS-G");
	} else {
		featureDLSSG = false;
		logger::info("[Streamline] DLSS-G skipped: Streamline DLSS-G runtime is D3D12/Vulkan only");
	}
	CheckFeature(sl::kFeatureReflex, a_adapter, featureReflex, "Reflex");
	CheckFeature(sl::kFeatureNIS, a_adapter, featureNIS, "NIS");
	CheckFeature(sl::kFeaturePCL, a_adapter, featurePCL, "PCL");
	if (UsesD3D12()) {
		CheckFeature(sl::kFeatureImGUI, a_adapter, featureImGUI, "SL ImGui");
	} else {
		featureImGUI = false;
	}
}

void Streamline::PrepareDirectDLSSNR()
{
	if (!UsesD3D12() || featureDLSSNR) {
		return;
	}

	auto* device = DX12SwapChain::GetSingleton()->GetD3D12Device();
	if (!device) {
		logger::info("[DLSS-NR Direct] D3D12 device is not ready; initialization remains deferred until evaluation");
		return;
	}

	logger::info("[DLSS-NR Direct] Streamline DLSS-NR is unavailable; initializing the direct path now");
	directDLSSNR.Prepare(device);
}

void Streamline::PostDevice()
{
	if (UsesD3D12()) {
		RTX40MFGUnlock::PatchLoadedModules();
	}

	if (featureDLSS) {
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void*&)slDLSSGetOptimalSettings);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", (void*&)slDLSSGetState);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void*&)slDLSSSetOptions);
	}

	if (featureDLSSNR) {
		if (SL_FAILED(result, slGetFeatureFunction(sl::kFeatureDLSS_NR, "slDLSSNRSetOptions", (void*&)slDLSSNRSetOptions)) || !slDLSSNRSetOptions) {
			logger::warn("[Streamline] Could not resolve slDLSSNRSetOptions: {}", magic_enum::enum_name(result));
			featureDLSSNR = false;
		}
	}
	if (!featureDLSSNR) {
		// The first feature query runs before the proxy creates its D3D12 device.
		// PostDevice is the earliest guaranteed point where direct NGX Init_Ext
		// can run after Streamline rejects the DLSS-NR plugin.
		PrepareDirectDLSSNR();
	}

	if (featureNIS) {
		slGetFeatureFunction(sl::kFeatureNIS, "slNISGetState", (void*&)slNISGetState);
		slGetFeatureFunction(sl::kFeatureNIS, "slNISSetOptions", (void*&)slNISSetOptions);
	}

	if (featureDLSSG) {
		slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)slDLSSGGetState);
		slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)slDLSSGSetOptions);
		RTX40MFGUnlock::ObserveWrapper(reinterpret_cast<const void*>(slDLSSGSetOptions));
	}

	if (featureReflex) {
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", (void*&)slReflexGetState);
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", (void*&)slReflexSleep);
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", (void*&)slReflexSetOptions);

		if (slReflexGetState) {
			sl::ReflexState state{};
			if (SL_SUCCEEDED(result, slReflexGetState(state))) {
				logger::info("[Streamline] Reflex low latency {}", state.lowLatencyAvailable ? "is available" : "is not available");
			}
		}
	}

	if (featurePCL) {
		slGetFeatureFunction(sl::kFeaturePCL, "slPCLGetState", (void*&)slPCLGetState);
		slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", (void*&)slPCLSetMarker);
		slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetOptions", (void*&)slPCLSetOptions);

		if (slPCLSetOptions) {
			sl::PCLOptions options{};
			// Leave idThread unset so PCL posts the stats message to the foreground game
			// window. Setting it makes Streamline use PostThreadMessageW, bypassing our
			// WndProc hook.
			options.idThread = 0;
			if (SL_FAILED(result, slPCLSetOptions(options))) {
				logger::warn("[Streamline] Could not set PCL options: {}", magic_enum::enum_name(result));
			}
		}

		if (slPCLGetState) {
			sl::PCLState state{};
			if (SL_SUCCEEDED(result, slPCLGetState(state))) {
				pclStatsWindowMessage = state.statsWindowMessage;
			}
		}
		if (pclStatsWindowMessage == 0) {
			pclStatsWindowMessage = RegisterWindowMessageW(kPCLStatsPingMessageName);
		}
		logger::info("[Streamline] PCL stats message id {}", pclStatsWindowMessage);
	}
}

bool Streamline::EnsureFrameToken(uint32_t a_frameIndex)
{
	if (!initialized || !slGetNewFrameToken) {
		return false;
	}

	if (markerFrameIndex == a_frameIndex && frameToken) {
		return true;
	}

	if (SL_FAILED(res, slGetNewFrameToken(frameToken, &a_frameIndex))) {
		logger::error("[Streamline] Could not get frame token: {}", magic_enum::enum_name(res));
		return false;
	}

	markerFrameIndex = a_frameIndex;
	currentFrameTokenIndex = static_cast<uint32_t>(*frameToken);

	return true;
}

bool Streamline::PaceFrame(uint32_t a_frameIndex)
{
	// Before Reflex Sleep and input markers, even without Streamline/Reflex.
	DX12SwapChain::GetSingleton()->PaceFrameStart(a_frameIndex);
	const auto* upscaling = Upscaling::GetSingleton();
	const bool validToken = EnsureFrameToken(a_frameIndex);
	if (!validToken || reflexSleepFrame == a_frameIndex) {
		return validToken;
	}
	reflexSleepFrame = a_frameIndex;
	UpdateReflex(upscaling->settings.reflexMode, upscaling->IsFrameGenerationActive());

	// Normal gameplay calls this before Main::OnIdle starts simulation/input
	// jobs. Renderer::Begin is only a fallback for independent loading/movie draws.
	// The SDK requires Sleep even with Reflex Off; the selected mode controls it.
	if (featureReflex && slReflexSleep) {
		if (SL_FAILED(res, slReflexSleep(*frameToken))) {
			logger::warn("[Streamline] Reflex sleep failed: {}", magic_enum::enum_name(res));
		}
	}

	return true;
}

bool Streamline::BeginSimulationFrame(uint32_t a_frameIndex)
{
	if (!PaceFrame(a_frameIndex) || simulationMarkerFrame == a_frameIndex) { return false; }
	simulationMarkerFrame = a_frameIndex;
	SetPCLMarker(sl::PCLMarker::eSimulationStart);
	return true;
}

void Streamline::EndSimulationFrame(uint32_t a_frameIndex)
{
	// A loading screen can render many frames while OnIdle is blocked on I/O.
	// Do not attribute the old simulation's end to its newer loading-frame token.
	if (markerFrameIndex != a_frameIndex || !frameToken) {
		logger::debug("[Reflex] Simulation {} interrupted by render frame {}; end marker omitted", a_frameIndex, markerFrameIndex);
		return;
	}
	SetPCLMarker(sl::PCLMarker::eSimulationEnd);
}

void Streamline::BeginRenderFrame(uint32_t a_frameIndex)
{
	if (!PaceFrame(a_frameIndex) || renderMarkerFrame == a_frameIndex) { return; }
	renderMarkerFrame = a_frameIndex;
	SetPCLMarker(sl::PCLMarker::eRenderSubmitStart);
}

void Streamline::RequestTemporalReset()
{
	// Published common constants are immutable for their token. A late request
	// is consumed by UpdateConstants on the next frame, never by republishing.
	temporalResetPending = true;
	directDLSSNR.RequestReset();
}

uint32_t Streamline::GetDLSSGPacingMultiplier() const
{
	if (!dlssgActive) { return 1; }
	// ActuallyPresented accumulates across state queries, not a per-frame multiplier.
	const auto generated = currentDLSSGMode == sl::DLSSGMode::eDynamic ? maxFramesToGenerate : currentDLSSGGeneratedFrames;
	return 1u + std::clamp(generated, 1u, 5u);
}

bool Streamline::ValidateConstantsForFrame(sl::FrameToken* a_frameToken)
{
	// Compare token IDs, not engine frame counters, to the published snapshot.
	if (!a_frameToken || !frameToken ||
		static_cast<uint32_t>(*a_frameToken) != static_cast<uint32_t>(*frameToken) ||
		constantsTokenIndex != static_cast<uint32_t>(*a_frameToken)) {
		logger::warn("[Streamline] Temporal evaluation rejected: constants/token mismatch");
		return false;
	}
	return true;
}

sl::FrameToken* Streamline::GetFrameTokenForFrame(uint32_t a_frameIndex)
{
	if (!initialized || !slGetNewFrameToken) {
		return nullptr;
	}

	if (frameToken && (markerFrameIndex == a_frameIndex || currentFrameTokenIndex == a_frameIndex)) {
		return frameToken;
	}

	sl::FrameToken* requestedFrameToken = nullptr;
	if (SL_FAILED(res, slGetNewFrameToken(requestedFrameToken, &a_frameIndex))) {
		logger::error("[Streamline] Could not get frame token for frame {}: {}", a_frameIndex, magic_enum::enum_name(res));
		return nullptr;
	}

	frameToken = requestedFrameToken;
	markerFrameIndex = a_frameIndex;
	currentFrameTokenIndex = requestedFrameToken ? static_cast<uint32_t>(*requestedFrameToken) : std::numeric_limits<uint32_t>::max();
	if (constantsFrameIndex != a_frameIndex) {
		logger::warn("[Streamline] Frame token {} reacquired without matching constants frame {}", a_frameIndex, constantsFrameIndex);
	}
	return requestedFrameToken;
}

void Streamline::SetPCLMarker(sl::PCLMarker a_marker, sl::FrameToken* a_frameToken)
{
	auto markerFrameToken = a_frameToken ? a_frameToken : frameToken;
	if (!markerFrameToken) {
		return;
	}

	if (!featurePCL || !slPCLSetMarker) {
		return;
	}

	if (SL_FAILED(res, slPCLSetMarker(a_marker, *markerFrameToken))) {
		logger::warn("[Streamline] PCL marker {} failed: {}", static_cast<uint32_t>(a_marker), magic_enum::enum_name(res));
	}
}

void Streamline::OnPCLStatsPing()
{
	if (!featurePCL || !slPCLSetMarker || !slGetNewFrameToken) {
		return;
	}

	static auto gameViewport = Util::State_GetSingleton();
	const auto nextFrameIndex = gameViewport ? gameViewport->frameCount + 1 : markerFrameIndex + 1;
	sl::FrameToken* pingFrameToken = nullptr;
	if (SL_FAILED(res, slGetNewFrameToken(pingFrameToken, &nextFrameIndex)) || !pingFrameToken) {
		logger::warn("[Streamline] Could not get PCL ping frame token {}: {}", nextFrameIndex, magic_enum::enum_name(res));
		return;
	}

	if (SL_FAILED(res, slPCLSetMarker(sl::PCLMarker::ePCLatencyPing, *pingFrameToken))) {
		logger::warn("[Streamline] PCL ping marker failed: {}", magic_enum::enum_name(res));
		return;
	}
	++pclPingCount;
}

void Streamline::UpdateReflex(uint a_reflexMode, bool a_forceEnabled)
{
	if (!featureReflex || !slReflexSetOptions) {
		return;
	}

	sl::ReflexMode mode = sl::ReflexMode::eOff;
	if (a_reflexMode == 2) {
		mode = sl::ReflexMode::eLowLatencyWithBoost;
	} else if (a_reflexMode == 1 || a_forceEnabled) {
		mode = sl::ReflexMode::eLowLatency;
	}

	// Official SL Reflex guidance says to leave marker-based optimization
	// disabled unless the Reflex team advises otherwise. PCL markers are still
	// emitted separately for latency reporting and DLSS-G frame matching.
	const bool useMarkersToOptimize = false;
	const uint32_t threadId = 0;
	if (currentReflexMode == mode && currentReflexUseMarkersToOptimize == useMarkersToOptimize && currentReflexThreadId == threadId) {
		return;
	}

	sl::ReflexOptions options{};
	options.mode = mode;
	options.useMarkersToOptimize = useMarkersToOptimize;
	options.idThread = threadId;

	if (SL_FAILED(result, slReflexSetOptions(options))) {
		logger::warn("[Streamline] Could not set Reflex mode {} markers={}: {}", static_cast<uint32_t>(mode), useMarkersToOptimize, magic_enum::enum_name(result));
		return;
	}

	currentReflexMode = mode;
	currentReflexUseMarkersToOptimize = useMarkersToOptimize;
	currentReflexThreadId = threadId;
}

bool Streamline::UpdateDLSSG(bool a_enabled, uint a_mode, uint a_numFramesToGenerate, bool a_dynamicMFGEnabled, uint a_dynamicMFGTargetFPS, float2 a_renderSize, float2 a_displaySize, DXGI_FORMAT a_colorFormat, DXGI_FORMAT a_motionVectorFormat, DXGI_FORMAT a_depthFormat, DXGI_FORMAT a_uiFormat)
{
	if (!featureDLSSG || !slDLSSGSetOptions) {
		dlssgActive = false;
		return false;
	}

	bool mfgUnlockReady = RTX40MFGUnlock::Ready();
	if (UsesD3D12() && RTX40MFGUnlock::AdaAdapterVerified() && !mfgUnlockReady) {
		static std::uint64_t lastUnlockRetryTick = 0;
		const auto now = GetTickCount64();
		if (now - lastUnlockRetryTick >= 1000) {
			lastUnlockRetryTick = now;
			mfgUnlockReady = RTX40MFGUnlock::PatchLoadedModules();
		}
	}
	const bool restrictAdaToNativeTwoX =
		RTX40MFGUnlock::AdaAdapterVerified() && !mfgUnlockReady;

	if (!dlssgStateKnown && slDLSSGGetState) {
		sl::DLSSGState state{};
		if (SL_SUCCEEDED(result, slDLSSGGetState(viewport, state, nullptr))) {
			maxFramesToGenerate = std::max<uint32_t>(1, state.numFramesToGenerateMax);
			dlssgVSyncSupported = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
			dynamicMFGSupported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
		}
		dlssgStateKnown = !restrictAdaToNativeTwoX;
	}
	if (restrictAdaToNativeTwoX) {
		maxFramesToGenerate = 1;
		dynamicMFGSupported = false;
	}
	if (RTX40MFGUnlock::AdaAdapterVerified()) {
		maxFramesToGenerate = std::min(maxFramesToGenerate, RTX40MFGUnlock::MaximumGeneratedFrames());
	}

	const bool hasSizes = a_renderSize.x > 0.0f && a_renderSize.y > 0.0f && a_displaySize.x > 0.0f && a_displaySize.y > 0.0f;
	// Old DLSS-G runtimes cannot honor application VSync. Prefer the user's
	// explicit tear-free policy over enabling an incompatible FG configuration.
	static bool loggedVSyncFallback = false;
	if (a_enabled && Upscaling::GetSingleton()->settings.vsyncMode == 2 && !dlssgVSyncSupported) {
		if (!loggedVSyncFallback) {
			logger::warn("[Presentation] DLSS-G runtime has no application VSync support; FG disabled while VSync On is selected");
		}
		loggedVSyncFallback = true;
		RequestDLSSGDisable();
		return true;
	}
	loggedVSyncFallback = false;
	sl::DLSSGMode mode = sl::DLSSGMode::eOff;
	if (a_enabled && hasSizes) {
		if (a_dynamicMFGEnabled || a_mode == 3) {
			mode = sl::DLSSGMode::eDynamic;
		} else if (a_mode == 2) {
			mode = sl::DLSSGMode::eAuto;
		} else {
			mode = sl::DLSSGMode::eOn;
		}
	}

	if (mode == sl::DLSSGMode::eDynamic && !dynamicMFGSupported) {
		if (!loggedDynamicMFGUnsupported) {
			logger::warn("[Streamline] Dynamic MFG requested but runtime reports unsupported; falling back to DLSS-G Auto mode");
			loggedDynamicMFGUnsupported = true;
		}
		mode = sl::DLSSGMode::eAuto;
	}

	const uint32_t renderWidth = hasSizes ? static_cast<uint32_t>(a_renderSize.x) : 0;
	const uint32_t renderHeight = hasSizes ? static_cast<uint32_t>(a_renderSize.y) : 0;
	const uint32_t displayWidth = hasSizes ? static_cast<uint32_t>(a_displaySize.x) : 0;
	const uint32_t displayHeight = hasSizes ? static_cast<uint32_t>(a_displaySize.y) : 0;
	const uint32_t generatedFrames = std::clamp<uint32_t>(a_numFramesToGenerate, 1, std::max<uint32_t>(1, maxFramesToGenerate));
	const auto outputLimit = Upscaling::GetSingleton()->settings.outputFPSLimit;
	const auto targetFPS = outputLimit ? (a_dynamicMFGTargetFPS ? std::min(a_dynamicMFGTargetFPS, outputLimit) : outputLimit) : a_dynamicMFGTargetFPS;
	const uint32_t dynamicTargetFPS = mode == sl::DLSSGMode::eDynamic ? targetFPS : 0;

	static uint32_t currentRenderWidth = 0;
	static uint32_t currentRenderHeight = 0;
	static uint32_t currentDisplayWidth = 0;
	static uint32_t currentDisplayHeight = 0;
	static DXGI_FORMAT currentColorFormat = DXGI_FORMAT_UNKNOWN;
	static DXGI_FORMAT currentMotionVectorFormat = DXGI_FORMAT_UNKNOWN;
	static DXGI_FORMAT currentDepthFormat = DXGI_FORMAT_UNKNOWN;
	static DXGI_FORMAT currentUIFormat = DXGI_FORMAT_UNKNOWN;

	if (mode == sl::DLSSGMode::eOff) {
		RequestDLSSGDisable();
		return true;
	}

	pendingDLSSGDisable = false;
	dlssgPresentSafetyFrames = 0;

	if (currentDLSSGMode == mode &&
		currentDLSSGGeneratedFrames == generatedFrames &&
		currentDLSSGDynamicTargetFPS == dynamicTargetFPS &&
		currentRenderWidth == renderWidth &&
		currentRenderHeight == renderHeight &&
		currentDisplayWidth == displayWidth &&
		currentDisplayHeight == displayHeight &&
		currentColorFormat == a_colorFormat &&
		currentMotionVectorFormat == a_motionVectorFormat &&
		currentDepthFormat == a_depthFormat &&
		currentUIFormat == a_uiFormat) {
		return true;
	}

	const auto hadConfiguredOptions = currentDLSSGMode != sl::DLSSGMode::eCount;
	sl::DLSSGOptions options{};
	options.mode = mode;
	options.numFramesToGenerate = generatedFrames;
	const bool hasUIBuffer = a_uiFormat != DXGI_FORMAT_UNKNOWN;
	options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
	if (hasUIBuffer) {
		options.flags |= sl::DLSSGFlags::eEnableFullscreenMenuDetection;
	}
	options.dynamicTargetFrameRate = static_cast<float>(dynamicTargetFPS);
	options.numBackBuffers = swapChainDesc.BufferCount ? swapChainDesc.BufferCount : 2;
	options.mvecDepthWidth = renderWidth;
	options.mvecDepthHeight = renderHeight;
	options.colorWidth = displayWidth;
	options.colorHeight = displayHeight;
	options.colorBufferFormat = static_cast<uint32_t>(a_colorFormat);
	options.mvecBufferFormat = static_cast<uint32_t>(a_motionVectorFormat);
	options.depthBufferFormat = static_cast<uint32_t>(a_depthFormat);
	options.hudLessBufferFormat = static_cast<uint32_t>(a_colorFormat);
	options.uiBufferFormat = static_cast<uint32_t>(a_uiFormat);
	// The current compositor tags HUD-less color and the composed backbuffer,
	// but no separate UI input. Recomposition/menu detection require that input.
	options.enableUserInterfaceRecomposition = hasUIBuffer ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	options.onErrorCallback = DLSSGAPIErrorCallback;

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		logger::warn("[Streamline] Could not set DLSS-G mode {}: {}", static_cast<uint32_t>(mode), magic_enum::enum_name(result));
		dlssgActive = false;
		return false;
	}

	currentDLSSGMode = mode;
	currentDLSSGGeneratedFrames = generatedFrames;
	currentDLSSGDynamicTargetFPS = dynamicTargetFPS;
	currentRenderWidth = renderWidth;
	currentRenderHeight = renderHeight;
	currentDisplayWidth = displayWidth;
	currentDisplayHeight = displayHeight;
	currentColorFormat = a_colorFormat;
	currentMotionVectorFormat = a_motionVectorFormat;
	currentDepthFormat = a_depthFormat;
	currentUIFormat = a_uiFormat;
	if (!dlssgActive) {
		lastDLSSGStateQueryFrame = std::numeric_limits<uint32_t>::max();
		lastDLSSGPresentMultiplier = 1.0;
	}
	dlssgActive = mode != sl::DLSSGMode::eOff;
	logger::info("[DLSS-G] options mode={} generated={} dynamicTarget={} render={}x{} output={}x{} uiInput={} uiRecomposition={}",
		static_cast<uint32_t>(mode), generatedFrames, dynamicTargetFPS, renderWidth, renderHeight, displayWidth, displayHeight,
		hasUIBuffer, options.enableUserInterfaceRecomposition == sl::Boolean::eTrue);
	if (hadConfiguredOptions && lastTemporalResetFrameIndex != constantsFrameIndex) {
		RequestTemporalReset();
	}

	return true;
}

void Streamline::RequestDLSSGDisable()
{
	if (!dlssgActive && (currentDLSSGMode == sl::DLSSGMode::eOff || currentDLSSGMode == sl::DLSSGMode::eCount)) {
		return;
	}
	if (pendingDLSSGDisable) {
		return;
	}

	pendingDLSSGDisable = true;
	RequestTemporalReset();
}

bool Streamline::DisableDLSSGNow()
{
	if (!featureDLSSG || !slDLSSGSetOptions) {
		return false;
	}

	sl::DLSSGOptions options{};
	options.mode = sl::DLSSGMode::eOff;
	options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
	options.onErrorCallback = DLSSGAPIErrorCallback;

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		logger::warn("[Streamline] Could not disable DLSS-G: {}", magic_enum::enum_name(result));
		return false;
	}

	currentDLSSGMode = sl::DLSSGMode::eOff;
	currentDLSSGGeneratedFrames = 0;
	currentDLSSGDynamicTargetFPS = 0;
	dlssgActive = false;
	return true;
}

void Streamline::ApplyPendingDLSSGDisable()
{
	if (!pendingDLSSGDisable) {
		return;
	}

	// Keep the request and active mode on failure so the next Present retries.
	if (DisableDLSSGNow()) {
		pendingDLSSGDisable = false;
		dlssgPresentSafetyFrames = 2;
	}
}

bool Streamline::NeedsDLSSGPresentSafety() const
{
	return dlssgActive || pendingDLSSGDisable || dlssgPresentSafetyFrames > 0;
}

void Streamline::OnDLSSGPresentComplete()
{
	if (!dlssgActive && !pendingDLSSGDisable && dlssgPresentSafetyFrames > 0) {
		--dlssgPresentSafetyFrames;
	}
}

void Streamline::TagDLSSGResources(ID3D11Texture2D* a_hudlessColor, ID3D11Texture2D* a_motionVectors, ID3D11Texture2D* a_depth, float2 a_renderSize, float2 a_displaySize)
{
	if (!dlssgActive || !frameToken || !slSetTagForFrame || !a_hudlessColor || !a_motionVectors || !a_depth) {
		return;
	}

	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	sl::Extent lowResExtent{ 0, 0, static_cast<uint32_t>(a_renderSize.x), static_cast<uint32_t>(a_renderSize.y) };
	sl::Extent fullExtent{ 0, 0, static_cast<uint32_t>(a_displaySize.x), static_cast<uint32_t>(a_displaySize.y) };

	sl::Resource hudless = { sl::ResourceType::eTex2d, a_hudlessColor, 0 };
	sl::Resource depth = { sl::ResourceType::eTex2d, a_depth, 0 };
	sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectors, 0 };

	sl::ResourceTag backbufferTag = { nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &fullExtent };
	sl::ResourceTag hudlessTag = { &hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };
	sl::ResourceTag depthTag = { &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };
	sl::ResourceTag mvecTag = { &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };

	sl::ResourceTag resourceTags[] = { backbufferTag, hudlessTag, depthTag, mvecTag };
	const auto tagResult = slSetTagForFrame(*frameToken, viewport, resourceTags, _countof(resourceTags), context);
	if (SL_FAILED(result, tagResult)) {
		logger::warn("[Streamline] Could not tag DLSS-G resources: {}", magic_enum::enum_name(result));
	}
}

void Streamline::TagDLSSGResources(ID3D12Resource* a_hudlessColor, ID3D12Resource* a_motionVectors, ID3D12Resource* a_depth, ID3D12Resource* a_uiColorAlpha, ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex, float2 a_renderSize, float2 a_displaySize)
{
	if (!dlssgActive || !slSetTagForFrame || !a_hudlessColor || !a_motionVectors || !a_depth || !a_commandList) {
		return;
	}

	auto* a_frameToken = GetFrameTokenForFrame(a_frameIndex);
	if (!a_frameToken) {
		return;
	}

	constexpr auto lifecycle = sl::ResourceLifecycle::eValidUntilPresent;

	sl::Extent lowResExtent{ 0, 0, static_cast<uint32_t>(a_renderSize.x), static_cast<uint32_t>(a_renderSize.y) };
	sl::Extent fullExtent{ 0, 0, static_cast<uint32_t>(a_displaySize.x), static_cast<uint32_t>(a_displaySize.y) };

	sl::Resource hudless = { sl::ResourceType::eTex2d, a_hudlessColor, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource depth = { sl::ResourceType::eTex2d, a_depth, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectors, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource uiColorAlpha = { sl::ResourceType::eTex2d, a_uiColorAlpha, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };

	sl::ResourceTag backbufferTag = { nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &fullExtent };
	sl::ResourceTag hudlessTag = { &hudless, sl::kBufferTypeHUDLessColor, lifecycle, &fullExtent };
	sl::ResourceTag depthTag = { &depth, sl::kBufferTypeDepth, lifecycle, &lowResExtent };
	sl::ResourceTag mvecTag = { &mvec, sl::kBufferTypeMotionVectors, lifecycle, &lowResExtent };
	sl::ResourceTag uiColorAlphaTag = { &uiColorAlpha, sl::kBufferTypeUIColorAndAlpha, lifecycle, &fullExtent };

	sl::ResourceTag resourceTags[] = { backbufferTag, hudlessTag, depthTag, mvecTag, uiColorAlphaTag };
	const auto numResourceTags = static_cast<uint32_t>(a_uiColorAlpha ? _countof(resourceTags) : _countof(resourceTags) - 1);
	const auto tagResult = slSetTagForFrame(*a_frameToken, viewport, resourceTags, numResourceTags, a_commandList);
	if (SL_FAILED(result, tagResult)) {
		logger::warn("[Streamline] Could not tag D3D12 DLSS-G resources: {}", magic_enum::enum_name(result));
	}
}

void Streamline::ClearDLSSGResourceTags(ID3D12GraphicsCommandList* a_commandList)
{
	if (!slSetTagForFrame) {
		return;
	}

	static auto gameViewport = Util::State_GetSingleton();
	if (!frameToken) {
		return;
	}

	const auto* dx12 = DX12SwapChain::GetSingleton();
	const sl::Extent fullExtent{
		0,
		0,
		dx12->swapChainDesc.Width ? dx12->swapChainDesc.Width : static_cast<uint32_t>(gameViewport->screenWidth),
		dx12->swapChainDesc.Height ? dx12->swapChainDesc.Height : static_cast<uint32_t>(gameViewport->screenHeight)
	};
	if (fullExtent.width == 0 || fullExtent.height == 0) {
		// Do not submit an optional 0x0 backbuffer extent while the proxy
		// swapchain is between creation and its first ResizeBuffers.
		return;
	}

	sl::ResourceTag backbufferTag = { nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &fullExtent };
	sl::ResourceTag hudlessTag = { nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle{} };
	sl::ResourceTag depthTag = { nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle{} };
	sl::ResourceTag mvecTag = { nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle{} };
	sl::ResourceTag uiColorAlphaTag = { nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle{} };
	sl::ResourceTag uiAlphaTag = { nullptr, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle{} };

	sl::ResourceTag resourceTags[] = {
		backbufferTag,
		hudlessTag,
		depthTag,
		mvecTag,
		uiColorAlphaTag,
		uiAlphaTag
	};
	const auto tagResult = slSetTagForFrame(*frameToken, viewport, resourceTags, _countof(resourceTags), a_commandList);
	if (SL_FAILED(result, tagResult)) {
		logger::warn("[Streamline] Could not clear D3D12 DLSS-G resource tags: {}", magic_enum::enum_name(result));
		return;
	}

	presentFrameToken = frameToken;
	presentFrameTokenIndex = currentFrameTokenIndex;
}

void Streamline::SetPresentFrameIndex(uint32_t a_frameIndex)
{
	presentFrameToken = GetFrameTokenForFrame(a_frameIndex);
	presentFrameTokenIndex = presentFrameToken ? static_cast<uint32_t>(*presentFrameToken) : std::numeric_limits<uint32_t>::max();
}

void Streamline::OnPresentStart()
{
	auto markerFrameToken = presentFrameToken ? presentFrameToken : frameToken;
	SetPCLMarker(sl::PCLMarker::eRenderSubmitEnd, markerFrameToken);
	SetPCLMarker(sl::PCLMarker::ePresentStart, markerFrameToken);
}

void Streamline::OnPresentEnd(HRESULT, bool a_queryState)
{
	auto markerFrameToken = presentFrameToken ? presentFrameToken : frameToken;
	lastPresentFrameTokenIndex = presentFrameToken ? presentFrameTokenIndex : currentFrameTokenIndex;
	SetPCLMarker(sl::PCLMarker::ePresentEnd, markerFrameToken);

	if (a_queryState) {
		QueryDLSSGState("post-present");
	}

	presentFrameToken = nullptr;
	presentFrameTokenIndex = std::numeric_limits<uint32_t>::max();
}

void Streamline::QueryDLSSGState(std::string_view a_phase)
{
	if (!featureDLSSG || !slDLSSGGetState) {
		return;
	}

	static auto gameViewport = Util::State_GetSingleton();
	const auto currentFrame = gameViewport ? gameViewport->frameCount : lastDLSSGStateQueryFrame + 1;
	if (dlssgActive &&
		lastDLSSGStateQueryFrame != std::numeric_limits<uint32_t>::max() &&
		currentFrame - lastDLSSGStateQueryFrame < kDLSSGStateQueryInterval) {
		return;
	}
	lastDLSSGStateQueryFrame = currentFrame;

	sl::DLSSGState state{};
	if (SL_FAILED(result, slDLSSGGetState(viewport, state, nullptr))) {
		logger::warn("[Streamline] Could not query DLSS-G state: {}", magic_enum::enum_name(result));
		return;
	}
	lastDLSSGPresentMultiplier = static_cast<double>(state.numFramesActuallyPresented);
	dlssgVSyncSupported = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue;

	maxFramesToGenerate = std::max<uint32_t>(1, state.numFramesToGenerateMax);
	if (RTX40MFGUnlock::AdaAdapterVerified()) {
		maxFramesToGenerate = std::min(maxFramesToGenerate, RTX40MFGUnlock::MaximumGeneratedFrames());
	}
	dynamicMFGSupported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
	dlssgStateKnown = true;

	const auto status = static_cast<uint32_t>(state.status);
	if (lastDLSSGStatus != status || lastDLSSGPresentedFrames != state.numFramesActuallyPresented) {
		logger::debug(
			"[Streamline] DLSS-G state phase={} status={}({}) requested={} actuallyPresented={} max={} dynamicMFG={} active={}",
			a_phase,
			status,
			DLSSGStatusFlags(state.status),
			currentDLSSGGeneratedFrames,
			state.numFramesActuallyPresented,
			state.numFramesToGenerateMax,
			state.bIsDynamicMFGSupported == sl::Boolean::eTrue,
			dlssgActive);
		lastDLSSGStatus = status;
		lastDLSSGPresentedFrames = state.numFramesActuallyPresented;
	}
	if (dlssgActive && state.status != sl::DLSSGStatus::eOk && slDLSSGSetOptions) {
		logger::warn("[Streamline] DLSS-G disable requested due to runtime status {}", status);
		RequestDLSSGDisable();
	}
}

float Streamline::GetReflexLatencyMs()
{
	pclLatencyReportAvailable = false;
	if (!featureReflex || !slReflexGetState) {
		return 0.0f;
	}

	sl::ReflexState state{};
	if (SL_FAILED(result, slReflexGetState(state)) || !state.latencyReportAvailable) {
		return 0.0f;
	}
	pclLatencyReportAvailable = true;

	// InputSample is deprecated in PCL. CPU Present end also excludes queued
	// GPU work. Report simulation-start to GPU-end instead, without claiming
	// peripheral/scanout/display latency. Require a completed simulation scope.
	const sl::ReflexReport* latest = nullptr;
	for (const auto& report : state.frameReport) {
		if (report.frameID == 0 || report.simStartTime == 0 ||
			report.simEndTime < report.simStartTime || report.gpuRenderEndTime <= report.simStartTime) {
			continue;
		}
		if (!latest || report.frameID > latest->frameID) {
			latest = &report;
		}
	}
	if (latest) {
		const auto latencyUs = latest->gpuRenderEndTime - latest->simStartTime;
		return static_cast<float>(static_cast<double>(latencyUs) / 1000.0);
	}

	return 0.0f;
}

void Streamline::ResetOptionCaches()
{
	RequestTemporalReset();
	srInputHistoryValid = false;
	srInputNRPassCount = 0;
	currentD3D12DLSSOptionsValid = false;
	currentD3D12DLSSMode = sl::DLSSMode::eOff;
	currentD3D12DLSSOutputWidth = 0;
	currentD3D12DLSSOutputHeight = 0;
	currentD3D12DLSSModelPreset = std::numeric_limits<uint>::max();
	currentD3D12DLSSNROptionsValid = false;
	currentD3D12DLSSNROptions = {};
	currentNISOptionsValid = false;
	currentNISSharpness = -1.0f;
}

bool Streamline::EnsureD3D12DLSSOptions(sl::DLSSMode a_mode, uint32_t a_outputWidth, uint32_t a_outputHeight, uint a_dlssModelPreset)
{
	if (currentD3D12DLSSOptionsValid &&
		currentD3D12DLSSMode == a_mode &&
		currentD3D12DLSSOutputWidth == a_outputWidth &&
		currentD3D12DLSSOutputHeight == a_outputHeight &&
		currentD3D12DLSSModelPreset == a_dlssModelPreset) {
		return true;
	}

	const auto hadValidOptions = currentD3D12DLSSOptionsValid;
	sl::DLSSOptions dlssOptions{};
	dlssOptions.mode = a_mode;
	dlssOptions.outputWidth = a_outputWidth;
	dlssOptions.outputHeight = a_outputHeight;
	dlssOptions.colorBuffersHDR = sl::Boolean::eFalse;
	// Color is post-imagespace LDR, not the engine's pre-tonemap HDR buffer.
	dlssOptions.preExposure = 1.0f;
	dlssOptions.exposureScale = 1.0f;
	dlssOptions.useAutoExposure = sl::Boolean::eTrue;
	ApplyDLSSModelPreset(dlssOptions, a_dlssModelPreset);

	if (SL_FAILED(result, slDLSSSetOptions(viewport, dlssOptions))) {
		logger::warn("[Streamline] Could not set D3D12 DLSS options: {}", magic_enum::enum_name(result));
		return false;
	}

	currentD3D12DLSSOptionsValid = true;
	currentD3D12DLSSMode = a_mode;
	currentD3D12DLSSOutputWidth = a_outputWidth;
	currentD3D12DLSSOutputHeight = a_outputHeight;
	currentD3D12DLSSModelPreset = a_dlssModelPreset;
	if (hadValidOptions && lastTemporalResetFrameIndex != constantsFrameIndex) {
		RequestTemporalReset();
	}
	return true;
}

bool Streamline::EnsureD3D12DLSSNROptions(const sl::DLSSNROptions& a_options)
{
	auto options = a_options;
	options.mode = sl::DLSSNRMode::eOn;
	options.performanceMode = nvngx::dlss_nr::kNativePerformanceMode;
	if (currentD3D12DLSSNROptionsValid && SameDLSSNROptions(currentD3D12DLSSNROptions, options)) {
		return true;
	}

	const auto hadValidOptions = currentD3D12DLSSNROptionsValid;
	if (SL_FAILED(result, slDLSSNRSetOptions(viewport, options))) {
		if (!loggedDLSSNRFallback) {
			logger::warn(
				"[Streamline] Could not set D3D12 DLSS-NR options: {} performanceMode={} preset={} style={}",
				magic_enum::enum_name(result),
				options.performanceMode,
				options.preset,
				options.style);
		}
		return false;
	}

	currentD3D12DLSSNROptionsValid = true;
	currentD3D12DLSSNROptions = options;
	if (hadValidOptions && lastTemporalResetFrameIndex != constantsFrameIndex) {
		RequestTemporalReset();
	}
	return true;
}

bool Streamline::EnsureNISOptions(float a_sharpness, std::string_view a_logContext)
{
	if (currentNISOptionsValid && currentNISSharpness == a_sharpness) {
		return true;
	}

	sl::NISOptions nisOptions{};
	nisOptions.mode = sl::NISMode::eSharpen;
	nisOptions.hdrMode = sl::NISHDR::eNone;
	nisOptions.sharpness = a_sharpness;
	if (SL_FAILED(result, slNISSetOptions(viewport, nisOptions))) {
		logger::warn("[Streamline] Could not set {} NIS options: {}", a_logContext, magic_enum::enum_name(result));
		return false;
	}

	currentNISOptionsValid = true;
	currentNISSharpness = a_sharpness;
	return true;
}

bool Streamline::ApplyNISSharpen(ID3D11Resource* a_inputColor, ID3D11Resource* a_outputColor, ID3D11DeviceContext* a_context, sl::FrameToken* a_frameToken, float2 a_displaySize, float a_sharpness)
{
	const auto sharpness = std::clamp(a_sharpness, 0.0f, 1.0f);
	if (sharpness <= 0.0f || !featureNIS || !slNISSetOptions || !slEvaluateFeature || !slSetTagForFrame || !a_inputColor || !a_outputColor || !a_context || !a_frameToken) {
		return false;
	}

	if (!EnsureNISOptions(sharpness, "NIS sharpen")) {
		return false;
	}

	sl::Extent fullExtent{ 0, 0, static_cast<uint32_t>(a_displaySize.x), static_cast<uint32_t>(a_displaySize.y) };
	sl::Resource colorIn = { sl::ResourceType::eTex2d, a_inputColor, 0 };
	sl::Resource colorOut = { sl::ResourceType::eTex2d, a_outputColor, 0 };
	sl::ResourceTag resourceTags[] = {
		{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent },
		{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent }
	};
	if (SL_FAILED(result, slSetTagForFrame(*a_frameToken, viewport, resourceTags, _countof(resourceTags), a_context))) {
		logger::warn("[Streamline] Could not tag NIS sharpen resources: {}", magic_enum::enum_name(result));
		return false;
	}

	sl::ViewportHandle view(viewport);
	const sl::BaseStructure* inputs[] = { &view };
	if (SL_FAILED(result, slEvaluateFeature(sl::kFeatureNIS, *a_frameToken, inputs, _countof(inputs), a_context))) {
		logger::warn("[Streamline] NIS sharpen evaluate failed: {}", magic_enum::enum_name(result));
		return false;
	}

	nisResourcesUsed = true;
	return true;
}

bool Streamline::ApplyNISSharpenD3D12(ID3D12Resource* a_inputColor, ID3D12Resource* a_outputColor, ID3D12GraphicsCommandList* a_commandList, sl::FrameToken* a_frameToken, float2 a_displaySize, float a_sharpness)
{
	const auto sharpness = std::clamp(a_sharpness, 0.0f, 1.0f);
	if (sharpness <= 0.0f || !featureNIS || !slNISSetOptions || !slEvaluateFeature || !a_inputColor || !a_outputColor || !a_commandList || !a_frameToken) {
		return false;
	}

	if (!EnsureNISOptions(sharpness, "D3D12 NIS sharpen")) {
		return false;
	}

	sl::Extent fullExtent{ 0, 0, static_cast<uint32_t>(a_displaySize.x), static_cast<uint32_t>(a_displaySize.y) };
	sl::Resource colorIn = { sl::ResourceType::eTex2d, a_inputColor, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource colorOut = { sl::ResourceType::eTex2d, a_outputColor, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::ResourceTag resourceTags[] = {
		{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &fullExtent },
		{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &fullExtent }
	};
	// These slot-owned colors are consumed on this list. Local tags avoid a
	// volatile input clone and do not replace SR's or FG's global tags.
	sl::ViewportHandle view(viewport);
	const sl::BaseStructure* inputs[] = { &view, &resourceTags[0], &resourceTags[1] };
	if (SL_FAILED(result, slEvaluateFeature(sl::kFeatureNIS, *a_frameToken, inputs, _countof(inputs), a_commandList))) {
		logger::warn("[Streamline] D3D12 NIS sharpen evaluate failed: {}", magic_enum::enum_name(result));
		return false;
	}

	nisResourcesUsed = true;
	return true;
}

bool Streamline::UpscaleD3D12(ID3D12Resource* a_color, ID3D12Resource* a_outputColor, ID3D12Resource* a_sharpenedOutput, ID3D12Resource* a_motionVectors, ID3D12Resource* a_depth, ID3D12Resource* a_animatedTextureMask, ID3D12GraphicsCommandList* a_commandList, sl::FrameToken* a_frameToken, float2 a_renderSize, float2 a_displaySize, DXGI_FORMAT a_colorFormat, DXGI_FORMAT a_motionVectorFormat, DXGI_FORMAT a_depthFormat, uint a_qualityMode, float a_sharpness, uint a_dlssModelPreset, uint a_dlssNRPassCount, ID3D12Resource* a_nrMotionVectors, float2 a_nrJitterDelta, bool a_nrAfterSR, ID3D12Resource* a_nrDepth, const sl::DLSSNROptions& a_dlssNROptions, bool* a_sharpened)
{
	if (a_sharpened) {
		*a_sharpened = false;
	}

	if (!slEvaluateFeature || !a_color || !a_outputColor || !a_motionVectors || !a_depth || !a_commandList || !a_frameToken) {
		logger::warn(
			"[Streamline] D3D12 upscaling unavailable evaluate={} color={} output={} mvec={} depth={} commandList={} frameToken={}",
			static_cast<bool>(slEvaluateFeature),
			static_cast<void*>(a_color),
			static_cast<void*>(a_outputColor),
			static_cast<void*>(a_motionVectors),
			static_cast<void*>(a_depth),
			static_cast<void*>(a_commandList),
			static_cast<void*>(a_frameToken));
		return false;
	}

	const auto validSize = [](float2 size) {
		return std::isfinite(size.x) && std::isfinite(size.y) &&
			size.x >= 1.0f && size.y >= 1.0f && size.x <= 16384.0f && size.y <= 16384.0f;
	};
	const auto covers = [](ID3D12Resource* resource, float2 size, bool output) {
		if (!resource) { return false; }
		const auto desc = resource->GetDesc();
		return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
			desc.SampleDesc.Count == 1 && desc.DepthOrArraySize == 1 &&
			desc.Width >= static_cast<uint32_t>(size.x) && desc.Height >= static_cast<uint32_t>(size.y) &&
			(!output || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
	};
	if (!validSize(a_renderSize) || !validSize(a_displaySize) ||
		a_renderSize.x > a_displaySize.x || a_renderSize.y > a_displaySize.y ||
		(a_qualityMode == 0 && (a_renderSize.x != a_displaySize.x || a_renderSize.y != a_displaySize.y)) ||
		!covers(a_color, a_renderSize, false) || !covers(a_depth, a_renderSize, false) ||
		!covers(a_motionVectors, a_renderSize, false) || !covers(a_outputColor, a_displaySize, true) ||
		(a_sharpenedOutput && !covers(a_sharpenedOutput, a_displaySize, true)) ||
		a_color == a_outputColor || (a_sharpenedOutput &&
			(a_sharpenedOutput == a_color || a_sharpenedOutput == a_outputColor))) {
		logger::warn("[Streamline] DLSS resource/extent contract rejected render={}x{} display={}x{} quality={}",
			a_renderSize.x, a_renderSize.y, a_displaySize.x, a_displaySize.y, a_qualityMode);
		RequestTemporalReset();
		return false;
	}

	sl::DLSSMode dlssMode;
	switch (a_qualityMode) {
	case 1:
		dlssMode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssMode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssMode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssMode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssMode = sl::DLSSMode::eDLAA;
		break;
	}

	currentDLSSQualityMode = a_qualityMode;
	currentDLSSModelPreset = a_dlssModelPreset;

	sl::Extent lowResExtent{ 0, 0, static_cast<uint32_t>(a_renderSize.x), static_cast<uint32_t>(a_renderSize.y) };
	sl::Extent fullExtent{ 0, 0, static_cast<uint32_t>(a_displaySize.x), static_cast<uint32_t>(a_displaySize.y) };

	sl::Resource depth = { sl::ResourceType::eTex2d, a_depth, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectors, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource animatedTexture = { sl::ResourceType::eTex2d, a_animatedTextureMask, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };

	const auto evaluate = [&](
		bool a_useDLSSNR,
		ID3D12Resource* a_featureColor,
		ID3D12Resource* a_featureOutput,
		const sl::Extent& a_colorExtent,
		const sl::Extent& a_outputExtent) {
		const auto featureName = a_useDLSSNR ? "DLSS-NR" : "DLSS";
		const auto feature = a_useDLSSNR ? sl::kFeatureDLSS_NR : sl::kFeatureDLSS;
		const bool featureAvailable = a_useDLSSNR ? featureDLSSNR : featureDLSS;
		const bool setOptionsAvailable = a_useDLSSNR ? static_cast<bool>(slDLSSNRSetOptions) : static_cast<bool>(slDLSSSetOptions);
		if (!featureAvailable || !setOptionsAvailable || !a_featureColor || !a_featureOutput) {
			if (!a_useDLSSNR || !loggedDLSSNRFallback) {
				logger::warn(
					"[Streamline] D3D12 {} unavailable feature={} setOptions={} color={} output={}",
					featureName,
					featureAvailable,
					setOptionsAvailable,
					static_cast<void*>(a_featureColor),
					static_cast<void*>(a_featureOutput));
			}
			return false;
		}

		if (a_useDLSSNR) {
			if (!EnsureD3D12DLSSNROptions(a_dlssNROptions)) {
				return false;
			}
		} else {
			if (!EnsureD3D12DLSSOptions(dlssMode, fullExtent.width, fullExtent.height, a_dlssModelPreset)) {
				return false;
			}
		}

		const auto inputColorType = a_useDLSSNR ? sl::kBufferTypeUpliftInputColor : sl::kBufferTypeScalingInputColor;
		if (!ValidateConstantsForFrame(a_frameToken)) {
			return false;
		}
		const auto outputColorType = a_useDLSSNR ? sl::kBufferTypeUpliftOutputColor : sl::kBufferTypeScalingOutputColor;
		sl::Resource colorIn = { sl::ResourceType::eTex2d, a_featureColor, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
		sl::Resource colorOut = { sl::ResourceType::eTex2d, a_featureOutput, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
		sl::ResourceTag resourceTags[] = {
			{ &colorIn, inputColorType, sl::ResourceLifecycle::eValidUntilEvaluate, &a_colorExtent },
			{ &colorOut, outputColorType, sl::ResourceLifecycle::eValidUntilEvaluate, &a_outputExtent },
			{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent },
			{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent },
			{ &animatedTexture, sl::kBufferTypeAnimatedTextureHint, sl::ResourceLifecycle::eValidUntilEvaluate, &lowResExtent }
		};
		// A Pipboy history hint is not an alpha/transparency classification.
		// Keep this local to SR; NR and FG must not inherit this resource.
		const auto numResourceTags = static_cast<uint32_t>(a_useDLSSNR || !a_animatedTextureMask ? _countof(resourceTags) - 1 : _countof(resourceTags));
		// Evaluation borrows these resources on the same command list. Global
		// eOnlyValidNow tags make SL allocate/copy color through its resource
		// pool, including the native-sized NR intermediate in Before SR.
		// Slot fences already protect GPU use. Keep SR guides local so the
		// separate FG tags remain owned by the D3D12 TagDLSSGResources overload.
		sl::ViewportHandle view(viewport);
		const sl::BaseStructure* inputs[] = {
			&view, &resourceTags[0], &resourceTags[1], &resourceTags[2],
			&resourceTags[3], &resourceTags[4]
		};
		if (SL_FAILED(result, slEvaluateFeature(feature, *a_frameToken, inputs, numResourceTags + 1, a_commandList))) {
			if (!a_useDLSSNR || !loggedDLSSNRFallback) {
				logger::warn(
					"[Streamline] D3D12 {} evaluate failed: {} token={} input={}x{} output={}x{} color={} outputResource={} mvec={} depth={} transparency={} formats color={} mvec={} depth={}",
					featureName,
					magic_enum::enum_name(result),
					static_cast<uint32_t>(*a_frameToken),
					a_colorExtent.width,
					a_colorExtent.height,
					a_outputExtent.width,
					a_outputExtent.height,
					static_cast<void*>(a_featureColor),
					static_cast<void*>(a_featureOutput),
					static_cast<void*>(a_motionVectors),
					static_cast<void*>(a_depth),
					static_cast<void*>(a_animatedTextureMask),
					magic_enum::enum_name(a_colorFormat),
					magic_enum::enum_name(a_motionVectorFormat),
					magic_enum::enum_name(a_depthFormat));
			}
			return false;
		}

		return true;
	};

	const auto& nrExtent = a_nrAfterSR ? fullExtent : lowResExtent;
	const auto evaluateDLSSNR = [&]() {
		// Prefer NVIDIA's Streamline plugin when it survived discovery. When
		// discovery rejected the private runtime, use the feature DLL directly;
		// do not retry a failed evaluation through the other NR backend.
		// After SR uses direct NGX so NR can consume resolved guides without
		// changing SR/FG's immutable Streamline common constants or tags.
		if (!a_nrAfterSR && featureDLSSNR && slDLSSNRSetOptions) {
			if (a_dlssNRPassCount > 1 && !loggedNativeDLSSNRMultipass) {
				logger::info("[Streamline] DLSS-NR multipass is only available on the direct-NGX path; using one Streamline NR pass");
				loggedNativeDLSSNRMultipass = true;
			} else if (a_dlssNRPassCount <= 1) {
				loggedNativeDLSSNRMultipass = false;
			}
			return evaluate(true, a_color, a_outputColor, lowResExtent, lowResExtent);
		}
		loggedNativeDLSSNRMultipass = false;

		nvngx::dlss_nr::D3D12EvaluationParameters parameters{};
		// Never silently feed raw UV motion to NR if conversion failed.
		if (!covers(a_nrMotionVectors, a_nrAfterSR ? a_displaySize : a_renderSize, false) ||
			(a_nrAfterSR && !covers(a_nrDepth, a_displaySize, false))) { return false; }
		parameters.color = a_nrAfterSR ? a_outputColor : a_color;
		parameters.output = a_nrAfterSR ? a_sharpenedOutput : a_outputColor;
		parameters.motionVectors = a_nrMotionVectors;
		parameters.depth = a_nrAfterSR ? a_nrDepth : a_depth;
		parameters.inputWidth = nrExtent.width;
		parameters.inputHeight = nrExtent.height;
		parameters.outputWidth = nrExtent.width;
		parameters.outputHeight = nrExtent.height;
		parameters.guideWidth = nrExtent.width;
		parameters.guideHeight = nrExtent.height;
		// The NR-only texture already contains pixel displacement and the
		// sample jitter delta before SR only. Do not scale it a second time.
		parameters.motionVectorScaleX = 1.0f;
		parameters.motionVectorScaleY = 1.0f;
		parameters.depthInverted = false;
		parameters.passCount = std::clamp(a_dlssNRPassCount, 1u, 3u);
		parameters.outputFormat = a_colorFormat;
		if (!ValidateConstantsForFrame(a_frameToken)) {
			return false;
		}
		parameters.reset = lastTemporalResetFrameIndex == constantsFrameIndex;
		parameters.options.performanceMode = nvngx::dlss_nr::kNativePerformanceMode;
		parameters.options.preset = a_dlssNROptions.preset;
		parameters.options.style = a_dlssNROptions.style;
		parameters.options.intensity = a_dlssNROptions.intensity;
		parameters.options.localToneStrength = a_dlssNROptions.localToneStrength;
		parameters.options.localStructureStrength = a_dlssNROptions.localStructureStrength;
		parameters.options.skinStructureStrength = a_dlssNROptions.skinStructureStrength;
		parameters.options.useAutoMask = a_dlssNROptions.useAutoMask == sl::Boolean::eTrue;
		return directDLSSNR.Evaluate(a_commandList, parameters);
	};

	const bool dlssNRRequested = !dlssNRSuspended && a_dlssNROptions.mode == sl::DLSSNRMode::eOn;

#ifdef UPSCALING_NR_CAPTURE
	const bool capture = NRDiagnosticCapture::Requested() && dlssNRRequested && a_sharpenedOutput &&
		ValidateConstantsForFrame(a_frameToken) && NRDiagnosticCapture::Begin(a_commandList, constantsFrameIndex,
			std::format("\"engine_frame\":{},\"token\":{},\"render\":[{},{}],\"display\":[{},{}],\"jitter\":[{},{}],\"common_reset\":{},\"direct_ngx\":{},\"requested_passes\":{},\"quality\":{},\"style\":{},\"intensity\":{},\"local_tone\":{},\"local_structure\":{},\"skin_structure\":{},\"auto_mask\":{}",
				constantsFrameIndex, static_cast<uint32_t>(*a_frameToken), lowResExtent.width, lowResExtent.height,
				fullExtent.width, fullExtent.height, constantsJitter.x, constantsJitter.y,
				lastTemporalResetFrameIndex == constantsFrameIndex, a_nrAfterSR || !(featureDLSSNR && slDLSSNRSetOptions),
				a_dlssNRPassCount, a_qualityMode, a_dlssNROptions.style, a_dlssNROptions.intensity,
				a_dlssNROptions.localToneStrength, a_dlssNROptions.localStructureStrength,
				a_dlssNROptions.skinStructureStrength, a_dlssNROptions.useAutoMask == sl::Boolean::eTrue));
	if (capture) {
		NRDiagnosticCapture::Copy(a_commandList, "input", a_color, lowResExtent.width, lowResExtent.height);
		NRDiagnosticCapture::Copy(a_commandList, "mv", a_motionVectors, lowResExtent.width, lowResExtent.height);
		NRDiagnosticCapture::Copy(a_commandList, "depth", a_depth, lowResExtent.width, lowResExtent.height);
		// ReShade consumes the separate depth snapshot captured from the engine
		// D3D11 depth target. Capture it beside the NR guides so the raw raster
		// depth and the jitter-corrected sample can be compared frame by frame.
		const auto reshadeInfo = Upscaling::GetSingleton()->GetCurrentSharedDepthInfo();
		NRDiagnosticCapture::Annotate(a_commandList,
			std::format("\"reshade_depth_available\":{}", reshadeInfo.resource ? "true" : "false"));
		if (auto* reshadeDepth = reshadeInfo.resource) {
			const auto reshadeDesc = reshadeDepth->GetDesc();
			NRDiagnosticCapture::Copy(a_commandList, "reshade_depth", reshadeDepth,
				static_cast<UINT>(reshadeDesc.Width), static_cast<UINT>(reshadeDesc.Height));
			NRDiagnosticCapture::Annotate(a_commandList,
					std::format("\"reshade_depth_physical_source_extent\":[{},{}],\"reshade_depth_sample_extent\":[{},{}],\"reshade_depth_output_extent\":[{},{}],\"reshade_depth_engine_jitter\":[{},{}],\"reshade_depth_streamline_jitter\":[{},{}],\"reshade_depth_sampling\":\"bilinear((id+0.5)*sample_extent/output_extent-engine_jitter)\"",
					reshadeInfo.physicalSourceWidth, reshadeInfo.physicalSourceHeight,
					reshadeInfo.sampleWidth, reshadeInfo.sampleHeight,
					reshadeInfo.outputWidth, reshadeInfo.outputHeight,
					reshadeInfo.engineJitter.x, reshadeInfo.engineJitter.y,
					-reshadeInfo.engineJitter.x, -reshadeInfo.engineJitter.y));
		}
		if ((a_nrAfterSR || !(featureDLSSNR && slDLSSNRSetOptions)) && a_nrMotionVectors) {
			NRDiagnosticCapture::Copy(a_commandList, "nr_mv", a_nrMotionVectors, nrExtent.width, nrExtent.height);
			if (a_nrAfterSR && a_nrDepth) {
				NRDiagnosticCapture::Copy(a_commandList, "nr_depth", a_nrDepth, nrExtent.width, nrExtent.height);
			}
			NRDiagnosticCapture::Annotate(a_commandList,
				std::format("\"nr_position\":\"{}\",\"nr_mv_encoding\":\"{}\",\"nr_jitter_delta\":[{},{}]",
					a_nrAfterSR ? "after_sr" : "before_sr",
					a_nrAfterSR ? "display_pixels_without_jitter" : "pixels_with_jitter", a_nrJitterDelta.x, a_nrJitterDelta.y));
			NRDiagnosticCapture::Annotate(a_commandList,
				std::format("\"nr_guide_extent\":[{},{}],\"nr_guide_sample_offset\":[{},{}],\"nr_guide_sampling\":\"point\"",
					nrExtent.width, nrExtent.height,
					a_nrAfterSR ? constantsJitter.x : 0.0f, a_nrAfterSR ? constantsJitter.y : 0.0f));
		}
	}
#endif
	// The jitter delta is capture metadata; NR consumes the corrected texture.
	(void)a_nrJitterDelta;
	if (a_nrAfterSR) {
		// SR always sees original color in this mode, including NR failure.
		if (srInputHistoryValid && srInputNRPassCount != 0) { RequestTemporalReset(); }
		if (!evaluate(false, a_color, a_outputColor, lowResExtent, fullExtent)) {
#ifdef UPSCALING_NR_CAPTURE
			if (capture) { NRDiagnosticCapture::Finish(a_commandList, false, false, 0); }
#endif
			srInputHistoryValid = false;
			RequestTemporalReset();
			return false;
		}
#ifdef UPSCALING_NR_CAPTURE
		if (capture) { NRDiagnosticCapture::Copy(a_commandList, "sr", a_outputColor, fullExtent.width, fullExtent.height); }
#endif
		srInputHistoryValid = true;
		srInputNRPassCount = 0;
		if (dlssNRRequested && a_sharpenedOutput) {
			auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(a_outputColor);
			a_commandList->ResourceBarrier(1, &barrier);
			if (evaluateDLSSNR()) {
#ifdef UPSCALING_NR_CAPTURE
				if (capture) {
					NRDiagnosticCapture::Copy(a_commandList, "nr", a_sharpenedOutput, fullExtent.width, fullExtent.height);
					NRDiagnosticCapture::Finish(a_commandList, true, true, directDLSSNR.GetLastEvaluatedPassCount());
				}
#endif
				loggedDLSSNRFallback = false;
				if (a_sharpened) { *a_sharpened = true; }
				return true;
			}
			if (!loggedDLSSNRFallback) {
				logger::warn("[Streamline] After-SR DLSS-NR failed; preserving SR output");
			}
			loggedDLSSNRFallback = true;
			directDLSSNR.RequestReset();
		}
#ifdef UPSCALING_NR_CAPTURE
		if (capture) { NRDiagnosticCapture::Finish(a_commandList, false, true, 0); }
#endif
		// Same sharpening fallback as Before SR. Failed NR output is never used.
		if (a_sharpness > 0.0f && a_sharpenedOutput &&
			ApplyNISSharpenD3D12(a_outputColor, a_sharpenedOutput, a_commandList, a_frameToken, a_displaySize, a_sharpness)) {
			if (a_sharpened) { *a_sharpened = true; }
		}
		return true;
	}

	bool neuralRendered = false;
	uint32_t evaluatedNRPassCount = 0;
	if (dlssNRRequested && a_sharpenedOutput) {
		neuralRendered = evaluateDLSSNR();
		if (neuralRendered) {
			evaluatedNRPassCount = featureDLSSNR && slDLSSNRSetOptions ? 1u : directDLSSNR.GetLastEvaluatedPassCount();
			// The signed 310.8 NR runtime is a 1:1 pass. Order its UAV writes before
			// the same top-left subrect is consumed as the DLSS SR color input.
			auto nrToSRBarrier = CD3DX12_RESOURCE_BARRIER::UAV(a_outputColor);
			a_commandList->ResourceBarrier(1, &nrToSRBarrier);
#ifdef UPSCALING_NR_CAPTURE
			if (capture) { NRDiagnosticCapture::Copy(a_commandList, "nr", a_outputColor, lowResExtent.width, lowResExtent.height); }
#endif
		} else {
			if (!loggedDLSSNRFallback) {
				logger::warn("[Streamline] D3D12 DLSS-NR failed; running DLSS SR from the original color input");
			}
			loggedDLSSNRFallback = true;
		}
	} else {
		// Retain the direct feature while NR is off. Releasing it here can race
		// evaluations from earlier frames; normal upscaler teardown releases it
		// after DX12SwapChain has drained the queue.
		loggedDLSSNRFallback = false;
	}

	// NR operates at render resolution. DLSS SR consumes either its 1:1 output
	// or, on fallback, the original render-resolution color and produces the
	// display-resolution scene used by presentation and frame generation.
	auto* srInput = neuralRendered ? a_outputColor : a_color;
	auto* srOutput = neuralRendered ? a_sharpenedOutput : a_outputColor;
	if (srInputHistoryValid && srInputNRPassCount != evaluatedNRPassCount) {
		// A failed/recovered NR pass changes SR's signal even without a setting
		// change. Do not blend the original frame with NR's altered appearance.
		RequestTemporalReset();
	}
	if (!evaluate(false, srInput, srOutput, lowResExtent, fullExtent)) {
#ifdef UPSCALING_NR_CAPTURE
		if (capture) { NRDiagnosticCapture::Finish(a_commandList, neuralRendered, false, evaluatedNRPassCount); }
#endif
		srInputHistoryValid = false;
		RequestTemporalReset();
		return false;
	}
	srInputNRPassCount = evaluatedNRPassCount;
#ifdef UPSCALING_NR_CAPTURE
	if (capture) {
		NRDiagnosticCapture::Copy(a_commandList, "sr", srOutput, fullExtent.width, fullExtent.height);
		NRDiagnosticCapture::Finish(a_commandList, neuralRendered, true, evaluatedNRPassCount);
	}
#endif
	srInputHistoryValid = true;
	if (neuralRendered) {
		loggedDLSSNRFallback = false;
		if (a_sharpened) {
			*a_sharpened = true;
		}
		return true;
	}

	if (a_sharpness > 0.0f && a_sharpenedOutput &&
		ApplyNISSharpenD3D12(a_outputColor, a_sharpenedOutput, a_commandList, a_frameToken, a_displaySize, a_sharpness)) {
		if (a_sharpened) {
			*a_sharpened = true;
		}
	}

	return true;
}

bool Streamline::UpdateConstants(float2 a_jitter)
{
	static auto gameViewport = Util::State_GetSingleton();
	if (!gameViewport) {
		return false;
	}

	const auto currentFrameIndex = gameViewport->frameCount;
	if (!EnsureFrameToken(currentFrameIndex)) {
		return false;
	}
	if (constantsTokenIndex == static_cast<uint32_t>(*frameToken)) {
		return true;
	}
	if (lastConstantsFrameIndex != std::numeric_limits<uint32_t>::max() &&
		currentFrameIndex != lastConstantsFrameIndex + 1) {
		RequestTemporalReset();
	}

	const auto cameraProjection = Util::GetCameraProjection();
	const auto* cameraState = cameraProjection.cameraState;
	if (!cameraState || !cameraProjection.usedMatrixFOV ||
		!std::isfinite(a_jitter.x) || !std::isfinite(a_jitter.y) ||
		!IsFinitePosition(cameraState->currentPosAdjust)) {
		logger::error(
			"[Streamline] Common constants have no valid world camera frame={} camera={} fov={} aspect={} matrixFov={}",
			currentFrameIndex,
			static_cast<const void*>(cameraState),
			cameraProjection.cameraFOV,
			cameraProjection.cameraAspectRatio,
			cameraProjection.usedMatrixFOV);
		return false;
	}

	static auto cameraNear = reinterpret_cast<float*>(REL::ID{ 57985, 2712882 }.address());
	static auto cameraFar = reinterpret_cast<float*>(REL::ID{ 958877, 2712883 }.address());
	if (!std::isfinite(*cameraNear) || !std::isfinite(*cameraFar) ||
		*cameraNear <= 0.0f || *cameraFar <= *cameraNear) {
		logger::error("[Streamline] Common constants have invalid camera planes near={} far={}", *cameraNear, *cameraFar);
		return false;
	}

	const auto& viewData = cameraState->camViewData;
	Util::CameraBasis cameraBasis{};
	if (!Util::TryGetCameraBasis(viewData, cameraBasis)) {
		logger::error("[Streamline] Common constants have no valid orthonormal camera basis");
		return false;
	}

	DirectX::XMMATRIX clipToCameraView{};
	DirectX::XMMATRIX clipToCurrentWorld{};
	DirectX::XMMATRIX prevClipToClip{};
	const auto currentViewProj = Util::ToXMMatrix(viewData.currentViewProjUnjittered);
	const auto previousViewProj = Util::ToXMMatrix(viewData.previousViewProjUnjittered);
	if (!TryInvertMatrix(cameraProjection.cameraViewToClip, clipToCameraView) ||
		!TryInvertMatrix(currentViewProj, clipToCurrentWorld) ||
		!IsUsableMatrix(previousViewProj)) {
		logger::error("[Streamline] Common constants have invalid camera transforms frame={}", currentFrameIndex);
		return false;
	}

	const auto clipToPrevClip = DirectX::XMMatrixMultiply(clipToCurrentWorld, previousViewProj);
	if (!IsUsableMatrix(clipToPrevClip) ||
		!TryInvertMatrix(clipToPrevClip, prevClipToClip)) {
		logger::error("[Streamline] Common constants have invalid camera transforms frame={}", currentFrameIndex);
		return false;
	}

	if (constantsReferenceCamera && constantsReferenceCamera != cameraState->referenceCamera) {
		RequestTemporalReset();
	}
	const auto resetHistory = temporalResetPending;

	sl::Constants slConstants{};
	slConstants.cameraViewToClip = ToSLMatrix(cameraProjection.cameraViewToClip);
	slConstants.clipToCameraView = ToSLMatrix(clipToCameraView);
	slConstants.clipToPrevClip = ToSLMatrix(clipToPrevClip);
	slConstants.prevClipToClip = ToSLMatrix(prevClipToClip);
	slConstants.cameraNear = *cameraNear;
	slConstants.cameraFar = *cameraFar;
	slConstants.cameraAspectRatio = cameraProjection.cameraAspectRatio;
	slConstants.cameraFOV = cameraProjection.cameraFOV;
	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraPinholeOffset = { 0.0f, 0.0f };
	slConstants.cameraPos = { cameraState->currentPosAdjust.x, cameraState->currentPosAdjust.y, cameraState->currentPosAdjust.z };
	slConstants.cameraFwd = { cameraBasis.forward.x, cameraBasis.forward.y, cameraBasis.forward.z };
	slConstants.cameraUp = { cameraBasis.up.x, cameraBasis.up.y, cameraBasis.up.z };
	slConstants.cameraRight = { cameraBasis.right.x, cameraBasis.right.y, cameraBasis.right.z };
	slConstants.depthInverted = sl::Boolean::eFalse;
	slConstants.jitterOffset = { -a_jitter.x, -a_jitter.y };
	slConstants.mvecScale = { 1.0f, 1.0f };
	slConstants.reset = resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.orthographicProjection = sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eTrue;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slSetConstants(slConstants, *frameToken, viewport))) {
		logger::error(
			"[Streamline] Could not set constants: {} frame={} token={} camera={} fov={} aspect={} reset={}",
			magic_enum::enum_name(res),
			currentFrameIndex,
			static_cast<std::uint32_t>(*frameToken),
			static_cast<const void*>(cameraState),
			cameraProjection.cameraFOV,
			cameraProjection.cameraAspectRatio,
			resetHistory);
		return false;
	}

	constantsFrameIndex = currentFrameIndex;
	constantsJitter = { slConstants.jitterOffset.x, slConstants.jitterOffset.y };
	constantsTokenIndex = static_cast<uint32_t>(*frameToken);
	lastConstantsFrameIndex = currentFrameIndex;
	constantsReferenceCamera = cameraState->referenceCamera;
	if (resetHistory) {
		temporalResetPending = false;
		lastTemporalResetFrameIndex = currentFrameIndex;
	}
	return true;
}

bool Streamline::GetD3D12DLSSNRPreparation(uint32_t a_slot, nvngx::dlss_nr::D3D12EvaluationParameters& a_parameters) const
{
	const auto* upscaling = Upscaling::GetSingleton();
	if (a_slot >= upscaling->dlssD3D12InputsReady.size() || dlssNRSuspended ||
		(!upscaling->nrAfterSR[a_slot] && featureDLSSNR && slDLSSNRSetOptions) ||
		!upscaling->IsDLSSNRReady() ||
		!upscaling->dlssD3D12InputsReady[a_slot] || !upscaling->dlssSharpenedD3D12[a_slot]) {
		return false;
	}
	const auto size = upscaling->nrAfterSR[a_slot] ? upscaling->dlssgInputDisplaySizes[a_slot] : upscaling->dlssgInputRenderSizes[a_slot];
	a_parameters = {};
	a_parameters.inputWidth = a_parameters.outputWidth = a_parameters.guideWidth = static_cast<uint32_t>(size.x);
	a_parameters.inputHeight = a_parameters.outputHeight = a_parameters.guideHeight = static_cast<uint32_t>(size.y);
	a_parameters.options.performanceMode = nvngx::dlss_nr::kNativePerformanceMode;
	a_parameters.options.preset = upscaling->settings.dlssNRPreset;
	a_parameters.options.style = upscaling->settings.dlssNRStyle;
	a_parameters.options.intensity = upscaling->settings.dlssNRIntensity;
	a_parameters.options.localToneStrength = upscaling->settings.dlssNRLocalToneStrength;
	a_parameters.options.localStructureStrength = upscaling->settings.dlssNRLocalStructureStrength;
	a_parameters.options.skinStructureStrength = upscaling->settings.dlssNRSkinStructureStrength;
	a_parameters.options.useAutoMask = upscaling->settings.dlssNRUseAutoMask != 0;
	a_parameters.passCount = std::clamp(upscaling->settings.dlssNRPassCount, 1u, 3u);
	a_parameters.outputFormat = upscaling->dlssD3D12ColorFormats[a_slot];
	return directDLSSNR.NeedsFeaturePreparation(a_parameters);
}

bool Streamline::PrepareD3D12DLSSNR(ID3D12GraphicsCommandList* a_list, const nvngx::dlss_nr::D3D12EvaluationParameters& a_parameters)
{
	return directDLSSNR.PrepareFeature(a_list, a_parameters);
}

void Streamline::DisableDLSS()
{
	currentD3D12DLSSOptionsValid = false;
	currentD3D12DLSSNROptionsValid = false;
	if (!initialized) {
		return;
	}

	if (featureDLSS && slDLSSSetOptions) {
		sl::DLSSOptions dlssOptions{};
		dlssOptions.mode = sl::DLSSMode::eOff;
		if (SL_FAILED(result, slDLSSSetOptions(viewport, dlssOptions))) {
			logger::warn("[Streamline] Could not disable DLSS: {}", magic_enum::enum_name(result));
		}
	}

	if (featureDLSSNR && slDLSSNRSetOptions) {
		sl::DLSSNROptions dlssNROptions{};
		dlssNROptions.mode = sl::DLSSNRMode::eOff;
		if (SL_FAILED(result, slDLSSNRSetOptions(viewport, dlssNROptions))) {
			logger::warn("[Streamline] Could not disable DLSS-NR: {}", magic_enum::enum_name(result));
		}
	}
}

void Streamline::DestroyDLSSResources(bool a_preserveDirectNR)
{
	RequestTemporalReset();
	srInputHistoryValid = false;
	srInputNRPassCount = 0;
	DisableDLSS();
	if (!a_preserveDirectNR) { directDLSSNR.ReleaseFeature(); }

	if (!initialized || !slFreeResources) {
		return;
	}

	if (featureDLSS) {
		if (SL_FAILED(result, slFreeResources(sl::kFeatureDLSS, viewport))) {
			logger::warn("[Streamline] Could not free DLSS resources: {}", magic_enum::enum_name(result));
		}
	}
	if (featureDLSSNR && !dlssNRSuspended) {
		if (SL_FAILED(result, slFreeResources(sl::kFeatureDLSS_NR, viewport))) {
			logger::warn("[Streamline] Could not free DLSS-NR resources: {}", magic_enum::enum_name(result));
		}
	}
	if (featureNIS && nisResourcesUsed) {
		// This teardown is called after the queue drain. NIS caches resource
		// descriptions by native pointer; recycled texture addresses otherwise
		// retain stale dimensions after scene/quality resource recreation.
		if (SL_FAILED(result, slFreeResources(sl::kFeatureNIS, viewport))) {
			logger::warn("[Streamline] Could not clear NIS resource cache: {}", magic_enum::enum_name(result));
		} else {
			nisResourcesUsed = false;
		}
	}
	currentNISOptionsValid = false;
	currentNISSharpness = -1.0f;
}

void Streamline::SuspendDLSSNR()
{
	if (dlssNRSuspended) {
		return;
	}

	currentD3D12DLSSNROptionsValid = false;
	if (initialized && featureDLSSNR && slDLSSNRSetOptions) {
		sl::DLSSNROptions options{};
		options.mode = sl::DLSSNRMode::eOff;
		if (SL_FAILED(result, slDLSSNRSetOptions(viewport, options))) {
			logger::warn("[Streamline] Could not suspend DLSS-NR: {}", magic_enum::enum_name(result));
		}
	}

	// The owner drains the D3D12 queue before entering this method. Both the
	// Streamline and direct NGX backends may otherwise retain GPU references.
	directDLSSNR.ReleaseFeature();
	if (initialized && featureDLSSNR && slFreeResources) {
		if (SL_FAILED(result, slFreeResources(sl::kFeatureDLSS_NR, viewport))) {
			logger::warn("[Streamline] Could not free suspended DLSS-NR resources: {}", magic_enum::enum_name(result));
		}
	}
	dlssNRSuspended = true;
	RequestTemporalReset();
}

void Streamline::ResumeDLSSNR()
{
	if (!dlssNRSuspended) {
		return;
	}
	dlssNRSuspended = false;
	currentD3D12DLSSNROptionsValid = false;
	RequestTemporalReset();
}
