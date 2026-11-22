#include "nvngx_dlss_sr_direct.h"

#include <filesystem>
#include <iterator>
#include <type_traits>

namespace
{
	const char* DescribeResult(NVSDK_NGX_Result a_result)
	{
		switch (a_result) {
		case NVSDK_NGX_Result_Success:
			return "Success";
		case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
			return "FeatureNotSupported";
		case NVSDK_NGX_Result_FAIL_PlatformError:
			return "PlatformError";
		case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:
			return "FeatureAlreadyExists";
		case NVSDK_NGX_Result_FAIL_FeatureNotFound:
			return "FeatureNotFound";
		case NVSDK_NGX_Result_FAIL_InvalidParameter:
			return "InvalidParameter";
		case NVSDK_NGX_Result_FAIL_NotInitialized:
			return "NotInitialized";
		case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
			return "UnsupportedInputFormat";
		case NVSDK_NGX_Result_FAIL_RWFlagMissing:
			return "RWFlagMissing";
		case NVSDK_NGX_Result_FAIL_MissingInput:
			return "MissingInput";
		case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
			return "UnableToInitializeFeature";
		case NVSDK_NGX_Result_FAIL_OutOfDate:
			return "OutOfDate";
		case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:
			return "OutOfGPUMemory";
		case NVSDK_NGX_Result_FAIL_UnsupportedFormat:
			return "UnsupportedFormat";
		case NVSDK_NGX_Result_FAIL_UnsupportedParameter:
			return "UnsupportedParameter";
		case NVSDK_NGX_Result_FAIL_Denied:
			return "Denied";
		case NVSDK_NGX_Result_FAIL_NotImplemented:
			return "NotImplemented";
		default:
			return NVSDK_NGX_SUCCEED(a_result) ? "Success" : "Fail";
		}
	}

	// Failures that mean this process cannot use the shared NGX core for
	// SuperSampling at all. Everything else (incl. OutOfGPUMemory) is treated
	// as a per-attempt failure handled by the caller's cooldown/fallback.
	bool IsPermanentCreateFailure(NVSDK_NGX_Result a_result)
	{
		return a_result == NVSDK_NGX_Result_FAIL_NotInitialized ||
			a_result == NVSDK_NGX_Result_FAIL_FeatureNotSupported ||
			a_result == NVSDK_NGX_Result_FAIL_UnableToInitializeFeature ||
			a_result == NVSDK_NGX_Result_FAIL_OutOfDate ||
			a_result == NVSDK_NGX_Result_FAIL_Denied ||
			a_result == NVSDK_NGX_Result_FAIL_NotImplemented;
	}
}

namespace nvngx::dlss_sr
{
	void D3D12Backend::MarkUnavailable(const char* a_reason, NVSDK_NGX_Result a_result)
	{
		if (!unavailable_) {
			logger::warn("[DLSS-SR Direct] {} (result=0x{:08X} {}); falling back to Streamline DLSS SR for this session",
				a_reason, static_cast<std::uint32_t>(a_result), DescribeResult(a_result));
		}
		unavailable_ = true;
	}

	bool D3D12Backend::BindSharedOperations()
	{
		if (bindAttempted_) {
			return createFeature_ != nullptr;
		}
		bindAttempted_ = true;

		// The driver NGX core is loaded and initialized for our device by
		// Streamline (sl.common) once its DLSS plugin is loaded. Only bind
		// the core; feature snippets such as nvngx_dlss.dll are not cores.
		constexpr const wchar_t* kCoreModules[] = { L"_nvngx.dll", L"nvngx.dll" };
		for (const auto* name : kCoreModules) {
			const auto module = GetModuleHandleW(name);
			if (!module) {
				continue;
			}
			const auto resolve = [module](auto& a_function, const char* a_name) {
				a_function = reinterpret_cast<std::remove_reference_t<decltype(a_function)>>(GetProcAddress(module, a_name));
				return a_function != nullptr;
			};
			const bool complete =
				resolve(allocateParameters_, "NVSDK_NGX_D3D12_AllocateParameters") &
				resolve(destroyParameters_, "NVSDK_NGX_D3D12_DestroyParameters") &
				resolve(createFeature_, "NVSDK_NGX_D3D12_CreateFeature") &
				resolve(evaluateFeature_, "NVSDK_NGX_D3D12_EvaluateFeature") &
				resolve(releaseFeature_, "NVSDK_NGX_D3D12_ReleaseFeature");
			if (complete) {
				wchar_t modulePath[MAX_PATH]{};
				GetModuleFileNameW(module, modulePath, static_cast<DWORD>(std::size(modulePath)));
				logger::info("[DLSS-SR Direct] Using shared NGX core operations from {}", std::filesystem::path(modulePath).string());
				return true;
			}
			allocateParameters_ = nullptr;
			destroyParameters_ = nullptr;
			createFeature_ = nullptr;
			evaluateFeature_ = nullptr;
			releaseFeature_ = nullptr;
		}

		MarkUnavailable("No initialized NGX core module exposes the D3D12 feature operations", NVSDK_NGX_Result_FAIL_NotInitialized);
		return false;
	}

	bool D3D12Backend::NeedsRecreation(const D3D12Parameters& a_parameters) const
	{
		return !feature_ ||
			a_parameters.renderWidth > configuration_.createWidth ||
			a_parameters.renderHeight > configuration_.createHeight ||
			a_parameters.outputWidth != configuration_.outputWidth ||
			a_parameters.outputHeight != configuration_.outputHeight ||
			a_parameters.perfQualityValue != configuration_.perfQualityValue ||
			a_parameters.presets != configuration_.presets ||
			a_parameters.createFlags != configuration_.createFlags;
	}

	bool D3D12Backend::CreateFeature(ID3D12GraphicsCommandList* a_commandList, const D3D12Parameters& a_parameters)
	{
		if (!parameters_) {
			const auto allocateResult = allocateParameters_(&parameters_);
			if (NVSDK_NGX_FAILED(allocateResult) || !parameters_) {
				lastResult_ = NVSDK_NGX_FAILED(allocateResult) ? allocateResult : NVSDK_NGX_Result_Fail;
				logger::warn("[DLSS-SR Direct] AllocateParameters failed result=0x{:08X} {}",
					static_cast<std::uint32_t>(lastResult_), DescribeResult(lastResult_));
				if (lastResult_ == NVSDK_NGX_Result_FAIL_NotInitialized) {
					MarkUnavailable("NGX core is not initialized for this device", lastResult_);
				}
				return false;
			}
		}

		auto* p = parameters_;
		p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
		p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
		p->Set(NVSDK_NGX_Parameter_Width, a_parameters.renderWidth);
		p->Set(NVSDK_NGX_Parameter_Height, a_parameters.renderHeight);
		p->Set(NVSDK_NGX_Parameter_OutWidth, a_parameters.outputWidth);
		p->Set(NVSDK_NGX_Parameter_OutHeight, a_parameters.outputHeight);
		p->Set(NVSDK_NGX_Parameter_PerfQualityValue, a_parameters.perfQualityValue);
		p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, a_parameters.createFlags);
		p->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
		p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, a_parameters.presets[0]);
		p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, a_parameters.presets[1]);
		p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, a_parameters.presets[2]);
		p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, a_parameters.presets[3]);
		p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, a_parameters.presets[4]);
		p->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, a_parameters.presets[5]);

		NVSDK_NGX_Handle* handle = nullptr;
		const auto createResult = createFeature_(a_commandList, NVSDK_NGX_Feature_SuperSampling, p, &handle);
		lastResult_ = createResult;
		if (NVSDK_NGX_FAILED(createResult) || !handle) {
			if (handle) {
				// Keep a partially created handle until its list has retired.
				retired_.push_back({ handle, nullptr, frame_ + kRetireFrames });
			}
			const bool outOfMemory = createResult == NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
			logger::warn(
				"[DLSS-SR Direct] CreateFeature(SuperSampling) failed result=0x{:08X} {}{} render={}x{} output={}x{} perfQuality={} flags=0x{:X}",
				static_cast<std::uint32_t>(createResult), DescribeResult(createResult),
				outOfMemory ? " (NGX out of GPU memory)" : "",
				a_parameters.renderWidth, a_parameters.renderHeight, a_parameters.outputWidth, a_parameters.outputHeight,
				a_parameters.perfQualityValue, static_cast<std::uint32_t>(a_parameters.createFlags));
			if (IsPermanentCreateFailure(createResult)) {
				MarkUnavailable("CreateFeature(SuperSampling) is not usable on the shared NGX core", createResult);
			}
			return false;
		}

		feature_ = handle;
		configuration_.createWidth = a_parameters.renderWidth;
		configuration_.createHeight = a_parameters.renderHeight;
		configuration_.outputWidth = a_parameters.outputWidth;
		configuration_.outputHeight = a_parameters.outputHeight;
		configuration_.perfQualityValue = a_parameters.perfQualityValue;
		configuration_.presets = a_parameters.presets;
		configuration_.createFlags = a_parameters.createFlags;
		forceReset_ = true;
		logger::info(
			"[DLSS-SR Direct] Created NGX SuperSampling feature render={}x{} output={}x{} perfQuality={} flags=0x{:X} presets(DLAA/Q/B/P/UP)={}/{}/{}/{}/{}",
			a_parameters.renderWidth, a_parameters.renderHeight, a_parameters.outputWidth, a_parameters.outputHeight,
			a_parameters.perfQualityValue, static_cast<std::uint32_t>(a_parameters.createFlags),
			a_parameters.presets[0], a_parameters.presets[1], a_parameters.presets[2], a_parameters.presets[3], a_parameters.presets[4]);
		return true;
	}

	bool D3D12Backend::Evaluate(ID3D12GraphicsCommandList* a_commandList, const D3D12Parameters& a_parameters, bool* a_recreated)
	{
		if (a_recreated) {
			*a_recreated = false;
		}
		lastResult_ = NVSDK_NGX_Result_Success;
		if (unavailable_ || !a_commandList || !a_parameters.color || !a_parameters.output ||
			!a_parameters.motionVectors || !a_parameters.depth ||
			!a_parameters.renderWidth || !a_parameters.renderHeight ||
			!a_parameters.outputWidth || !a_parameters.outputHeight) {
			return false;
		}
		if (!BindSharedOperations()) {
			return false;
		}

		if (NeedsRecreation(a_parameters)) {
			const bool hadFeature = feature_ != nullptr;
			// Same contract as Streamline's plugin: the old handle may still be
			// referenced by in-flight lists, so its release is deferred.
			RetireCurrent();
			if (!CreateFeature(a_commandList, a_parameters)) {
				return false;
			}
			if (a_recreated) {
				*a_recreated = hadFeature;
			}
		}

		auto* p = parameters_;
		const bool reset = a_parameters.reset || forceReset_;
		p->Set(NVSDK_NGX_Parameter_Color, a_parameters.color);
		p->Set(NVSDK_NGX_Parameter_Output, a_parameters.output);
		p->Set(NVSDK_NGX_Parameter_MotionVectors, a_parameters.motionVectors);
		p->Set(NVSDK_NGX_Parameter_Depth, a_parameters.depth);
		p->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, a_parameters.animatedTextureMask);
		p->Set(NVSDK_NGX_Parameter_TransparencyMask, static_cast<ID3D12Resource*>(nullptr));
		p->Set(NVSDK_NGX_Parameter_ExposureTexture, static_cast<ID3D12Resource*>(nullptr));
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, static_cast<ID3D12Resource*>(nullptr));
		p->Set(NVSDK_NGX_Parameter_RayTracingHitDistance, static_cast<ID3D12Resource*>(nullptr));
		p->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, static_cast<ID3D12Resource*>(nullptr));
		p->Set(NVSDK_NGX_Parameter_IsParticleMask, static_cast<ID3D12Resource*>(nullptr));
		p->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
		p->Set(NVSDK_NGX_Parameter_MV_Scale_X, a_parameters.motionVectorScaleX);
		p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, a_parameters.motionVectorScaleY);
		p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, a_parameters.jitterX);
		p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, a_parameters.jitterY);
		p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, a_parameters.preExposure);
		p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, a_parameters.exposureScale);
		p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
		p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, a_parameters.renderWidth);
		p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, a_parameters.renderHeight);
		p->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, 0);
		p->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, 0);

		// Plugin resources rest in COMMON between passes (Streamline used the
		// same declared state and transitioned internally).
		constexpr auto readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		D3D12_RESOURCE_BARRIER before[5]{};
		UINT count = 0;
		before[count++] = CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.color, D3D12_RESOURCE_STATE_COMMON, readState);
		before[count++] = CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.motionVectors, D3D12_RESOURCE_STATE_COMMON, readState);
		before[count++] = CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.depth, D3D12_RESOURCE_STATE_COMMON, readState);
		before[count++] = CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (a_parameters.animatedTextureMask) {
			before[count++] = CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.animatedTextureMask, D3D12_RESOURCE_STATE_COMMON, readState);
		}
		a_commandList->ResourceBarrier(count, before);
		const auto result = evaluateFeature_(a_commandList, feature_, p, nullptr);
		D3D12_RESOURCE_BARRIER after[5]{};
		for (UINT i = 0; i < count; ++i) {
			after[i] = before[i];
			std::swap(after[i].Transition.StateBefore, after[i].Transition.StateAfter);
		}
		a_commandList->ResourceBarrier(count, after);

		lastResult_ = result;
		if (NVSDK_NGX_FAILED(result)) {
			logger::warn(
				"[DLSS-SR Direct] EvaluateFeature(SuperSampling) failed result=0x{:08X} {}{} render={}x{} output={}x{} reset={}; feature will be recreated on retry",
				static_cast<std::uint32_t>(result), DescribeResult(result),
				result == NVSDK_NGX_Result_FAIL_OutOfGPUMemory ? " (NGX out of GPU memory)" : "",
				a_parameters.renderWidth, a_parameters.renderHeight, a_parameters.outputWidth, a_parameters.outputHeight, reset);
			RetireCurrent();
			return false;
		}
		forceReset_ = false;
		return true;
	}

	void D3D12Backend::RetireCurrent()
	{
		if (feature_) {
			retired_.push_back({ feature_, nullptr, frame_ + kRetireFrames });
			feature_ = nullptr;
		}
		configuration_ = {};
		forceReset_ = true;
	}

	bool D3D12Backend::ReleaseHandles(NVSDK_NGX_Handle* a_feature, NVSDK_NGX_Parameter* a_parameters)
	{
		// Process teardown may already have unloaded the core.
		HMODULE core = nullptr;
		if (!releaseFeature_ || !GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(releaseFeature_), &core) || !core) {
			return false;
		}
		bool ok = true;
		if (a_feature && releaseFeature_) {
			const auto result = releaseFeature_(a_feature);
			if (NVSDK_NGX_FAILED(result)) {
				logger::warn("[DLSS-SR Direct] ReleaseFeature failed result=0x{:08X} {}", static_cast<std::uint32_t>(result), DescribeResult(result));
				ok = false;
			}
		}
		if (a_parameters && destroyParameters_) {
			const auto result = destroyParameters_(a_parameters);
			if (NVSDK_NGX_FAILED(result)) {
				logger::warn("[DLSS-SR Direct] DestroyParameters failed result=0x{:08X} {}", static_cast<std::uint32_t>(result), DescribeResult(result));
				ok = false;
			}
		}
		return ok;
	}

	void D3D12Backend::AdvanceFrame(bool a_retireCurrent)
	{
		++frame_;
		if (a_retireCurrent && feature_) {
			logger::info("[DLSS-SR Direct] Direct SR disabled; retiring the NGX SuperSampling feature");
			RetireCurrent();
		}
		for (auto it = retired_.begin(); it != retired_.end();) {
			if (frame_ >= it->releaseAfterFrame) {
				ReleaseHandles(it->feature, it->parameters);
				it = retired_.erase(it);
			} else {
				++it;
			}
		}
	}

	void D3D12Backend::ReleaseAll()
	{
		for (const auto& retired : retired_) {
			ReleaseHandles(retired.feature, retired.parameters);
		}
		retired_.clear();
		if (feature_ || parameters_) {
			ReleaseHandles(feature_, parameters_);
		}
		feature_ = nullptr;
		parameters_ = nullptr;
		configuration_ = {};
		forceReset_ = true;
	}
}
