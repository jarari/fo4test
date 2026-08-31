#pragma once

// Provisional direct NGX DLSS-NR integration.
//
// NVIDIA has not published the DLSS-NR NGX parameter contract yet. Keep all
// recovered feature IDs, parameter names and entry-point signatures isolated
// here so they can be replaced with the public SDK declarations later.

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <cstdint>
#include <filesystem>

inline constexpr auto NVSDK_NGX_Feature_DLSSNR = static_cast<NVSDK_NGX_Feature>(18);
inline constexpr std::uint64_t NVSDK_NGX_DLSSNR_ApplicationId = 141959980;
inline constexpr NVSDK_NGX_Version NVSDK_NGX_DLSSNR_SDKVersion = static_cast<NVSDK_NGX_Version>(21);

inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Width[] = "DLSSNR.Width";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Height[] = "DLSSNR.Height";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_InputWidth[] = "DLSSNR.InputWidth";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_InputHeight[] = "DLSSNR.InputHeight";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_OutputWidth[] = "DLSSNR.OutputWidth";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_OutputHeight[] = "DLSSNR.OutputHeight";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Output_Width[] = "DLSSNR.Output.Width";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Output_Height[] = "DLSSNR.Output.Height";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_ScalingRatio[] = "DLSSNR.ScalingRatio";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Scale[] = "DLSSNR.Scale";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Upscaling[] = "DLSSNR.Upscaling";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_ComputeScalingRatioCallback[] = "DLSSNRComputeScalingRatioCallback";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset[] = "DLSSNR.Hint.Render.Preset";

inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Color[] = "DLSSNR.Color";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Output[] = "DLSSNR.Output";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_MVec[] = "DLSSNR.MVec";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Depth[] = "DLSSNR.Depth";

inline constexpr char NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX[] = "DLSSNR.ColorSubrectBaseX";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY[] = "DLSSNR.ColorSubrectBaseY";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth[] = "DLSSNR.ColorSubrectWidth";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight[] = "DLSSNR.ColorSubrectHeight";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX[] = "DLSSNR.MVecSubrectBaseX";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY[] = "DLSSNR.MVecSubrectBaseY";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth[] = "DLSSNR.MVecSubrectWidth";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight[] = "DLSSNR.MVecSubrectHeight";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_MVecScaleX[] = "DLSSNR.MVecScaleX";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_MVecScaleY[] = "DLSSNR.MVecScaleY";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX[] = "DLSSNR.DepthSubrectBaseX";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY[] = "DLSSNR.DepthSubrectBaseY";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth[] = "DLSSNR.DepthSubrectWidth";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight[] = "DLSSNR.DepthSubrectHeight";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_DepthInverted[] = "DLSSNR.DepthInverted";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX[] = "DLSSNR.OutputSubrectBaseX";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY[] = "DLSSNR.OutputSubrectBaseY";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth[] = "DLSSNR.OutputSubrectWidth";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight[] = "DLSSNR.OutputSubrectHeight";

inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Enabled[] = "DLSSNR.Enabled";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Reset[] = "DLSSNR.Reset";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Intensity[] = "DLSSNR.Intensity";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength[] = "DLSSNR.LocalToneStrength";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength[] = "DLSSNR.LocalStructureStrength";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_GlobalToneStrength[] = "DLSSNR.GlobalToneStrength";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength[] = "DLSSNR.SkinStructureStrength";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_UseAutoMask[] = "DLSSNR.UseAutoMask";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_Style[] = "DLSSNR.Style";
inline constexpr char NVSDK_NGX_Parameter_DLSSNR_UICorrection[] = "DLSSNR.UICorrection";

namespace nvngx::dlss_nr
{
	struct Options
	{
		std::uint32_t performanceMode = 3;
		std::uint32_t preset = 0;
		std::uint32_t style = 0;
		float intensity = 1.0f;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float globalToneStrength = 1.0f;
		float skinStructureStrength = 1.0f;
		bool useAutoMask = false;
	};

	struct D3D12EvaluationParameters
	{
		ID3D12Resource* color = nullptr;
		ID3D12Resource* output = nullptr;
		ID3D12Resource* motionVectors = nullptr;
		ID3D12Resource* depth = nullptr;
		std::uint32_t inputWidth = 0;
		std::uint32_t inputHeight = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		std::uint32_t guideWidth = 0;
		std::uint32_t guideHeight = 0;
		float motionVectorScaleX = 1.0f;
		float motionVectorScaleY = 1.0f;
		bool depthInverted = false;
		bool reset = false;
		Options options{};
	};

	class D3D12Backend
	{
	public:
		D3D12Backend() = default;
		~D3D12Backend();

		D3D12Backend(const D3D12Backend&) = delete;
		D3D12Backend(D3D12Backend&&) = delete;
		D3D12Backend& operator=(const D3D12Backend&) = delete;
		D3D12Backend& operator=(D3D12Backend&&) = delete;

		void SetRuntimeDirectory(const std::filesystem::path& a_runtimeDirectory);
		bool Prepare(ID3D12Device* a_device);
		bool NeedsFeatureRecreation(const D3D12EvaluationParameters& a_parameters) const;
		bool Evaluate(ID3D12GraphicsCommandList* a_commandList, const D3D12EvaluationParameters& a_parameters);
		void RequestReset();
		void ReleaseFeature();
		void Shutdown();

	private:
		using PFun_InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(std::uint64_t, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
		using PFun_AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
		using PFun_DestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using PFun_CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
		using PFun_EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
		using PFun_ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
		using PFun_Shutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

		bool LoadRuntime();
		bool InstallModuleNameHook();
		void RestoreModuleNameHook();
		bool EnsureFeature(ID3D12GraphicsCommandList* a_commandList, const D3D12EvaluationParameters& a_parameters);
		void SetCreationParameters(const D3D12EvaluationParameters& a_parameters);
		void SetEvaluationParameters(const D3D12EvaluationParameters& a_parameters, bool a_reset);

		std::filesystem::path runtimeDirectory_;
		HMODULE runtime_ = nullptr;
		HMODULE operationRuntime_ = nullptr;
		HMODULE spoofedCallerModule_ = nullptr;
		std::uintptr_t* moduleNameImportSlot_ = nullptr;
		std::uintptr_t originalModuleNameImport_ = 0;
		ID3D12Device* device_ = nullptr;
		NVSDK_NGX_Parameter* parameters_ = nullptr;
		NVSDK_NGX_Handle* feature_ = nullptr;
		bool initializationAttempted_ = false;
		bool initialized_ = false;
		bool forceReset_ = true;
		std::uint32_t featureInputWidth_ = 0;
		std::uint32_t featureInputHeight_ = 0;
		std::uint32_t featureOutputWidth_ = 0;
		std::uint32_t featureOutputHeight_ = 0;
		std::uint32_t featurePerformanceMode_ = 0;
		std::uint32_t featurePreset_ = 0;
		bool failureLatched_ = false;
		std::uint32_t failedInputWidth_ = 0;
		std::uint32_t failedInputHeight_ = 0;
		std::uint32_t failedOutputWidth_ = 0;
		std::uint32_t failedOutputHeight_ = 0;
		std::uint32_t failedGuideWidth_ = 0;
		std::uint32_t failedGuideHeight_ = 0;
		std::uint32_t failedPerformanceMode_ = 0;
		std::uint32_t failedPreset_ = 0;

		PFun_InitExt initExt_ = nullptr;
		PFun_AllocateParameters allocateParameters_ = nullptr;
		PFun_DestroyParameters destroyParameters_ = nullptr;
		PFun_CreateFeature createFeature_ = nullptr;
		PFun_EvaluateFeature evaluateFeature_ = nullptr;
		PFun_ReleaseFeature releaseFeature_ = nullptr;
		PFun_CreateFeature snippetCreateFeature_ = nullptr;
		PFun_EvaluateFeature snippetEvaluateFeature_ = nullptr;
		PFun_ReleaseFeature snippetReleaseFeature_ = nullptr;
		PFun_EvaluateFeature activeEvaluateFeature_ = nullptr;
		PFun_ReleaseFeature activeReleaseFeature_ = nullptr;
		PFun_Shutdown shutdown_ = nullptr;
	};
}
