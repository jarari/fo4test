#include "nvngx_dlss_nr_private.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string_view>
#include <type_traits>

namespace
{
	using PFun_GetModuleFileNameW = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

	std::atomic<PFun_GetModuleFileNameW> g_originalGetModuleFileNameW = nullptr;
	std::atomic<HMODULE> g_spoofedCallerModule = nullptr;

	DWORD WINAPI NVSDK_NGX_GetModuleFileNameW_Proxy(HMODULE a_module, LPWSTR a_filename, DWORD a_size)
	{
		if (a_module == g_spoofedCallerModule.load(std::memory_order_acquire)) {
			constexpr wchar_t kSpoofedName[] = L"nvngx.dll";
			constexpr auto kSpoofedNameLength = static_cast<DWORD>(std::size(kSpoofedName) - 1);
			if (!a_filename || a_size == 0) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return 0;
			}

			const auto copyLength = std::min(kSpoofedNameLength, a_size - 1);
			std::wmemcpy(a_filename, kSpoofedName, copyLength);
			a_filename[copyLength] = L'\0';
			if (copyLength != kSpoofedNameLength) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return a_size;
			}
			return kSpoofedNameLength;
		}

		const auto original = g_originalGetModuleFileNameW.load(std::memory_order_acquire);
		return original ? original(a_module, a_filename, a_size) : 0;
	}

	bool WritePointer(std::uintptr_t* a_slot, std::uintptr_t a_value)
	{
		if (!a_slot) {
			return false;
		}

		DWORD previousProtection = 0;
		if (!VirtualProtect(a_slot, sizeof(*a_slot), PAGE_READWRITE, &previousProtection)) {
			return false;
		}

		InterlockedExchangePointer(
			reinterpret_cast<void* volatile*>(a_slot),
			reinterpret_cast<void*>(a_value));
		DWORD ignoredProtection = 0;
		VirtualProtect(a_slot, sizeof(*a_slot), previousProtection, &ignoredProtection);
		FlushInstructionCache(GetCurrentProcess(), a_slot, sizeof(*a_slot));
		return true;
	}

	float ScalingRatio(bool a_upscaling)
	{
		// The signed 310.8 runtime reads this parameter but fixes both feature
		// creation and evaluation to a native-resolution (1:1) network.
		(void)a_upscaling;
		return 1.0f;
	}

	NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_DLSSNR_ComputeScalingRatio(NVSDK_NGX_Parameter* a_parameters)
	{
		if (!a_parameters) {
			return NVSDK_NGX_Result_FAIL_InvalidParameter;
		}

		int upscaling = 0;
		const auto getResult = a_parameters->Get(NVSDK_NGX_Parameter_DLSSNR_Upscaling, &upscaling);
		if (NVSDK_NGX_FAILED(getResult)) {
			return getResult;
		}

		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ScalingRatio, ScalingRatio(upscaling != 0));
		return NVSDK_NGX_Result_Success;
	}

	bool IsSupportedPerformanceMode(std::uint32_t a_performanceMode)
	{
		return a_performanceMode == 1 ||
			a_performanceMode == 2 ||
			a_performanceMode == 3 ||
			a_performanceMode == 4 ||
			a_performanceMode == 6;
	}

	bool IsNGXSuccess(NVSDK_NGX_Result a_result)
	{
		return NVSDK_NGX_SUCCEED(a_result);
	}
}

namespace nvngx::dlss_nr
{
	D3D12Backend::~D3D12Backend()
	{
		Shutdown();
	}

	void D3D12Backend::SetRuntimeDirectory(const std::filesystem::path& a_runtimeDirectory)
	{
		if (runtimeDirectory_ == a_runtimeDirectory) {
			return;
		}

		Shutdown();
		runtimeDirectory_ = a_runtimeDirectory;
	}

	bool D3D12Backend::LoadRuntime()
	{
		if (runtime_) {
			return true;
		}

		const auto runtimePath = runtimeDirectory_ / L"nvngx_dlssnr.dll";
		if (runtimeDirectory_.empty() || !std::filesystem::exists(runtimePath)) {
			logger::warn("[DLSS-NR Direct] Runtime {} is missing", runtimePath.string());
			return false;
		}

		runtime_ = LoadLibraryW(runtimePath.c_str());
		if (!runtime_) {
			logger::warn(
				"[DLSS-NR Direct] Could not load {} error=0x{:08X}",
				runtimePath.string(),
				GetLastError());
			return false;
		}

		const auto resolve = [](HMODULE a_module, auto& a_function, const char* a_name) {
			a_function = reinterpret_cast<std::remove_reference_t<decltype(a_function)>>(GetProcAddress(a_module, a_name));
			return a_function != nullptr;
		};

		PFun_AllocateParameters runtimeAllocateParameters = nullptr;
		PFun_DestroyParameters runtimeDestroyParameters = nullptr;

		bool requiredExportsAvailable = true;
		const auto require = [&](auto& a_function, const char* a_name) {
			if (!resolve(runtime_, a_function, a_name)) {
				logger::warn("[DLSS-NR Direct] Runtime export {} is missing", a_name);
				requiredExportsAvailable = false;
			}
		};

		require(initExt_, "NVSDK_NGX_D3D12_Init_Ext");
		resolve(runtime_, runtimeAllocateParameters, "NVSDK_NGX_D3D12_AllocateParameters");
		resolve(runtime_, runtimeDestroyParameters, "NVSDK_NGX_D3D12_DestroyParameters");
		require(snippetCreateFeature_, "NVSDK_NGX_D3D12_CreateFeature");
		require(snippetEvaluateFeature_, "NVSDK_NGX_D3D12_EvaluateFeature");
		require(snippetReleaseFeature_, "NVSDK_NGX_D3D12_ReleaseFeature");
		require(shutdown_, "NVSDK_NGX_D3D12_Shutdown1");
		if (!requiredExportsAvailable) {
			return false;
		}

		// RenoDX obtains the public feature-operation entry points from the first
		// already-loaded NGX core/feature module. Newer signed DLSS-NR snippets do
		// not necessarily export AllocateParameters or DestroyParameters even
		// though they still own Init_Ext and Shutdown1.
		constexpr const wchar_t* kSharedNGXModuleNames[] = {
			L"_nvngx.dll",
			L"nvngx.dll",
			L"nvngx_dlss.dll",
			L"nvngx_dlssd.dll"
		};
		for (const auto* moduleName : kSharedNGXModuleNames) {
			operationRuntime_ = GetModuleHandleW(moduleName);
			if (operationRuntime_) {
				break;
			}
		}

		if (operationRuntime_) {
			PFun_AllocateParameters sharedAllocateParameters = nullptr;
			PFun_DestroyParameters sharedDestroyParameters = nullptr;
			PFun_CreateFeature sharedCreateFeature = nullptr;
			PFun_EvaluateFeature sharedEvaluateFeature = nullptr;
			PFun_ReleaseFeature sharedReleaseFeature = nullptr;
			const auto sharedOperationsAvailable =
				resolve(operationRuntime_, sharedAllocateParameters, "NVSDK_NGX_D3D12_AllocateParameters") &
				resolve(operationRuntime_, sharedDestroyParameters, "NVSDK_NGX_D3D12_DestroyParameters") &
				resolve(operationRuntime_, sharedCreateFeature, "NVSDK_NGX_D3D12_CreateFeature") &
				resolve(operationRuntime_, sharedEvaluateFeature, "NVSDK_NGX_D3D12_EvaluateFeature") &
				resolve(operationRuntime_, sharedReleaseFeature, "NVSDK_NGX_D3D12_ReleaseFeature");
			if (sharedOperationsAvailable) {
				allocateParameters_ = sharedAllocateParameters;
				destroyParameters_ = sharedDestroyParameters;
				createFeature_ = sharedCreateFeature;
				evaluateFeature_ = sharedEvaluateFeature;
				releaseFeature_ = sharedReleaseFeature;

				wchar_t modulePath[MAX_PATH]{};
				GetModuleFileNameW(operationRuntime_, modulePath, static_cast<DWORD>(std::size(modulePath)));
				logger::info("[DLSS-NR Direct] Using shared NGX feature operations from {}", std::filesystem::path(modulePath).string());
				return true;
			}
		}

		const auto runtimeOperationsAvailable =
			runtimeAllocateParameters &&
			runtimeDestroyParameters &&
			snippetCreateFeature_ &&
			snippetEvaluateFeature_ &&
			snippetReleaseFeature_;
		if (runtimeOperationsAvailable) {
			operationRuntime_ = runtime_;
			allocateParameters_ = runtimeAllocateParameters;
			destroyParameters_ = runtimeDestroyParameters;
			createFeature_ = snippetCreateFeature_;
			evaluateFeature_ = snippetEvaluateFeature_;
			releaseFeature_ = snippetReleaseFeature_;
			logger::info("[DLSS-NR Direct] Using feature operations exported by nvngx_dlssnr.dll");
			return true;
		}

		logger::warn(
			"[DLSS-NR Direct] No loaded NGX module exposes the complete D3D12 feature operation set required by nvngx_dlssnr.dll");
		operationRuntime_ = nullptr;
		return false;
	}

	bool D3D12Backend::InstallModuleNameHook()
	{
		if (moduleNameImportSlot_) {
			return true;
		}
		if (!runtime_) {
			return false;
		}

		HMODULE callerModule = nullptr;
		if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&NVSDK_NGX_GetModuleFileNameW_Proxy),
				&callerModule) ||
			!callerModule) {
			logger::warn("[DLSS-NR Direct] Could not identify the calling plugin module");
			return false;
		}

		const auto imageBase = reinterpret_cast<std::uintptr_t>(runtime_);
		const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
			logger::warn("[DLSS-NR Direct] Runtime has an invalid DOS header");
			return false;
		}

		const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE || ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
			logger::warn("[DLSS-NR Direct] Runtime has an invalid PE header");
			return false;
		}

		const auto imageSize = static_cast<std::uintptr_t>(ntHeaders->OptionalHeader.SizeOfImage);
		const auto& importDirectory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!importDirectory.VirtualAddress || !importDirectory.Size || importDirectory.VirtualAddress >= imageSize) {
			logger::warn("[DLSS-NR Direct] Runtime has no import directory");
			return false;
		}

		const auto isRvaInImage = [imageSize](std::uintptr_t a_rva, std::uintptr_t a_size = 1) {
			return a_rva < imageSize && a_size <= imageSize - a_rva;
		};
		auto* importDescriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(imageBase + importDirectory.VirtualAddress);
		const auto importEnd = importDirectory.VirtualAddress + importDirectory.Size;
		for (auto descriptorRva = static_cast<std::uintptr_t>(importDirectory.VirtualAddress);
			isRvaInImage(descriptorRva, sizeof(IMAGE_IMPORT_DESCRIPTOR)) && descriptorRva < importEnd && importDescriptor->Name;
			descriptorRva += sizeof(IMAGE_IMPORT_DESCRIPTOR), ++importDescriptor) {
			if (!isRvaInImage(importDescriptor->FirstThunk, sizeof(IMAGE_THUNK_DATA64))) {
				continue;
			}

			auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(imageBase + importDescriptor->FirstThunk);
			auto* nameThunk = importDescriptor->OriginalFirstThunk && isRvaInImage(importDescriptor->OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA64)) ?
				reinterpret_cast<IMAGE_THUNK_DATA64*>(imageBase + importDescriptor->OriginalFirstThunk) : nullptr;

			for (std::size_t index = 0;; ++index) {
				const auto addressThunkRva = static_cast<std::uintptr_t>(importDescriptor->FirstThunk) + index * sizeof(IMAGE_THUNK_DATA64);
				if (!isRvaInImage(addressThunkRva, sizeof(IMAGE_THUNK_DATA64)) || !addressThunk[index].u1.Function) {
					break;
				}

				bool matches = false;
				if (nameThunk) {
					const auto nameThunkRva = static_cast<std::uintptr_t>(importDescriptor->OriginalFirstThunk) + index * sizeof(IMAGE_THUNK_DATA64);
					if (!isRvaInImage(nameThunkRva, sizeof(IMAGE_THUNK_DATA64)) || !nameThunk[index].u1.AddressOfData) {
						break;
					}
					if (!IMAGE_SNAP_BY_ORDINAL64(nameThunk[index].u1.Ordinal) &&
						isRvaInImage(nameThunk[index].u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME))) {
						const auto* importByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(imageBase + nameThunk[index].u1.AddressOfData);
						matches = std::string_view(reinterpret_cast<const char*>(importByName->Name)) == "GetModuleFileNameW";
					}
				} else {
					matches = addressThunk[index].u1.Function == reinterpret_cast<std::uintptr_t>(&GetModuleFileNameW);
				}

				if (!matches) {
					continue;
				}

				moduleNameImportSlot_ = reinterpret_cast<std::uintptr_t*>(&addressThunk[index].u1.Function);
				originalModuleNameImport_ = *moduleNameImportSlot_;
				spoofedCallerModule_ = callerModule;
				g_originalGetModuleFileNameW.store(
					reinterpret_cast<PFun_GetModuleFileNameW>(originalModuleNameImport_),
					std::memory_order_release);
				g_spoofedCallerModule.store(spoofedCallerModule_, std::memory_order_release);
				if (!WritePointer(moduleNameImportSlot_, reinterpret_cast<std::uintptr_t>(&NVSDK_NGX_GetModuleFileNameW_Proxy))) {
					g_spoofedCallerModule.store(nullptr, std::memory_order_release);
					g_originalGetModuleFileNameW.store(nullptr, std::memory_order_release);
					moduleNameImportSlot_ = nullptr;
					originalModuleNameImport_ = 0;
					spoofedCallerModule_ = nullptr;
					logger::warn("[DLSS-NR Direct] Could not patch the runtime module-name import");
					return false;
				}

				logger::info("[DLSS-NR Direct] Installed scoped nvngx.dll caller-name shim");
				return true;
			}
		}

		logger::warn("[DLSS-NR Direct] Runtime does not import GetModuleFileNameW");
		return false;
	}

	void D3D12Backend::RestoreModuleNameHook()
	{
		if (moduleNameImportSlot_ && originalModuleNameImport_) {
			const auto proxy = reinterpret_cast<std::uintptr_t>(&NVSDK_NGX_GetModuleFileNameW_Proxy);
			if (*moduleNameImportSlot_ == proxy) {
				WritePointer(moduleNameImportSlot_, originalModuleNameImport_);
			}
		}

		g_spoofedCallerModule.store(nullptr, std::memory_order_release);
		g_originalGetModuleFileNameW.store(nullptr, std::memory_order_release);
		moduleNameImportSlot_ = nullptr;
		originalModuleNameImport_ = 0;
		spoofedCallerModule_ = nullptr;
	}

	bool D3D12Backend::Prepare(ID3D12Device* a_device)
	{
		if (initialized_) {
			return device_ == a_device;
		}
		if (initializationAttempted_ || !a_device) {
			return false;
		}
		initializationAttempted_ = true;

		device_ = a_device;
		device_->AddRef();
		if (!LoadRuntime() || !InstallModuleNameHook()) {
			if (runtime_) {
				RestoreModuleNameHook();
				FreeLibrary(runtime_);
				runtime_ = nullptr;
			}
			device_->Release();
			device_ = nullptr;
			return false;
		}

		const auto result = initExt_(
			NVSDK_NGX_DLSSNR_ApplicationId,
			runtimeDirectory_.c_str(),
			device_,
			NVSDK_NGX_DLSSNR_SDKVersion,
			nullptr);
		if (!IsNGXSuccess(result)) {
			logger::warn("[DLSS-NR Direct] NVSDK_NGX_D3D12_Init_Ext failed result=0x{:08X}", static_cast<std::uint32_t>(result));
			RestoreModuleNameHook();
			FreeLibrary(runtime_);
			runtime_ = nullptr;
			device_->Release();
			device_ = nullptr;
			return false;
		}

		initialized_ = true;
		logger::info(
			"[DLSS-NR Direct] Initialized nvngx_dlssnr.dll feature={} appId={} sdkVersion={}",
			static_cast<std::uint32_t>(NVSDK_NGX_Feature_DLSSNR),
			NVSDK_NGX_DLSSNR_ApplicationId,
			static_cast<std::uint32_t>(NVSDK_NGX_DLSSNR_SDKVersion));
		return true;
	}

	void D3D12Backend::SetCreationParameters(const D3D12EvaluationParameters& a_parameters)
	{
		const auto upscaling =
			a_parameters.inputWidth != a_parameters.outputWidth ||
			a_parameters.inputHeight != a_parameters.outputHeight;
		const auto ratio = ScalingRatio(upscaling);
		constexpr auto createFlags = static_cast<int>(
			NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
			NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);

		parameters_->Set(NVSDK_NGX_Parameter_Width, a_parameters.inputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_Height, a_parameters.inputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_OutWidth, a_parameters.outputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_OutHeight, a_parameters.outputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Width, a_parameters.outputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Height, a_parameters.outputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_InputWidth, a_parameters.inputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_InputHeight, a_parameters.inputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_OutputWidth, a_parameters.outputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_OutputHeight, a_parameters.outputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Output_Width, a_parameters.outputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Output_Height, a_parameters.outputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_ScalingRatio, ratio);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Scale, ratio);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Upscaling, static_cast<int>(upscaling));
		parameters_->Set(
			NVSDK_NGX_Parameter_DLSSNR_ComputeScalingRatioCallback,
			reinterpret_cast<void*>(&NVSDK_NGX_DLSSNR_ComputeScalingRatio));
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, a_parameters.options.preset);
		parameters_->Set(NVSDK_NGX_Parameter_PerfQualityValue, a_parameters.options.performanceMode - 1);
		parameters_->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, createFlags);
		parameters_->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
		parameters_->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
	}

	bool D3D12Backend::EnsureFeature(ID3D12GraphicsCommandList* a_commandList, const D3D12EvaluationParameters& a_parameters)
	{
		if (!IsSupportedPerformanceMode(a_parameters.options.performanceMode)) {
			logger::warn("[DLSS-NR Direct] Performance mode {} is unsupported", a_parameters.options.performanceMode);
			return false;
		}

		if (NeedsFeatureRecreation(a_parameters)) {
			ReleaseFeature();
		}
		if (feature_) {
			return true;
		}

		const auto allocateResult = allocateParameters_(&parameters_);
		if (!IsNGXSuccess(allocateResult) || !parameters_) {
			logger::warn("[DLSS-NR Direct] AllocateParameters failed result=0x{:08X}", static_cast<std::uint32_t>(allocateResult));
			parameters_ = nullptr;
			return false;
		}

		SetCreationParameters(a_parameters);
		auto createResult = createFeature_(a_commandList, NVSDK_NGX_Feature_DLSSNR, parameters_, &feature_);
		auto snippetCreateResult = createFeature_ == snippetCreateFeature_ ? createResult : NVSDK_NGX_Result_Success;
		activeEvaluateFeature_ = evaluateFeature_;
		activeReleaseFeature_ = releaseFeature_;
		if ((!IsNGXSuccess(createResult) || !feature_) && createFeature_ != snippetCreateFeature_) {
			// Match RenoDX's creation sequence: the shared NGX core gets the first
			// chance, then feature 18 is created through the signed snippet when the
			// core reports UnableToInitializeFeature. Evaluation never retries across
			// backends; it is bound to the creator selected here.
			feature_ = nullptr;
			snippetCreateResult = snippetCreateFeature_(
				a_commandList,
				NVSDK_NGX_Feature_DLSSNR,
				parameters_,
				&feature_);
			if (IsNGXSuccess(snippetCreateResult) && feature_) {
				activeEvaluateFeature_ = snippetEvaluateFeature_;
				activeReleaseFeature_ = snippetReleaseFeature_;
				logger::info(
					"[DLSS-NR Direct] Shared NGX CreateFeature returned 0x{:08X}; feature 18 was created by nvngx_dlssnr.dll",
					static_cast<std::uint32_t>(createResult));
				createResult = snippetCreateResult;
			}
		}
		if (!IsNGXSuccess(createResult) || !feature_) {
			logger::warn(
				"[DLSS-NR Direct] CreateFeature failed primaryResult=0x{:08X} snippetResult=0x{:08X} input={}x{} output={}x{} performanceMode={} preset={}",
				static_cast<std::uint32_t>(createResult),
				static_cast<std::uint32_t>(snippetCreateResult),
				a_parameters.inputWidth,
				a_parameters.inputHeight,
				a_parameters.outputWidth,
				a_parameters.outputHeight,
				a_parameters.options.performanceMode,
				a_parameters.options.preset);
			feature_ = nullptr;
			activeEvaluateFeature_ = nullptr;
			activeReleaseFeature_ = nullptr;
			destroyParameters_(parameters_);
			parameters_ = nullptr;
			return false;
		}

		featureInputWidth_ = a_parameters.inputWidth;
		featureInputHeight_ = a_parameters.inputHeight;
		featureOutputWidth_ = a_parameters.outputWidth;
		featureOutputHeight_ = a_parameters.outputHeight;
		featurePerformanceMode_ = a_parameters.options.performanceMode;
		featurePreset_ = a_parameters.options.preset;
		forceReset_ = true;
		logger::info(
			"[DLSS-NR Direct] Created NGX feature 18 input={}x{} output={}x{} performanceMode={} preset={}",
			featureInputWidth_,
			featureInputHeight_,
			featureOutputWidth_,
			featureOutputHeight_,
			featurePerformanceMode_,
			featurePreset_);
		return true;
	}

	void D3D12Backend::SetEvaluationParameters(const D3D12EvaluationParameters& a_parameters, bool a_reset)
	{
		const auto upscaling =
			a_parameters.inputWidth != a_parameters.outputWidth ||
			a_parameters.inputHeight != a_parameters.outputHeight;
		const auto ratio = ScalingRatio(upscaling);

		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Color, a_parameters.color);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Output, a_parameters.output);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_MVec, a_parameters.motionVectors);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Depth, a_parameters.depth);
		parameters_->Set(NVSDK_NGX_Parameter_Width, a_parameters.inputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_Height, a_parameters.inputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_OutWidth, a_parameters.outputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_OutHeight, a_parameters.outputHeight);

		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, 0u);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY, 0u);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, a_parameters.inputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, a_parameters.inputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, 0u);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY, 0u);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, a_parameters.guideWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, a_parameters.guideHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, a_parameters.motionVectorScaleX);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, a_parameters.motionVectorScaleY);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, 0u);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, 0u);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, a_parameters.guideWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, a_parameters.guideHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, static_cast<int>(a_parameters.depthInverted));
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, 0u);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY, 0u);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, a_parameters.outputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, a_parameters.outputHeight);

		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_InputWidth, a_parameters.inputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_InputHeight, a_parameters.inputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_OutputWidth, a_parameters.outputWidth);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_OutputHeight, a_parameters.outputHeight);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_ScalingRatio, ratio);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Scale, ratio);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Upscaling, static_cast<int>(upscaling));
		parameters_->Set(
			NVSDK_NGX_Parameter_DLSSNR_ComputeScalingRatioCallback,
			reinterpret_cast<void*>(&NVSDK_NGX_DLSSNR_ComputeScalingRatio));
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Reset, static_cast<int>(a_reset));
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Intensity, a_parameters.options.intensity);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, a_parameters.options.localToneStrength);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, a_parameters.options.localStructureStrength);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, a_parameters.options.skinStructureStrength);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, static_cast<int>(a_parameters.options.useAutoMask));
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_Style, a_parameters.options.style);
		parameters_->Set(NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);
		parameters_->Set("DLSS.Indicator.Invert.X.Axis", 0);
		parameters_->Set("DLSS.Indicator.Invert.Y.Axis", 0);
	}

	bool D3D12Backend::NeedsFeatureRecreation(const D3D12EvaluationParameters& a_parameters) const
	{
		return feature_ &&
			(featureInputWidth_ != a_parameters.inputWidth ||
				featureInputHeight_ != a_parameters.inputHeight ||
				featureOutputWidth_ != a_parameters.outputWidth ||
				featureOutputHeight_ != a_parameters.outputHeight ||
				featurePerformanceMode_ != a_parameters.options.performanceMode ||
				featurePreset_ != a_parameters.options.preset);
	}

	bool D3D12Backend::Evaluate(ID3D12GraphicsCommandList* a_commandList, const D3D12EvaluationParameters& a_parameters)
	{
		if (!a_commandList || !a_parameters.color || !a_parameters.output ||
			!a_parameters.motionVectors || !a_parameters.depth ||
			!a_parameters.inputWidth || !a_parameters.inputHeight ||
			!a_parameters.outputWidth || !a_parameters.outputHeight ||
			!a_parameters.guideWidth || !a_parameters.guideHeight) {
			return false;
		}
		const auto sameFailedConfiguration =
			failedInputWidth_ == a_parameters.inputWidth &&
			failedInputHeight_ == a_parameters.inputHeight &&
			failedOutputWidth_ == a_parameters.outputWidth &&
			failedOutputHeight_ == a_parameters.outputHeight &&
			failedGuideWidth_ == a_parameters.guideWidth &&
			failedGuideHeight_ == a_parameters.guideHeight &&
			failedPerformanceMode_ == a_parameters.options.performanceMode &&
			failedPreset_ == a_parameters.options.preset;
		if (failureLatched_ && sameFailedConfiguration) {
			return false;
		}
		if (failureLatched_) {
			failureLatched_ = false;
		}
		if (!initialized_) {
			ID3D12Device* device = nullptr;
			if (FAILED(a_commandList->GetDevice(IID_PPV_ARGS(&device))) || !device) {
				logger::warn("[DLSS-NR Direct] Could not obtain the D3D12 device");
				return false;
			}
			const auto prepared = Prepare(device);
			device->Release();
			if (!prepared) {
				return false;
			}
		}
		if (!EnsureFeature(a_commandList, a_parameters)) {
			failureLatched_ = true;
			failedInputWidth_ = a_parameters.inputWidth;
			failedInputHeight_ = a_parameters.inputHeight;
			failedOutputWidth_ = a_parameters.outputWidth;
			failedOutputHeight_ = a_parameters.outputHeight;
			failedGuideWidth_ = a_parameters.guideWidth;
			failedGuideHeight_ = a_parameters.guideHeight;
			failedPerformanceMode_ = a_parameters.options.performanceMode;
			failedPreset_ = a_parameters.options.preset;
			return false;
		}

		const auto reset = forceReset_ || a_parameters.reset;
		SetEvaluationParameters(a_parameters, reset);

		D3D12_RESOURCE_BARRIER beforeEvaluation[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.motionVectors, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};
		a_commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeEvaluation)), beforeEvaluation);
		const auto evaluateResult = activeEvaluateFeature_(a_commandList, feature_, parameters_, nullptr);
		D3D12_RESOURCE_BARRIER afterEvaluation[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.motionVectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_parameters.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
		};
		a_commandList->ResourceBarrier(static_cast<UINT>(std::size(afterEvaluation)), afterEvaluation);

		if (!IsNGXSuccess(evaluateResult)) {
			const auto colorDesc = a_parameters.color->GetDesc();
			const auto outputDesc = a_parameters.output->GetDesc();
			logger::warn(
				"[DLSS-NR Direct] EvaluateFeature failed result=0x{:08X} color={}x{} guides={}x{} output={}x{} colorResource={}x{} outputResource={}x{} performanceMode={} reset={}; disabling this configuration",
				static_cast<std::uint32_t>(evaluateResult),
				a_parameters.inputWidth,
				a_parameters.inputHeight,
				a_parameters.guideWidth,
				a_parameters.guideHeight,
				a_parameters.outputWidth,
				a_parameters.outputHeight,
				static_cast<std::uint32_t>(colorDesc.Width),
				colorDesc.Height,
				static_cast<std::uint32_t>(outputDesc.Width),
				outputDesc.Height,
				a_parameters.options.performanceMode,
				reset);
			// The failed call may still have recorded commands referencing the NGX
			// feature. Keep it alive until a queue-drained recreation or shutdown.
			failureLatched_ = true;
			failedInputWidth_ = a_parameters.inputWidth;
			failedInputHeight_ = a_parameters.inputHeight;
			failedOutputWidth_ = a_parameters.outputWidth;
			failedOutputHeight_ = a_parameters.outputHeight;
			failedGuideWidth_ = a_parameters.guideWidth;
			failedGuideHeight_ = a_parameters.guideHeight;
			failedPerformanceMode_ = a_parameters.options.performanceMode;
			failedPreset_ = a_parameters.options.preset;
			return false;
		}

		forceReset_ = false;
		return true;
	}

	void D3D12Backend::RequestReset()
	{
		forceReset_ = true;
	}

	void D3D12Backend::ReleaseFeature()
	{
		if (feature_ && activeReleaseFeature_) {
			const auto result = activeReleaseFeature_(feature_);
			if (!IsNGXSuccess(result)) {
				logger::warn("[DLSS-NR Direct] ReleaseFeature failed result=0x{:08X}", static_cast<std::uint32_t>(result));
			}
		}
		feature_ = nullptr;
		activeEvaluateFeature_ = nullptr;
		activeReleaseFeature_ = nullptr;

		if (parameters_ && destroyParameters_) {
			const auto result = destroyParameters_(parameters_);
			if (!IsNGXSuccess(result)) {
				logger::warn("[DLSS-NR Direct] DestroyParameters failed result=0x{:08X}", static_cast<std::uint32_t>(result));
			}
		}
		parameters_ = nullptr;
		featureInputWidth_ = 0;
		featureInputHeight_ = 0;
		featureOutputWidth_ = 0;
		featureOutputHeight_ = 0;
		featurePerformanceMode_ = 0;
		featurePreset_ = 0;
		failureLatched_ = false;
		failedInputWidth_ = 0;
		failedInputHeight_ = 0;
		failedOutputWidth_ = 0;
		failedOutputHeight_ = 0;
		failedGuideWidth_ = 0;
		failedGuideHeight_ = 0;
		failedPerformanceMode_ = 0;
		failedPreset_ = 0;
		forceReset_ = true;
	}

	void D3D12Backend::Shutdown()
	{
		ReleaseFeature();
		if (initialized_ && shutdown_) {
			const auto result = shutdown_(device_);
			if (!IsNGXSuccess(result)) {
				logger::warn("[DLSS-NR Direct] Shutdown failed result=0x{:08X}", static_cast<std::uint32_t>(result));
			}
		}
		initialized_ = false;

		RestoreModuleNameHook();
		if (runtime_) {
			FreeLibrary(runtime_);
			runtime_ = nullptr;
		}
		operationRuntime_ = nullptr;
		if (device_) {
			device_->Release();
			device_ = nullptr;
		}

		initExt_ = nullptr;
		allocateParameters_ = nullptr;
		destroyParameters_ = nullptr;
		createFeature_ = nullptr;
		evaluateFeature_ = nullptr;
		releaseFeature_ = nullptr;
		snippetCreateFeature_ = nullptr;
		snippetEvaluateFeature_ = nullptr;
		snippetReleaseFeature_ = nullptr;
		activeEvaluateFeature_ = nullptr;
		activeReleaseFeature_ = nullptr;
		shutdown_ = nullptr;
		initializationAttempted_ = false;
	}
}
