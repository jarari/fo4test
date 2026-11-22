#pragma once

// Direct NGX DLSS super resolution (NVSDK_NGX_Feature_SuperSampling).
//
// Streamline's sl.common already initializes the driver NGX core
// (_nvngx.dll) for the plugin's D3D12 device when its DLSS plugin loads. This
// backend does not call Init/Shutdown: it binds the core's exported D3D12
// feature operations (the same shared entry points the direct DLSS-NR path
// prefers) and owns only its own parameter map and feature handle. Streamline
// never creates its own DLSS feature while this path is active, so its
// per-evaluate VRAM budget bookkeeping and logging are bypassed for SR.

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <array>
#include <cstdint>
#include <vector>

namespace nvngx::dlss_sr
{
	// Render preset hints, ordered DLAA, Quality, Balanced, Performance,
	// Ultra Performance, Ultra Quality (NVSDK_NGX_DLSS_Hint_Render_Preset).
	using PresetHints = std::array<std::uint32_t, 6>;

	struct D3D12Parameters
	{
		ID3D12Resource* color = nullptr;
		ID3D12Resource* output = nullptr;
		ID3D12Resource* motionVectors = nullptr;
		ID3D12Resource* depth = nullptr;
		ID3D12Resource* animatedTextureMask = nullptr;  // optional
		std::uint32_t renderWidth = 0;
		std::uint32_t renderHeight = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		int perfQualityValue = 0;  // NVSDK_NGX_PerfQuality_Value
		PresetHints presets{};
		int createFlags = 0;       // NVSDK_NGX_DLSS_Feature_Flags
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		float motionVectorScaleX = 1.0f;
		float motionVectorScaleY = 1.0f;
		float preExposure = 1.0f;
		float exposureScale = 1.0f;
		bool reset = false;
	};

	class D3D12Backend
	{
	public:
		D3D12Backend() = default;
		~D3D12Backend() = default;
		D3D12Backend(const D3D12Backend&) = delete;
		D3D12Backend& operator=(const D3D12Backend&) = delete;

		// True once binding/creation failed in a way that will not recover
		// (core not loaded/initialized, feature unsupported). Callers then use
		// the Streamline DLSS path.
		bool IsUnavailable() const { return unavailable_; }
		bool HasFeature() const { return feature_ != nullptr; }

		// Creates or recreates the feature on this list when its configuration
		// changed, then records the evaluation. a_recreated reports whether a
		// previous feature with a different configuration was replaced.
		bool Evaluate(ID3D12GraphicsCommandList* a_commandList, const D3D12Parameters& a_parameters, bool* a_recreated = nullptr);

		// Called once per DLSS SR frame. Releases retired handles once their
		// command lists can no longer be in flight; optionally retires the
		// current feature (direct path switched off at runtime).
		void AdvanceFrame(bool a_retireCurrent);

		// Immediate release of every handle. Caller has drained the GPU queue.
		void ReleaseAll();

		NVSDK_NGX_Result GetLastResult() const { return lastResult_; }

	private:
		using PFun_AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
		using PFun_DestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using PFun_CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
		using PFun_EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
		using PFun_ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

		struct Configuration
		{
			std::uint32_t createWidth = 0;
			std::uint32_t createHeight = 0;
			std::uint32_t outputWidth = 0;
			std::uint32_t outputHeight = 0;
			int perfQualityValue = -1;
			PresetHints presets{};
			int createFlags = 0;
		};

		struct Retired
		{
			NVSDK_NGX_Handle* feature = nullptr;
			NVSDK_NGX_Parameter* parameters = nullptr;
			std::uint64_t releaseAfterFrame = 0;
		};

		bool BindSharedOperations();
		bool NeedsRecreation(const D3D12Parameters& a_parameters) const;
		bool CreateFeature(ID3D12GraphicsCommandList* a_commandList, const D3D12Parameters& a_parameters);
		void RetireCurrent();
		bool ReleaseHandles(NVSDK_NGX_Handle* a_feature, NVSDK_NGX_Parameter* a_parameters);
		void MarkUnavailable(const char* a_reason, NVSDK_NGX_Result a_result);

		static constexpr std::uint64_t kRetireFrames = 8;

		bool bindAttempted_ = false;
		bool unavailable_ = false;
		PFun_AllocateParameters allocateParameters_ = nullptr;
		PFun_DestroyParameters destroyParameters_ = nullptr;
		PFun_CreateFeature createFeature_ = nullptr;
		PFun_EvaluateFeature evaluateFeature_ = nullptr;
		PFun_ReleaseFeature releaseFeature_ = nullptr;

		NVSDK_NGX_Parameter* parameters_ = nullptr;
		NVSDK_NGX_Handle* feature_ = nullptr;
		Configuration configuration_{};
		bool forceReset_ = true;
		std::uint64_t frame_ = 0;
		std::vector<Retired> retired_;
		NVSDK_NGX_Result lastResult_ = NVSDK_NGX_Result_Success;
	};
}
