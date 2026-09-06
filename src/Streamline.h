#pragma once

#define NV_WINDOWS

#include <utility>
#include <d3d12.h>

#pragma warning(push)
#pragma warning(disable: 4471)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_nis.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)
#include "sl_dlss_nr.h"
#include "nvngx_dlss_nr_private.h"
#include "Buffer.h"

#include <limits>
#include <filesystem>
#include <string_view>

using PFun_slSetTag2 = sl::Result(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer);

/**
 * @class Streamline
 * @brief Manager for NVIDIA Streamline integration and DLSS upscaling
 *
 * Handles initialization of NVIDIA Streamline SDK, feature detection,
 * and execution of DLSS (Deep Learning Super Sampling) upscaling.
 * Uses manual hooking mode to integrate with Fallout 4's rendering pipeline.
 */
class Streamline
{
public:
	// ========================================
	// Singleton & Lifecycle
	// ========================================

	/**
	 * @brief Get the singleton instance
	 * @return Pointer to the global Streamline instance
	 */
	static Streamline* GetSingleton()
	{
		static Streamline singleton;
		return &singleton;
	}

	/**
	 * @brief Destructor - frees Streamline interposer DLL
	 *
	 * Ensures proper cleanup of the loaded interposer library
	 */
	~Streamline()
	{
		Shutdown();
		if (interposer) {
			FreeLibrary(interposer);
			interposer = nullptr;
		}
	}

	/**
	 * @brief Get short name for logging
	 * @return "Streamline"
	 */
	inline std::string GetShortName() { return "Streamline"; }

	// ========================================
	// Initialization
	// ========================================

	/**
	 * @brief Load Streamline interposer DLL
	 *
	 * Loads sl.interposer.dll from Data/F4SE/Plugins/Upscaling/Streamline/
	 * The interposer provides the Streamline SDK interface.
	 */
	void LoadInterposer();

	/**
	 * @brief Initialize Streamline SDK
	 *
	 * Sets up Streamline preferences, initializes the SDK, and queries for
	 * available features (DLSS). Uses manual hooking mode to integrate with
	 * the game's D3D11 device.
	 */
	void Initialize(sl::RenderAPI a_renderAPI = sl::RenderAPI::eD3D11);
	void Shutdown();

	/**
	 * @brief Check Streamline plugin compat
	 */
	void CheckFeatures(IDXGIAdapter* a_adapte);

	/**
	 * @brief Initialise features after device
	 */
	void PostDevice();

	/**
	 * @brief Track the swap chain used for present-time Streamline features.
	 */
	void SetSwapChain(IDXGISwapChain* a_swapChain);

	/**
	 * @brief Create D3D11 device and swap chain with Streamline integration
	 * @param pAdapter GPU adapter to use
	 * @param DriverType Driver type
	 * @param Software Software rasterizer module (if applicable)
	 * @param Flags Device creation flags
	 * @param pFeatureLevels Array of feature levels to try
	 * @param FeatureLevels Number of feature levels
	 * @param SDKVersion D3D11 SDK version
	 * @param pSwapChainDesc Swap chain description
	 * @param ppSwapChain Output swap chain pointer
	 * @param ppDevice Output device pointer
	 * @param pFeatureLevel Output feature level
	 * @param ppImmediateContext Output device context pointer
	 * @return HRESULT indicating success or failure
	 *
	 * Wraps D3D11CreateDeviceAndSwapChain to inject Streamline initialization
	 * and feature detection. Checks for DLSS availability on the current GPU.
	 */
	HRESULT CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);

	// ========================================
	// DLSS Operations
	// ========================================

	bool UpscaleD3D12(ID3D12Resource* a_color, ID3D12Resource* a_outputColor, ID3D12Resource* a_sharpenedOutput, ID3D12Resource* a_motionVectors, ID3D12Resource* a_depth, ID3D12Resource* a_transparencyMask, ID3D12GraphicsCommandList* a_commandList, sl::FrameToken* a_frameToken, float2 a_renderSize, float2 a_displaySize, DXGI_FORMAT a_colorFormat, DXGI_FORMAT a_motionVectorFormat, DXGI_FORMAT a_depthFormat, uint a_qualityMode, float a_sharpness, uint a_dlssModelPreset, const sl::DLSSNROptions& a_dlssNROptions, bool* a_sharpened);

	/**
	 * @brief Update Streamline constants for current frame
	 * @param a_jitter Camera jitter offset
	 * @return True when a frame token and valid common constants were submitted
	 *
	 * Submits the complete unjittered world-camera payload required by all
	 * Streamline temporal features. Must be called before evaluation/tagging.
	 */
	bool UpdateConstants(float2 a_jitter);

	/**
	 * @brief Invalidate temporal history on the next constants submission.
	 */
	void RequestTemporalReset();

	/**
	 * @brief Configure Reflex based on user settings and DLSS-G requirements.
	 */
	void UpdateReflex(uint a_reflexMode, bool a_forceEnabled);
	void BeginRenderFrame(uint32_t a_frameIndex);
	bool BeginSimulationFrame(uint32_t a_frameIndex);
	void EndSimulationFrame(uint32_t a_frameIndex);

	/**
	 * @brief Enable/disable DLSS-G for the current frame.
	 */
	bool UpdateDLSSG(bool a_enabled, uint a_mode, uint a_numFramesToGenerate, bool a_dynamicMFGEnabled, uint a_dynamicMFGTargetFPS, float2 a_renderSize, float2 a_displaySize, DXGI_FORMAT a_colorFormat, DXGI_FORMAT a_motionVectorFormat, DXGI_FORMAT a_depthFormat, DXGI_FORMAT a_uiFormat = DXGI_FORMAT_UNKNOWN);
	void RequestDLSSGDisable();
	bool HasPendingDLSSGDisable() const { return pendingDLSSGDisable; }
	void ApplyPendingDLSSGDisable();
	bool NeedsDLSSGPresentSafety() const;
	bool NeedsPresentMarkers() const { return featurePCL && slPCLSetMarker && (presentFrameToken || frameToken); }
	void OnDLSSGPresentComplete();
	uint32_t GetCurrentFrameTokenIndex() const { return currentFrameTokenIndex; }
	sl::FrameToken* GetFrameTokenForFrame(uint32_t a_frameIndex);
	uint32_t GetLastPresentFrameTokenIndex() const { return lastPresentFrameTokenIndex; }

	/**
	 * @brief Tag present-time DLSS-G resources for the current frame.
	 */
	void TagDLSSGResources(ID3D11Texture2D* a_hudlessColor, ID3D11Texture2D* a_motionVectors, ID3D11Texture2D* a_depth, float2 a_renderSize, float2 a_displaySize);
	void TagDLSSGResources(ID3D12Resource* a_hudlessColor, ID3D12Resource* a_motionVectors, ID3D12Resource* a_depth, ID3D12Resource* a_uiColorAlpha, ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex, float2 a_renderSize, float2 a_displaySize);
	void ClearDLSSGResourceTags(ID3D12GraphicsCommandList* a_commandList);
	void SetPresentFrameIndex(uint32_t a_frameIndex);

	/**
	 * @brief Present hook markers and DLSS-G status polling.
	 */
	void OnPresentStart();
	void OnPresentEnd(HRESULT a_result, bool a_queryState = true);
	void QueryDLSSGState(std::string_view a_phase);
	sl::ReflexMode GetCurrentReflexMode() const { return currentReflexMode; }
	// Latest completed simulation-start -> GPU-end duration; 0 means unavailable.
	float GetReflexLatencyMs();
	uint32_t GetOSDGeneratedFramesPerRenderFrame() const { return dlssgActive ? (currentDLSSGGeneratedFrames != 0 ? currentDLSSGGeneratedFrames : 1) : 0; }
	double GetDLSSGPresentedFrameMultiplier() const { return lastDLSSGPresentMultiplier; }
	uint GetCurrentDLSSQualityMode() const { return currentDLSSQualityMode; }
	uint GetCurrentDLSSModelPreset() const { return currentDLSSModelPreset; }
	uint32_t GetPCLStatsWindowMessage() const { return pclStatsWindowMessage; }
	void OnPCLStatsPing();
	uint32_t GetPCLPingCount() const { return pclPingCount; }
	bool IsPCLLatencyReportAvailable() const { return pclLatencyReportAvailable; }

	/**
	 * @brief Destroy DLSS resources and disable DLSS
	 *
	 * Disables DLSS mode and frees Streamline resources for the current viewport.
	 * Called when switching to a different upscaling method.
	 */
	void DisableDLSS();
	void DestroyDLSSResources();
	void SuspendDLSSNR();
	void ResumeDLSSNR();

	// ========================================
	// State
	// ========================================

	bool initialized = false;  ///< True if Streamline SDK is initialized
	sl::RenderAPI initializedRenderAPI = sl::RenderAPI::eD3D11; ///< Streamline RHI used at initialization
	bool featureDLSS = false;  ///< True if DLSS is available on current GPU
	bool featureDLSSNR = false; ///< True if DLSS Neural Rendering uplift is available
	bool featureDLSSG = false; ///< True if DLSS Frame Generation is available
	bool featureNIS = false; ///< True if NVIDIA Image Scaling is available
	bool featureReflex = false; ///< True if NVIDIA Reflex is available
	bool featurePCL = false; ///< True if PCL markers are available
	bool featureImGUI = false; ///< True if Streamline ImGui debug UI is available
	bool dlssgActive = false; ///< True if DLSS-G options are currently enabled

	sl::ViewportHandle viewport{ 0 };  ///< Streamline viewport handle
	sl::FrameToken* frameToken = nullptr;        ///< Current frame token for Streamline

	HMODULE interposer = NULL;  ///< Handle to sl.interposer.dll
	std::wstring interposerDirectory; ///< Directory containing Streamline runtime DLLs
	IDXGISwapChain* swapChain = nullptr; ///< Present-time swap chain
	DXGI_SWAP_CHAIN_DESC swapChainDesc{}; ///< Cached swap chain description

	bool UsesD3D12() const { return initialized && initializedRenderAPI == sl::RenderAPI::eD3D12; }

	// ========================================
	// SL Interposer Function Pointers
	// ========================================

	// Core Functions
	PFun_slInit* slInit{};                                  ///< Initialize Streamline
	PFun_slShutdown* slShutdown{};                          ///< Shutdown Streamline
	PFun_slIsFeatureSupported* slIsFeatureSupported{};      ///< Check if feature is supported
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};            ///< Check if feature is loaded
	PFun_slSetFeatureLoaded* slSetFeatureLoaded{};          ///< Set feature loaded state
	PFun_slEvaluateFeature* slEvaluateFeature{};            ///< Execute feature (e.g., DLSS)
	PFun_slAllocateResources* slAllocateResources{};        ///< Allocate feature resources
	PFun_slFreeResources* slFreeResources{};                ///< Free feature resources
	PFun_slSetTag2* slSetTag{};                             ///< Tag resources for Streamline
	PFun_slSetTagForFrame* slSetTagForFrame{};              ///< Tag resources for a specific frame
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};  ///< Get feature requirements
	PFun_slGetFeatureVersion* slGetFeatureVersion{};        ///< Get feature version
	PFun_slUpgradeInterface* slUpgradeInterface{};          ///< Upgrade interface version
	PFun_slSetConstants* slSetConstants{};                  ///< Set frame constants
	PFun_slGetNativeInterface* slGetNativeInterface{};      ///< Get native interface
	PFun_slGetFeatureFunction* slGetFeatureFunction{};      ///< Get feature-specific function
	PFun_slGetNewFrameToken* slGetNewFrameToken{};          ///< Get new frame token
	PFun_slSetD3DDevice* slSetD3DDevice{};                  ///< Set D3D11 device

	// DLSS Specific Functions
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};  ///< Get optimal DLSS settings
	PFun_slDLSSGetState* slDLSSGetState{};                      ///< Get DLSS state
	PFun_slDLSSSetOptions* slDLSSSetOptions{};                  ///< Set DLSS options
	PFun_slDLSSNRSetOptions* slDLSSNRSetOptions{};              ///< Set DLSS-NR options

	// NIS Specific Functions
	PFun_slNISGetState* slNISGetState{};                        ///< Get NIS state
	PFun_slNISSetOptions* slNISSetOptions{};                    ///< Set NIS options

	// DLSS-G Specific Functions
	PFun_slDLSSGGetState* slDLSSGGetState{};                    ///< Get DLSS-G state
	PFun_slDLSSGSetOptions* slDLSSGSetOptions{};                ///< Set DLSS-G options

	// Reflex/PCL Specific Functions
	PFun_slReflexGetState* slReflexGetState{};                  ///< Get Reflex state
	PFun_slReflexSleep* slReflexSleep{};                        ///< Reflex sleep
	PFun_slReflexSetOptions* slReflexSetOptions{};              ///< Set Reflex options
	PFun_slPCLGetState* slPCLGetState{};                        ///< Get PCL state
	PFun_slPCLSetMarker* slPCLSetMarker{};                      ///< Set PCL marker
	PFun_slPCLSetOptions* slPCLSetOptions{};                    ///< Set PCL options

private:
	void CheckFeature(sl::Feature a_feature, IDXGIAdapter* a_adapter, bool& a_available, std::string_view a_name);
	bool EnsureFrameToken(uint32_t a_frameIndex);
	bool PaceFrame(uint32_t a_frameIndex);
	bool ApplyNISSharpen(ID3D11Resource* a_inputColor, ID3D11Resource* a_outputColor, ID3D11DeviceContext* a_context, sl::FrameToken* a_frameToken, float2 a_displaySize, float a_sharpness);
	bool ApplyNISSharpenD3D12(ID3D12Resource* a_inputColor, ID3D12Resource* a_outputColor, ID3D12GraphicsCommandList* a_commandList, sl::FrameToken* a_frameToken, float2 a_displaySize, float a_sharpness);
	bool EnsureD3D12DLSSOptions(sl::DLSSMode a_mode, uint32_t a_outputWidth, uint32_t a_outputHeight, uint a_dlssModelPreset);
	bool EnsureD3D12DLSSNROptions(sl::DLSSMode a_mode, const sl::DLSSNROptions& a_options);
	void PrepareDirectDLSSNR();
	bool EnsureNISOptions(float a_sharpness, std::string_view a_logContext);
	void ResetOptionCaches();
	void SetPCLMarker(sl::PCLMarker a_marker, sl::FrameToken* a_frameToken = nullptr);
	bool DisableDLSSGNow();
	uint32_t constantsFrameIndex = std::numeric_limits<uint32_t>::max();
	uint32_t lastConstantsFrameIndex = std::numeric_limits<uint32_t>::max();
	uint32_t lastTemporalResetFrameIndex = std::numeric_limits<uint32_t>::max();
	const void* constantsReferenceCamera = nullptr;
	bool temporalResetPending = true;
	uint32_t markerFrameIndex = std::numeric_limits<uint32_t>::max();
	uint32_t reflexSleepFrame = std::numeric_limits<uint32_t>::max();
	uint32_t simulationMarkerFrame = std::numeric_limits<uint32_t>::max();
	uint32_t renderMarkerFrame = std::numeric_limits<uint32_t>::max();
	uint32_t lastDLSSGStatus = std::numeric_limits<uint32_t>::max();
	uint32_t lastDLSSGPresentedFrames = std::numeric_limits<uint32_t>::max();
	uint32_t lastDLSSGStateQueryFrame = std::numeric_limits<uint32_t>::max();
	double lastDLSSGPresentMultiplier = 1.0;
	uint32_t maxFramesToGenerate = 1;
	bool dynamicMFGSupported = false;
	bool dlssgStateKnown = false;
	bool loggedDynamicMFGUnsupported = false;
	uint32_t currentFrameTokenIndex = std::numeric_limits<uint32_t>::max();
	sl::FrameToken* presentFrameToken = nullptr;
	uint32_t presentFrameTokenIndex = std::numeric_limits<uint32_t>::max();
	uint32_t lastPresentFrameTokenIndex = std::numeric_limits<uint32_t>::max();
	sl::ReflexMode currentReflexMode = sl::ReflexMode::ReflexMode_eCount;
	bool currentReflexUseMarkersToOptimize = false;
	uint32_t currentReflexThreadId = 0;
	sl::DLSSGMode currentDLSSGMode = sl::DLSSGMode::eCount;
	uint32_t currentDLSSGGeneratedFrames = 0;
	uint32_t currentDLSSGDynamicTargetFPS = 0;
	uint currentDLSSQualityMode = 1;
	uint currentDLSSModelPreset = 0;
	bool currentD3D12DLSSOptionsValid = false;
	sl::DLSSMode currentD3D12DLSSMode = sl::DLSSMode::eOff;
	uint32_t currentD3D12DLSSOutputWidth = 0;
	uint32_t currentD3D12DLSSOutputHeight = 0;
	uint currentD3D12DLSSModelPreset = std::numeric_limits<uint>::max();
	bool currentD3D12DLSSNROptionsValid = false;
	sl::DLSSNROptions currentD3D12DLSSNROptions{};
	bool currentNISOptionsValid = false;
	float currentNISSharpness = -1.0f;
	uint32_t pclStatsWindowMessage = 0;
	uint32_t pclPingCount = 0;
	bool pclLatencyReportAvailable = false;
	bool pendingDLSSGDisable = false;
	uint32_t dlssgPresentSafetyFrames = 0;
	bool loggedDLSSNRFallback = false;
	bool dlssNRSuspended = false;
	nvngx::dlss_nr::D3D12Backend directDLSSNR;
};
