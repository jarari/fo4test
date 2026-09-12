#pragma once

#include "Util.h"
#include "Buffer.h"
#include "FidelityFX.h"
#include "FrameCount.h"
#include "Streamline.h"

#include <array>
#include <deque>
#include <initializer_list>
#include <memory>
#include <winrt/base.h>

const uint renderTargetsPatch[] = { 0, 20, 57, 24, 25, 23, 58, 59, 28, 3, 9, 60, 61, 4, 29, 1, 2, 36, 37, 22, 10, 11, 7, 8, 64, 14, 16 };

/**
 * @class Upscaling
 * @brief Main upscaling manager that handles FSR3 and DLSS upscaling for Fallout 4
 *
 * This class manages all aspects of upscaling including:
 * - Dynamic render target scaling
 * - Sampler state mipmap bias adjustment
 * - Depth buffer management
 * - Integration with FSR3 and DLSS backends
 */
class Upscaling : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
	// ========================================
	// Singleton & Initialization
	// ========================================

	/**
	 * @brief Get the singleton instance
	 * @return Pointer to the global Upscaling instance
	 */
	static Upscaling* GetSingleton()
	{
		static Upscaling singleton;
		return &singleton;
	}

	/**
	 * @brief Initialize the upscaling system after game data is loaded
	 *
	 * Registers event sinks and loads initial settings from INI file
	 */
	void OnDataLoaded();

	/**
	 * @brief Install all game engine hooks required for upscaling
	 *
	 * Patches render pipeline, TAA shaders, dynamic resolution, and other
	 * game systems to integrate upscaling functionality
	 */
	static void InstallHooks();
	static void InstallHighFPSPhysicsFixCompatibility();

	// ========================================
	// Settings & Configuration
	// ========================================

	/**
	 * @enum UpscaleMethod
	 * @brief Available upscaling methods
	 */
	enum class UpscaleMethod
	{
		kDisabled,  ///< No upscaling, native TAA
		kFSR,       ///< AMD FidelityFX Super Resolution 3
		kDLSS,      ///< NVIDIA Deep Learning Super Sampling
		kSpatialFallback  ///< Local spatial fallback used while temporal SDK requests are retry-blocked
	};

	static constexpr bool kForceFSRFrameGenerationForTesting = false;

	/**
	 * @struct Settings
	 * @brief User-configurable upscaling settings
	 */
	struct Settings
	{
		uint upscaleMethodPreference = (uint)UpscaleMethod::kDLSS; ///< Preferred upscaling method
		uint qualityMode = 1;									   ///< Quality mode: 0=Native AA, 1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance
		uint frameGenerationMode = 0;                              ///< DLSS-G mode: 0=Disabled, 1=On, 2=Auto, 3=Dynamic
		uint dlssgGeneratedFrames = 0;                              ///< MCM index: 0=one generated frame, up to runtime-supported max
		uint dynamicMFGEnabled = 0;                                 ///< Enable DLSS-G Dynamic Multi Frame Generation when supported
		uint dynamicMFGTargetFPS = 300;                              ///< Dynamic MFG target FPS; 0 lets Streamline auto-detect display refresh
		uint reflexMode = 1;                                        ///< Reflex mode: 0=Off, 1=On, 2=On + Boost
		uint dlssModelPreset = 0;                                   ///< DLSS model preset: 0=Recommended, 1=Default, 2=K, 3=M, 4=L
		uint dlssNRPosition = 0;                                    ///< 0: Before SR, 1: After SR
		uint dlssNREnabled = 1;                                     ///< Enable optional NR at the selected SR stage
		uint dlssNRPassCount = 1;                                   ///< Direct-NGX NR histories evaluated in sequence (1..3)
		uint vsyncMode = 0;                                        ///< 0=game, 1=off, 2=on
		uint outputFPSLimit = 0;                                   ///< FG-inclusive target, 0=unlimited, otherwise 10..500
		uint dlssNRPreset = 0;                                      ///< DLSS-NR preset: 0=Default, 1..3=preview presets
		uint dlssNRStyle = 0;                                       ///< DLSS-NR style: 0=Natural, 1=Cinematic
		uint dlssNRUseAutoMask = 0;                                 ///< Ask DLSS-NR to generate its control mask
		uint osdMode = 0;                                           ///< Debug OSD: 0=Off, 1=Compact, 2=Detailed
		float sharpness = 0.2f;                                       ///< Upscaler sharpness: 0.0=off, 1.0=max
		float dlssNRIntensity = 1.0f;
		float dlssNRLocalToneStrength = 1.0f;
		float dlssNRLocalStructureStrength = 1.0f;
		float dlssNRSkinStructureStrength = 1.0f;
	};

	Settings settings;

	/**
	 * @brief Load settings from the persistent configuration file
	 *
	 * Reads the legacy-compatible Data/MCM/Settings/Upscaling.ini location and
	 * updates the settings struct.
	 */
	void LoadSettings();
	bool SaveSettings(const Settings& a_settings);
	void ReloadSettingsIfChanged();

	/**
	 * @brief Determine which upscaling method should be used
	 * @param a_checkMenu If true, disable upscaling when certain menus are open
	 * @return The active upscaling method
	 *
	 * Falls back to FSR if DLSS is not available but preferred
	 */
	UpscaleMethod GetUpscaleMethod(bool a_checkMenu);

	/**
	 * @brief Determine whether DLSS-G should run for the current frame.
	 */
	bool ShouldBlockUpscaling() const;
	bool ShouldBlockFrameGeneration() const;
	bool ShouldBlockTemporalFeatures() const;
	bool ShouldUseFrameGeneration(bool a_checkMenu);
	bool ShouldUseFSRFrameGeneration(bool a_checkMenu);
	bool IsFrameGenerationActive() const { return frameGenerationActive; }
	bool IsFSRFrameGenerationActive() const { return fsrFrameGenerationActive; }
	bool WantsFrameGenerationInputsThisFrame() const { return frameGenerationInputsWanted; }
	bool IsD3D12DLSSActive() const { return d3d12DLSSActive; }

	/**
	 * @brief Process menu open/close events
	 * @param a_event The menu event
	 * @param a_source Event source (unused)
	 * @return Event control flag
	 *
	 * Reloads settings when the existing Fallout pause-menu path closes
	 */
	RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*);

	// ========================================
	// Main Upscaling Operations
	// ========================================

	/**
	 * @brief Update camera jitter for temporal anti-aliasing
	 *
	 * Calculates per-frame jitter offsets, updates sampler states, render targets,
	 * and manages resource creation/destruction based on active upscaling method
	 */
	void UpdateUpscaling();

	/**
	 * @brief Perform upscaling operation
	 *
	 * Executes the active upscaling method (FSR3 or DLSS) to upscale the
	 * rendered image from render resolution to display resolution
	 */
	void Upscale(int a_renderTargetIndex = static_cast<int>(Util::RenderTarget::kFrameBuffer));

	/**
	 * @brief Capture and tag present-time DLSS-G inputs after the HUD-less scene is available.
	 */
	void CaptureDLSSGInputs(int a_renderTargetIndex, ID3D11Texture2D* a_motionVectorTexture = nullptr, float2 a_renderSize = { 0.0f, 0.0f }, float2 a_displaySize = { 0.0f, 0.0f });
	bool CaptureD3D12FSRInputs(int a_renderTargetIndex, ID3D11Texture2D* a_motionVectorTexture, float2 a_jitter, float2 a_renderSize, float2 a_displaySize);
	bool EvaluateD3D12DLSS(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex);
	bool EvaluateD3D12FSR(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex);
	bool EvaluateFSRFrameGeneration(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex);
	void TagDLSSGInputs(ID3D12GraphicsCommandList* a_commandList, uint32_t a_frameIndex);
	void OnD3D12TemporalSuspend();

	// Preserve the existing SDK grace period for tagged resources, in addition
	// to both API fences. Only private NR guide textures may omit that grace.
	void RetireD3D11Texture(std::unique_ptr<Texture2D>& a_texture);
	void RetireSharedD3D12Texture(std::unique_ptr<Texture2D>& a_texture, winrt::com_ptr<ID3D12Resource>& a_d3d12Resource, bool a_requiresPresentGrace = true);
	void RetireD3D12Resource(winrt::com_ptr<ID3D12Resource>& a_resource);
	void AdvanceDeferredResourceReleases();
	void FlushDeferredResourceReleases();

	/**
	 * @brief Check and manage upscaling resources
	 *
	 * Creates or destroys upscaling resources when switching between
	 * different upscaling methods
	 */
	void CheckResources();

	float2 jitter = { 0, 0 };  ///< Current frame's camera jitter offset
	float2 osdRenderSize = { 0.0f, 0.0f };
	float2 osdNativeSize = { 0.0f, 0.0f };
	UpscaleMethod upscaleMethodNoMenu = UpscaleMethod::kDisabled;
	UpscaleMethod upscaleMethod = UpscaleMethod::kDisabled;

	// ========================================
	// Render Target Management
	// ========================================

	/**
	 * @brief Update all render targets for new resolution scaling
	 * @param a_currentWidthRatio Width scale factor (e.g., 0.67 for balanced mode)
	 * @param a_currentHeightRatio Height scale factor
	 *
	 * Recreates proxy render targets when resolution changes
	 */
	void UpdateRenderTargets(float a_currentWidthRatio, float a_currentHeightRatio);

	/**
	 * @brief Override game render targets with scaled proxy targets
	 * @param a_indicesToCopy Optional array of render target indices that require expensive copy. Empty = copy all.
	 *
	 * Temporarily replaces game render targets with lower resolution proxies
	 * during main rendering pass
	 */
	void OverrideRenderTargets(std::initializer_list<int> a_indicesToCopy = {});

	/**
	 * @brief Restore original render targets
	 * @param a_indicesToCopy Optional array of render target indices that require expensive copy. Empty = copy all.
	 *
	 * Restores full resolution render targets after scaled rendering is complete
	 */
	void ResetRenderTargets(std::initializer_list<int> a_indicesToCopy = {});

	/**
	 * @brief Update a single render target
	 * @param index Render target index
	 * @param a_currentWidthRatio Width scale factor
	 * @param a_currentHeightRatio Height scale factor
	 */
	void UpdateRenderTarget(int index, float a_currentWidthRatio, float a_currentHeightRatio);

	/**
	 * @brief Override a single render target
	 * @param index Render target index
	 * @param a_doCopy If true, performs expensive copy of texture content. If false, only swaps pointers.
	 */
	void OverrideRenderTarget(int index, bool a_doCopy = true);

	/**
	 * @brief Reset a single render target
	 * @param index Render target index
	 * @param a_doCopy If true, performs expensive copy of texture content. If false, only swaps pointers.
	 */
	void ResetRenderTarget(int index, bool a_doCopy = true);

	RE::BSGraphics::RenderTarget originalRenderTargets[101];      ///< Original full-resolution render targets
	RE::BSGraphics::RenderTarget proxyRenderTargets[101];         ///< Scaled proxy render targets
	RE::BSGraphics::RenderTargetProperties originalRenderTargetData[100];  ///< Original RT properties

	// ========================================
	// Sampler State Management
	// ========================================

	/**
	 * @brief Update sampler states with mipmap LOD bias
	 * @param a_currentMipBias Mipmap bias to apply (negative = sharper textures)
	 *
	 * Creates modified sampler states with adjusted mip bias to compensate
	 * for lower render resolution
	 */
	void UpdateSamplerStates(float a_currentMipBias);

	/**
	 * @brief Override game sampler states with biased versions
	 *
	 * Applies sampler states with negative LOD bias during rendering
	 */
	void OverrideSamplerStates();

	/**
	 * @brief Restore original sampler states
	 */
	void ResetSamplerStates();

	std::array<ID3D11SamplerState*, 320> originalSamplerStates;  ///< Original game sampler states
	std::array<ID3D11SamplerState*, 320> biasedSamplerStates;	 ///< Modified sampler states with LOD bias

	// ========================================
	// Depth Management
	// ========================================

	/**
	 * @brief Override depth buffer with upscaled version
	 * @param a_doCopy If true, performs expensive copy of depth content. If false, only swaps pointers.
	 *
	 * Replaces depth buffer SRV with full-resolution depth for correct
	 * depth testing in post-processing effects
	 */
	void OverrideDepth(bool a_doCopy = true);

	/**
	 * @brief Restore original depth buffer
	 */
	void ResetDepth();

	/**
	 * @brief Copy and upscale depth buffers
	 */
	void CopyDepth();
	void PreFrameGenerationAlpha();
	bool PostFrameGenerationAlpha();
	void CopyFrameGenerationBuffers();
	void CaptureReShadeDepth();
	struct ReShadeDepthCaptureInfo
	{
		ID3D12Resource* resource = nullptr;
		uint32_t physicalSourceWidth = 0;
		uint32_t physicalSourceHeight = 0;
		uint32_t sampleWidth = 0;
		uint32_t sampleHeight = 0;
		uint32_t outputWidth = 0;
		uint32_t outputHeight = 0;
		float2 engineJitter{};
	};
	ReShadeDepthCaptureInfo GetCurrentSharedDepthInfo() const;
	ID3D12Resource* GetCurrentSharedDepth() const;
	uint64_t reshadeSceneDepthFrame = 0;
	std::array<uint64_t, kDX12FrameCount> dlssDepthCaptureFrames{};
	std::array<uint64_t, kDX12FrameCount> fsrDepthCaptureFrames{};
	std::array<uint64_t, kDX12FrameCount> reshadeDepthCaptureFrames{};
	std::array<uint32_t, kDX12FrameCount> reshadeDepthPhysicalSourceWidths{};
	std::array<uint32_t, kDX12FrameCount> reshadeDepthPhysicalSourceHeights{};
	std::array<uint32_t, kDX12FrameCount> reshadeDepthSampleWidths{};
	std::array<uint32_t, kDX12FrameCount> reshadeDepthSampleHeights{};
	std::array<float2, kDX12FrameCount> reshadeDepthJitters{};
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> reshadeDepthSharedTextures;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> reshadeDepthD3D12;
	winrt::com_ptr<ID3D11ComputeShader> reshadeDepthCS;
	winrt::com_ptr<ID3D11Buffer> reshadeDepthConstants;

	ID3D11ShaderResourceView* originalDepthView;	    ///< Original depth buffer SRV
	std::unique_ptr<Texture2D> depthOverrideTexture;    ///< Dynamic resolution depth override texture

	// ========================================
	// Shader Management
	// ========================================

	/**
	 * @brief Patch screen-space reflections shader
	 *
	 * Injects custom SSR shader that properly handles scaled render targets
	 */
	void PatchSSRShader();

	/**
	 * @brief Get or compile motion vector dilation shader
	 * @return Compiled compute shader
	 *
	 * Dilates motion vectors for better temporal stability in DLSS
	 */
	ID3D11ComputeShader* GetDilateMotionVectorCS();

	/**
	 * @brief Get or compile depth override shader
	 * @return Compiled compute shader
	 *
	 * Upscales depth buffer from render to display resolution
	 */
	ID3D11ComputeShader* GetOverrideLinearDepthCS();

	/**
	 * @brief Get or compile depth override shader
	 * @return Compiled compute shader
	 *
	 * Copies depth buffer
	 */
	ID3D11ComputeShader* GetOverrideDepthCS();
	ID3D11ComputeShader* GetCopyDepthToFrameGenerationCS();
	ID3D11ComputeShader* GetReShadeDepthCS();
	ID3D11ComputeShader* GetGenerateFrameGenerationBuffersCS();
	ID3D11ComputeShader* GetGenerateDLSSTransparencyMaskCS();
	void CopyPipboyMaskForSR(uint32_t slot, UINT width, UINT height);
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> pipboyMaskSharedTextures;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> pipboyMaskD3D12;
	std::array<bool, kDX12FrameCount> pipboyMaskReady{};
	ID3D11ComputeShader* GetSpatialFallbackUpscaleCS();

	/**
	 * @brief Get or compile custom SSR raytracing pixel shader
	 * @return Compiled pixel shader
	 */
	ID3D11PixelShader* GetBSImagespaceShaderSSLRRaytracing();

	/**
	 * @brief Get constant buffer for upscaling parameters
	 * @return Pointer to constant buffer
	 *
	 * Contains screen size, render size, and camera data
	 */
	ConstantBuffer* GetUpscalingCB();

	/**
	 * @brief Update and bind upscaling constant buffer
	 * @param a_context D3D11 device context
	 * @param a_screenSize Display resolution
	 * @param a_renderSize Render resolution
	 *
	 * Helper function to fill and bind the upscaling CB to slot 0
	 * Automatically reads camera parameters from the game engine
	 */
	void UpdateAndBindUpscalingCB(ID3D11DeviceContext* a_context, float2 a_screenSize, float2 a_renderSize);

	/**
	 * @brief Updates game settings
	 */
	void UpdateGameSettings();

	// ========================================
	// Resource Management
	// ========================================

	/**
	 * @brief Create upscaling-specific resources
	 *
	 * Creates textures needed for DLSS (dilated motion vectors)
	 */
	void CreateUpscalingResources();

	/**
	 * @brief Destroy upscaling-specific resources
	 */
	void DestroyUpscalingResources(bool a_preserveNativeNR = false, bool a_interopIdle = false);
	bool IsDLSSNRReady() const { return settings.dlssNREnabled != 0 && !nrAwaitingPresent; }
	void DeferNRUntilPresent(bool a_settingsChanged = true);
	void OnNRPresentComplete(uint32_t a_slot);
	bool nrAwaitingPresent = true;
	uint64_t nrSettingsGeneration = 1;
	std::array<uint64_t, kDX12FrameCount> nrCapturedGeneration{};
	std::array<uint64_t, kDX12FrameCount> nrEvaluatedGeneration{};

	std::unique_ptr<Texture2D> upscalingTexture;           ///< Intermediate upscaling texture
	std::unique_ptr<Texture2D> spatialFallbackTexture;     ///< Full-resolution local fallback output
	std::unique_ptr<Texture2D> dlssOutputTexture;          ///< Full-resolution DLSS output texture
	std::unique_ptr<Texture2D> dilatedMotionVectorTexture; ///< Dilated motion vectors for DLSS
	std::unique_ptr<Texture2D> dlssgHUDLessTexture;        ///< Persistent HUD-less color for DLSS-G present-time consumption
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> dlssInputSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> dlssSharpenedSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> dlssgHUDLessSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> dlssgMotionVectorSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> dlssgDepthSharedTextures;
	// Unmodified engine guides for NR/SR, independent of FG's first-person repair.
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> dlssMotionVectorSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> dlssDepthSharedTextures;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssMotionVectorD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssDepthD3D12;
	// NR consumes pixel displacement; only Before SR includes sample jitter.
	// SR/FG continue to use their original, separate guide resources.
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> nrMotionSharedTextures;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> nrMotionD3D12;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> nrDepthSharedTextures;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> nrDepthD3D12;
	std::array<bool, kDX12FrameCount> nrAfterSR{};
	std::array<bool, kDX12FrameCount> nrMotionReady{};
	std::array<float2, kDX12FrameCount> nrMotionJitterDeltas{};
	winrt::com_ptr<ID3D11ComputeShader> nrMotionCS;
	winrt::com_ptr<ID3D11Buffer> nrMotionConstants;
	bool nrMotionHistoryValid = false;
	uint32_t nrMotionPreviousFrame = 0;
	float2 nrMotionPreviousJitter{}, nrMotionPreviousSize{}, nrMotionCurrentDelta{};
	winrt::com_ptr<ID3D11ComputeShader> nrAfterMotionCS;
	winrt::com_ptr<ID3D11Buffer> nrAfterMotionConstants;
	void EnsureNRGuideResources(UINT width, UINT height, bool afterSR);
	bool CaptureNRMotion(UINT slot, UINT width, UINT height);
	bool CaptureNRAfterSRGuides(UINT slot, UINT width, UINT height, float2 displaySize);
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> dlssTransparencyMaskSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> fsrInputSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> fsrOutputSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> fsrMotionVectorSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> fsrDepthSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> fsrOpaqueOnlySharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> fsrReactiveMaskSharedTextures;
	std::array<std::unique_ptr<Texture2D>, kDX12FrameCount> enbFallbackSharedTextures;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> enbFallbackD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssInputD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssSharpenedD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssD3D12PresentFinal;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssgHUDLessD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssgMotionVectorD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssgDepthD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> dlssTransparencyMaskD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> fsrInputD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> fsrOutputD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> fsrMotionVectorD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> fsrDepthD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> fsrOpaqueOnlyD3D12;
	std::array<winrt::com_ptr<ID3D12Resource>, kDX12FrameCount> fsrReactiveMaskD3D12;
	std::array<bool, kDX12FrameCount> dlssgInputsReady{};
	std::array<bool, kDX12FrameCount> fsrFrameGenerationInputsReady{};
	std::array<bool, kDX12FrameCount> fsrD3D12InputsReady{};
	std::array<uint32_t, kDX12FrameCount> dlssgInputFrameTokenIndices{};
	std::array<bool, kDX12FrameCount> dlssD3D12InputsReady{};
	std::array<bool, kDX12FrameCount> dlssD3D12Sharpened{};
	std::array<bool, kDX12FrameCount> dlssD3D12TransparencyMaskReady{};
	std::array<DXGI_FORMAT, kDX12FrameCount> dlssD3D12ColorFormats{};
	std::array<DXGI_FORMAT, kDX12FrameCount> dlssD3D12MotionVectorFormats{};
	std::array<DXGI_FORMAT, kDX12FrameCount> dlssD3D12DepthFormats{};
	std::array<float2, kDX12FrameCount> dlssgInputRenderSizes{};
	std::array<float2, kDX12FrameCount> dlssgInputDisplaySizes{};
	std::array<DXGI_FORMAT, kDX12FrameCount> fsrFrameGenerationColorFormats{};
	std::array<uint64_t, kDX12FrameCount> fsrFrameGenerationFrameIDs{};
	std::array<float2, kDX12FrameCount> fsrInputJitters{};
	std::array<float2, kDX12FrameCount> fsrInputRenderSizes{};
	std::array<float2, kDX12FrameCount> fsrInputDisplaySizes{};

	// DLSS-G can retain resources beyond work visible on our command queue.
	// Keep the proven Present grace period in addition to explicit GPU fences.
	static constexpr uint64_t kDeferredResourceReleasePresents = 16;
	struct DeferredResourceRelease
	{
		uint64_t d3d11Fence = 0;
		std::unique_ptr<Texture2D> d3d11Texture;
		winrt::com_ptr<ID3D12Resource> d3d12Resource;
		uint64_t d3d12Fence = 0;
		uint64_t releasePresent = 0;
	};
	std::deque<DeferredResourceRelease> deferredResourceReleases;
	uint64_t completedPresentCount = 0;
	bool d3d12DLSSMenuSuspended = false;
	bool dlssgMenuResumeReady = true;
	uint32_t dlssgStableGameplayFrames = 0;
	bool temporalFeaturesBlocked = false;
	bool frameGenerationActive = false;
	bool fsrFrameGenerationActive = false;
	bool frameGenerationInputsWanted = false;
	bool d3d12DLSSActive = false;

	enum class FeatureRequest
	{
		kDLSS,
		kFSR,
		kDLSSG,
		kFSRFrameGeneration
	};

	struct FeatureRetryBlock
	{
		uint64_t retryGameFrame = 0;
		uint64_t lastFailureGameFrame = std::numeric_limits<uint64_t>::max();
		uint32_t consecutiveFailures = 0;
		bool active = false;
	};

	std::array<FeatureRetryBlock, 4> featureRetryBlocks{};

	/**
	 * @struct UpscalingCB
	 * @brief Constant buffer structure for upscaling shaders
	 */
	struct UpscalingCB
	{
		uint ScreenSize[2];    ///< Display resolution (width, height)
		uint RenderSize[2];    ///< Render resolution (width, height)
		float4 CameraData;     ///< Camera parameters (far, near, far-near, far*near)
	};

private:
	// ========================================
	// Shader Resources (Private)
	// ========================================

	winrt::com_ptr<ID3D11ComputeShader> rcas;                        ///< RCAS sharpening shader
	winrt::com_ptr<ID3D11ComputeShader> dilateMotionVectorCS;        ///< Motion vector dilation shader
	winrt::com_ptr<ID3D11ComputeShader> overrideLinearDepthCS;       ///< Linear depth upscaling shader
	winrt::com_ptr<ID3D11ComputeShader> overrideDepthCS;             ///< Depth copy shader
	winrt::com_ptr<ID3D11ComputeShader> copyDepthToFrameGenerationCS;  ///< Depth copy shader for frame generation inputs
	winrt::com_ptr<ID3D11ComputeShader> generateFrameGenerationBuffersCS;  ///< First-person alpha motion/depth fix shader for frame generation inputs
	winrt::com_ptr<ID3D11ComputeShader> generateDLSSTransparencyMaskCS;  ///< DLSS transparency/history rejection mask
	winrt::com_ptr<ID3D11ComputeShader> spatialFallbackUpscaleCS;       ///< Minimal local spatial fallback upscaler
	winrt::com_ptr<ID3D11PixelShader> BSImagespaceShaderSSLRRaytracing;  ///< Custom SSR shader

	std::unique_ptr<Texture2D> frameGenerationPreAlphaTexture;
	std::unique_ptr<Texture2D> frameGenerationMotionVectorTexture;
	std::unique_ptr<Texture2D> frameGenerationDepthTexture;
	bool frameGenerationPreAlphaReady = false;
	uint32_t frameGenerationPreAlphaFrame = 0;
	bool frameGenerationBuffersReady = false;
	uint32_t frameGenerationBuffersFrame = 0;

	bool WantsFrameGenerationInputs();
	bool IsFeatureRequestBlocked(FeatureRequest a_feature) const;
	void ReportFeatureRequestFailure(FeatureRequest a_feature, std::string_view a_context);
	void ClearFeatureRequestFailure(FeatureRequest a_feature);
	void ClearFrameFeatureRequests();
	bool EnsureFrameGenerationPatchResources(float2 a_renderSize, DXGI_FORMAT a_colorResourceFormat, DXGI_FORMAT a_colorSRVFormat, DXGI_FORMAT a_motionVectorFormat);
};
