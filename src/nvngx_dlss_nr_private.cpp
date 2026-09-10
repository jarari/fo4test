#include "nvngx_dlss_nr_private.h"
#ifdef UPSCALING_NR_CAPTURE
#include "NRDiagnosticCapture.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string_view>
#include <type_traits>

namespace
{
	using PFun_GetModuleFileNameW = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

	std::atomic<PFun_GetModuleFileNameW> g_originalGetModuleFileNameW = nullptr;
	std::atomic<HMODULE> g_spoofedCallerModule = nullptr;

	// NGX allocation callbacks have no user-data argument. Retain the owning
	// backend's device until its features and runtime have finished shutdown.
	std::mutex g_nrAllocationMutex;
	nvngx::dlss_nr::D3D12Backend* g_nrAllocationOwner = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Device> g_nrAllocationDevice;

	void NVSDK_CONV AllocateNRResource(D3D12_RESOURCE_DESC* a_desc, int a_state,
		D3D12_HEAP_PROPERTIES* a_heap, ID3D12Resource** a_output)
	{
		if (!a_output) { return; }
		*a_output = nullptr;
		Microsoft::WRL::ComPtr<ID3D12Device> device;
		{
			std::scoped_lock lock(g_nrAllocationMutex);
			device = g_nrAllocationDevice;
		}
		if (!device || !a_desc || !a_heap) { return; }
		// Match NGX's committed allocation, including its heap, node masks,
		// resource flags and initial state. Upload/readback allocations stay as-is.
		const auto result = device->CreateCommittedResource(a_heap, D3D12_HEAP_FLAG_NONE,
			a_desc, static_cast<D3D12_RESOURCE_STATES>(a_state), nullptr, IID_PPV_ARGS(a_output));
		if (FAILED(result)) {
			logger::warn("[DLSS-NR Direct] Resource allocation failed hr=0x{:08X}", static_cast<uint32_t>(result));
			return;
		}
		if (a_heap->Type == D3D12_HEAP_TYPE_DEFAULT) {
			// The transition ETL ties a 146 MiB NR internal buffer's promotion
			// into local VRAM to evaluation dropping from ~68 ms to ~8 ms.
			// These repeatedly accessed working buffers should outlive cold game
			// allocations in VRAM. This is a residency hint, not a memory pin.
			Microsoft::WRL::ComPtr<ID3D12Device1> residencyDevice;
			if (SUCCEEDED(device.As(&residencyDevice))) {
				ID3D12Pageable* resource = *a_output;
				constexpr auto priority = D3D12_RESIDENCY_PRIORITY_MAXIMUM;
				const auto priorityResult = residencyDevice->SetResidencyPriority(1, &resource, &priority);
				if (FAILED(priorityResult)) {
					logger::warn("[DLSS-NR Direct] Residency priority failed hr=0x{:08X}", static_cast<uint32_t>(priorityResult));
				}
			}
		}
	}

	void NVSDK_CONV ReleaseNRResource(IUnknown* a_resource)
	{
		// Feature teardown already drains GPU use. Preserve immediate NR release.
		if (a_resource) { a_resource->Release(); }
	}

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
		if (runtime_ || device_) {
			return;  // Failed shutdown still owns the old runtime and its resources.
		}
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

	void D3D12Backend::SetCreationParameters(
		NVSDK_NGX_Parameter* a_parameters,
		const D3D12EvaluationParameters& a_evaluationParameters)
	{
		{
			std::scoped_lock lock(g_nrAllocationMutex);
			if (!g_nrAllocationOwner || g_nrAllocationOwner == this) {
				g_nrAllocationOwner = this;
				g_nrAllocationDevice = device_;
				a_parameters->Set(NVSDK_NGX_Parameter_ResourceAllocCallback, reinterpret_cast<void*>(&AllocateNRResource));
				a_parameters->Set(NVSDK_NGX_Parameter_ResourceReleaseCallback, reinterpret_cast<void*>(&ReleaseNRResource));
			}
		}
		const auto upscaling =
			a_evaluationParameters.inputWidth != a_evaluationParameters.outputWidth ||
			a_evaluationParameters.inputHeight != a_evaluationParameters.outputHeight;
		const auto ratio = ScalingRatio(upscaling);
		constexpr auto createFlags = static_cast<int>(
			NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
			NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);

		a_parameters->Set(NVSDK_NGX_Parameter_Width, a_evaluationParameters.inputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_Height, a_evaluationParameters.inputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_OutWidth, a_evaluationParameters.outputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_OutHeight, a_evaluationParameters.outputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Width, a_evaluationParameters.outputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Height, a_evaluationParameters.outputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_InputWidth, a_evaluationParameters.inputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_InputHeight, a_evaluationParameters.inputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputWidth, a_evaluationParameters.outputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputHeight, a_evaluationParameters.outputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Output_Width, a_evaluationParameters.outputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Output_Height, a_evaluationParameters.outputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ScalingRatio, ratio);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Scale, ratio);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Upscaling, static_cast<int>(upscaling));
		a_parameters->Set(
			NVSDK_NGX_Parameter_DLSSNR_ComputeScalingRatioCallback,
			reinterpret_cast<void*>(&NVSDK_NGX_DLSSNR_ComputeScalingRatio));
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, a_evaluationParameters.options.preset);
		a_parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, a_evaluationParameters.options.performanceMode - 1);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, createFlags);
		a_parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
		a_parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
	}

	bool D3D12Backend::EnsureFeature(
		FeatureState& a_state,
		ID3D12GraphicsCommandList* a_commandList,
		const D3D12EvaluationParameters& a_parameters,
		std::uint32_t a_passIndex)
	{
		if (!IsSupportedPerformanceMode(a_parameters.options.performanceMode)) {
			logger::warn("[DLSS-NR Direct] Performance mode {} is unsupported", a_parameters.options.performanceMode);
			return false;
		}

		if ((a_state.failureLatched || NeedsFeatureRecreation(a_state, a_parameters) ||
			(!a_state.feature && a_state.parameters)) && !ReleaseFeature(a_state)) {
			return false;
		}
		if (a_state.feature) {
			return true;
		}

		const auto allocateResult = allocateParameters_(&a_state.parameters);
		if (!IsNGXSuccess(allocateResult) || !a_state.parameters) {
			logger::warn("[DLSS-NR Direct] AllocateParameters failed result=0x{:08X}", static_cast<std::uint32_t>(allocateResult));
			// Retain any returned allocation for its matching DestroyParameters.
			return false;
		}

		SetCreationParameters(a_state.parameters, a_parameters);
		auto createResult = createFeature_(a_commandList, NVSDK_NGX_Feature_DLSSNR, a_state.parameters, &a_state.feature);
		auto snippetCreateResult = createFeature_ == snippetCreateFeature_ ? createResult : NVSDK_NGX_Result_Success;
		a_state.activeEvaluateFeature = evaluateFeature_;
		a_state.activeReleaseFeature = releaseFeature_;
		if (!a_state.feature && createFeature_ != snippetCreateFeature_) {
			// Match RenoDX's creation sequence: the shared NGX core gets the first
			// chance, then feature 18 is created through the signed snippet when the
			// core reports UnableToInitializeFeature. Evaluation never retries across
			// backends; it is bound to the creator selected here.
			a_state.activeEvaluateFeature = snippetEvaluateFeature_;
			a_state.activeReleaseFeature = snippetReleaseFeature_;
			snippetCreateResult = snippetCreateFeature_(
				a_commandList,
				NVSDK_NGX_Feature_DLSSNR,
				a_state.parameters,
				&a_state.feature);
			if (IsNGXSuccess(snippetCreateResult) && a_state.feature) {
				logger::info(
					"[DLSS-NR Direct] Shared NGX CreateFeature returned 0x{:08X}; feature 18 was created by nvngx_dlssnr.dll",
					static_cast<std::uint32_t>(createResult));
				createResult = snippetCreateResult;
			}
		}
		if (!IsNGXSuccess(createResult) || !a_state.feature) {
			logger::warn(
				"[DLSS-NR Direct] CreateFeature failed pass={} primaryResult=0x{:08X} snippetResult=0x{:08X} input={}x{} output={}x{} performanceMode={} preset={}",
				a_passIndex + 1,
				static_cast<std::uint32_t>(createResult),
				static_cast<std::uint32_t>(snippetCreateResult),
				a_parameters.inputWidth,
				a_parameters.inputHeight,
				a_parameters.outputWidth,
				a_parameters.outputHeight,
				a_parameters.options.performanceMode,
				a_parameters.options.preset);
			// A failed create may still return a handle or record GPU work. Keep
			// its creator's release function and parameters; the next recreation
			// drains submitted work before cleanup. Never overwrite that handle
			// with another backend's fallback result.
			return false;
		}

		a_state.inputWidth = a_parameters.inputWidth;
		a_state.inputHeight = a_parameters.inputHeight;
		a_state.outputWidth = a_parameters.outputWidth;
		a_state.outputHeight = a_parameters.outputHeight;
		a_state.performanceMode = a_parameters.options.performanceMode;
		a_state.preset = a_parameters.options.preset;
		a_state.forceReset = true;
		logger::info(
			"[DLSS-NR Direct] Created NGX feature 18 pass={} input={}x{} output={}x{} performanceMode={} preset={}",
			a_passIndex + 1,
			a_state.inputWidth,
			a_state.inputHeight,
			a_state.outputWidth,
			a_state.outputHeight,
			a_state.performanceMode,
			a_state.preset);
		return true;
	}

	void D3D12Backend::SetEvaluationParameters(
		NVSDK_NGX_Parameter* a_parameters,
		const D3D12EvaluationParameters& a_evaluationParameters,
		bool a_reset)
	{
		const auto upscaling =
			a_evaluationParameters.inputWidth != a_evaluationParameters.outputWidth ||
			a_evaluationParameters.inputHeight != a_evaluationParameters.outputHeight;
		const auto ratio = ScalingRatio(upscaling);

		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Color, a_evaluationParameters.color);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Output, a_evaluationParameters.output);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVec, a_evaluationParameters.motionVectors);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Depth, a_evaluationParameters.depth);
		a_parameters->Set(NVSDK_NGX_Parameter_Width, a_evaluationParameters.inputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_Height, a_evaluationParameters.inputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_OutWidth, a_evaluationParameters.outputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_OutHeight, a_evaluationParameters.outputHeight);

		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, 0u);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY, 0u);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, a_evaluationParameters.inputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, a_evaluationParameters.inputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, 0u);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY, 0u);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, a_evaluationParameters.guideWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, a_evaluationParameters.guideHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, a_evaluationParameters.motionVectorScaleX);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, a_evaluationParameters.motionVectorScaleY);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, 0u);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, 0u);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, a_evaluationParameters.guideWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, a_evaluationParameters.guideHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, static_cast<int>(a_evaluationParameters.depthInverted));
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, 0u);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY, 0u);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, a_evaluationParameters.outputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, a_evaluationParameters.outputHeight);

		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_InputWidth, a_evaluationParameters.inputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_InputHeight, a_evaluationParameters.inputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputWidth, a_evaluationParameters.outputWidth);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputHeight, a_evaluationParameters.outputHeight);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ScalingRatio, ratio);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Scale, ratio);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Upscaling, static_cast<int>(upscaling));
		a_parameters->Set(
			NVSDK_NGX_Parameter_DLSSNR_ComputeScalingRatioCallback,
			reinterpret_cast<void*>(&NVSDK_NGX_DLSSNR_ComputeScalingRatio));
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Reset, static_cast<int>(a_reset));
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Intensity, a_evaluationParameters.options.intensity);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, a_evaluationParameters.options.localToneStrength);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, a_evaluationParameters.options.localStructureStrength);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, a_evaluationParameters.options.skinStructureStrength);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, static_cast<int>(a_evaluationParameters.options.useAutoMask));
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Style, a_evaluationParameters.options.style);
		a_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);
		a_parameters->Set("DLSS.Indicator.Invert.X.Axis", 0);
		a_parameters->Set("DLSS.Indicator.Invert.Y.Axis", 0);
	}

	bool D3D12Backend::NeedsFeatureRecreation(
		const FeatureState& a_state,
		const D3D12EvaluationParameters& a_parameters) const
	{
		return a_state.feature &&
			(a_state.inputWidth != a_parameters.inputWidth ||
				a_state.inputHeight != a_parameters.inputHeight ||
				a_state.outputWidth != a_parameters.outputWidth ||
				a_state.outputHeight != a_parameters.outputHeight ||
				a_state.performanceMode != a_parameters.options.performanceMode ||
				a_state.preset != a_parameters.options.preset);
	}

	bool D3D12Backend::NeedsFeaturePreparation(
		const FeatureState& a_state,
		const D3D12EvaluationParameters& a_parameters) const
	{
		const auto sameFailedConfiguration =
			a_state.failedInputWidth == a_parameters.inputWidth &&
			a_state.failedInputHeight == a_parameters.inputHeight &&
			a_state.failedOutputWidth == a_parameters.outputWidth &&
			a_state.failedOutputHeight == a_parameters.outputHeight &&
			a_state.failedGuideWidth == a_parameters.guideWidth &&
			a_state.failedGuideHeight == a_parameters.guideHeight &&
			a_state.failedPerformanceMode == a_parameters.options.performanceMode &&
			a_state.failedPreset == a_parameters.options.preset;
		if (a_state.failureLatched && sameFailedConfiguration) {
			return false;
		}
		return !a_state.feature || NeedsFeatureRecreation(a_state, a_parameters) || a_state.failureLatched;
	}

	bool D3D12Backend::EnsureIntermediateResources(const D3D12EvaluationParameters& a_parameters)
	{
		const auto passCount = std::clamp(a_parameters.passCount, 1u, kMaxPassCount);
		if (passCount == 1) {
			return true;
		}
		if (!device_ || a_parameters.outputFormat == DXGI_FORMAT_UNKNOWN) {
			return false;
		}

		const auto requiredCount = passCount - 1;
		const auto configurationMatches =
			intermediateWidth_ == a_parameters.outputWidth &&
			intermediateHeight_ == a_parameters.outputHeight &&
			intermediateFormat_ == a_parameters.outputFormat;
		bool resourcesReady = configurationMatches;
		for (std::uint32_t i = 0; resourcesReady && i < requiredCount; ++i) {
			resourcesReady = intermediateResources_[i] != nullptr;
		}
		if (resourcesReady) {
			intermediateFailureLatched_ = false;
			return true;
		}

		for (auto& resource : intermediateResources_) {
			resource.Reset();
		}
		intermediateWidth_ = 0;
		intermediateHeight_ = 0;
		intermediateFormat_ = DXGI_FORMAT_UNKNOWN;

		const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		const auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			a_parameters.outputFormat,
			a_parameters.outputWidth,
			a_parameters.outputHeight,
			1,
			1,
			1,
			0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		for (std::uint32_t i = 0; i < requiredCount; ++i) {
			const auto result = device_->CreateCommittedResource(
				&heapProperties,
				D3D12_HEAP_FLAG_NONE,
				&resourceDesc,
				D3D12_RESOURCE_STATE_COMMON,
				nullptr,
				IID_PPV_ARGS(intermediateResources_[i].ReleaseAndGetAddressOf()));
			if (FAILED(result)) {
				logger::warn(
					"[DLSS-NR Direct] Could not allocate pass {} intermediate {}x{} format={} result=0x{:08X}",
					i + 1,
					a_parameters.outputWidth,
					a_parameters.outputHeight,
					static_cast<std::uint32_t>(a_parameters.outputFormat),
					static_cast<std::uint32_t>(result));
				intermediateFailureLatched_ = true;
				failedIntermediateWidth_ = a_parameters.outputWidth;
				failedIntermediateHeight_ = a_parameters.outputHeight;
				failedIntermediateFormat_ = a_parameters.outputFormat;
				failedIntermediatePassCount_ = passCount;
				return false;
			}
		}

		intermediateWidth_ = a_parameters.outputWidth;
		intermediateHeight_ = a_parameters.outputHeight;
		intermediateFormat_ = a_parameters.outputFormat;
		intermediateFailureLatched_ = false;
		return true;
	}

	bool D3D12Backend::NeedsFeaturePreparation(const D3D12EvaluationParameters& a_parameters) const
	{
		const auto passCount = std::clamp(a_parameters.passCount, 1u, kMaxPassCount);
		if (!initialized_) {
			return !initializationAttempted_;
		}

		if (passCount > 1) {
			const auto sameFailedIntermediateConfiguration =
				failedIntermediateWidth_ == a_parameters.outputWidth &&
				failedIntermediateHeight_ == a_parameters.outputHeight &&
				failedIntermediateFormat_ == a_parameters.outputFormat &&
				failedIntermediatePassCount_ == passCount;
			if (intermediateFailureLatched_ && sameFailedIntermediateConfiguration) {
				return false;
			}
			if (intermediateWidth_ != a_parameters.outputWidth ||
				intermediateHeight_ != a_parameters.outputHeight ||
				intermediateFormat_ != a_parameters.outputFormat) {
				return true;
			}
			for (std::uint32_t i = 0; i < passCount - 1; ++i) {
				if (!intermediateResources_[i]) {
					return true;
				}
			}
		}

		for (std::uint32_t i = 0; i < passCount; ++i) {
			const auto& state = features_[i];
			const auto sameFailedConfiguration =
				state.failedInputWidth == a_parameters.inputWidth &&
				state.failedInputHeight == a_parameters.inputHeight &&
				state.failedOutputWidth == a_parameters.outputWidth &&
				state.failedOutputHeight == a_parameters.outputHeight &&
				state.failedGuideWidth == a_parameters.guideWidth &&
				state.failedGuideHeight == a_parameters.guideHeight &&
				state.failedPerformanceMode == a_parameters.options.performanceMode &&
				state.failedPreset == a_parameters.options.preset;
			if (state.failureLatched && sameFailedConfiguration) {
				return false;
			}
			if (NeedsFeaturePreparation(state, a_parameters)) {
				return true;
			}
		}
		return false;
	}

	bool D3D12Backend::PrepareFeature(ID3D12GraphicsCommandList* a_commandList, const D3D12EvaluationParameters& a_parameters)
	{
		if (!a_commandList || !a_parameters.inputWidth || !a_parameters.inputHeight ||
			!a_parameters.outputWidth || !a_parameters.outputHeight) {
			return false;
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
		if (!EnsureIntermediateResources(a_parameters)) {
			return false;
		}

		const auto passCount = std::clamp(a_parameters.passCount, 1u, kMaxPassCount);
		for (std::uint32_t i = 0; i < passCount; ++i) {
			auto& state = features_[i];
			if (!EnsureFeature(state, a_commandList, a_parameters, i)) {
				state.failureLatched = true;
				state.failedInputWidth = a_parameters.inputWidth;
				state.failedInputHeight = a_parameters.inputHeight;
				state.failedOutputWidth = a_parameters.outputWidth;
				state.failedOutputHeight = a_parameters.outputHeight;
				state.failedGuideWidth = a_parameters.guideWidth;
				state.failedGuideHeight = a_parameters.guideHeight;
				state.failedPerformanceMode = a_parameters.options.performanceMode;
				state.failedPreset = a_parameters.options.preset;
				return false;
			}
		}
		return true;
	}

	bool D3D12Backend::Evaluate(ID3D12GraphicsCommandList* a_commandList, const D3D12EvaluationParameters& a_parameters)
	{
		// Evaluation never initializes, releases or recreates NGX resources.
		if (!a_commandList || !a_parameters.color || !a_parameters.output ||
			!a_parameters.motionVectors || !a_parameters.depth ||
			!a_parameters.inputWidth || !a_parameters.inputHeight ||
			!a_parameters.outputWidth || !a_parameters.outputHeight ||
			!a_parameters.guideWidth || !a_parameters.guideHeight ||
			!initialized_) {
			lastEvaluatedPassCount_ = 0;
			return false;
		}

		const auto requestedPassCount = std::clamp(a_parameters.passCount, 1u, kMaxPassCount);
		const auto scratchMatches = [&](ID3D12Resource* a_resource) {
			if (!a_resource) {
				return false;
			}
			const auto desc = a_resource->GetDesc();
			return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
				desc.Width == a_parameters.outputWidth &&
				desc.Height == a_parameters.outputHeight &&
				desc.Format == a_parameters.outputFormat &&
				desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
				desc.SampleDesc.Count == 1 &&
				(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
		};
		std::uint32_t passCount = 0;
		for (; passCount < requestedPassCount; ++passCount) {
			const auto& state = features_[passCount];
			if (!state.feature || !state.parameters || !state.activeEvaluateFeature || state.failureLatched ||
				NeedsFeatureRecreation(state, a_parameters)) {
				break;
			}
			if (passCount > 0 && !scratchMatches(intermediateResources_[passCount - 1].Get())) {
				break;
			}
		}
		if (passCount == 0) {
			lastEvaluatedPassCount_ = 0;
			return false;
		}
		if (passCount != requestedPassCount) {
			const auto fallbackKey = (requestedPassCount << 8) | passCount;
			if (loggedFallbackPassCount_ != fallbackKey) {
				logger::warn(
					"[DLSS-NR Direct] Requested {} passes but only {} prepared; using the available independent histories",
					requestedPassCount,
					passCount);
				loggedFallbackPassCount_ = fallbackKey;
			}
		} else {
			loggedFallbackPassCount_ = 0;
		}

		for (std::uint32_t pass = 0; pass < passCount; ++pass) {
			auto& state = features_[pass];
			auto passParameters = a_parameters;
			passParameters.color = pass == 0 ? a_parameters.color : intermediateResources_[pass - 1].Get();
			passParameters.output = pass + 1 == passCount ? a_parameters.output : intermediateResources_[pass].Get();
			if (pass > 0) {
				// Avoid compounding local tone remapping across independent histories.
				passParameters.options.localToneStrength = 0.0f;
			}

			const auto reset = state.forceReset || a_parameters.reset;
#ifdef UPSCALING_NR_CAPTURE
			if (NRDiagnosticCapture::Requested()) {
				NRDiagnosticCapture::Annotate(a_commandList,
					std::format("\"nr_pass_{}\":{{\"reset\":{},\"mv_scale\":[{},{}],\"local_structure\":{},\"local_tone\":{}}}",
						pass + 1, reset, passParameters.motionVectorScaleX, passParameters.motionVectorScaleY,
						passParameters.options.localStructureStrength, passParameters.options.localToneStrength));
			}
#endif
			SetEvaluationParameters(state.parameters, passParameters, reset);
			D3D12_RESOURCE_BARRIER beforeEvaluation[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(passParameters.color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(passParameters.motionVectors, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(passParameters.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(passParameters.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			};
			a_commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeEvaluation)), beforeEvaluation);
			const auto evaluateResult = state.activeEvaluateFeature(
				a_commandList,
				state.feature,
				state.parameters,
				nullptr);
			D3D12_RESOURCE_BARRIER afterEvaluation[] = {
				CD3DX12_RESOURCE_BARRIER::Transition(passParameters.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(passParameters.motionVectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(passParameters.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(passParameters.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
			};
			a_commandList->ResourceBarrier(static_cast<UINT>(std::size(afterEvaluation)), afterEvaluation);

			if (!IsNGXSuccess(evaluateResult)) {
				const auto colorDesc = passParameters.color->GetDesc();
				const auto outputDesc = passParameters.output->GetDesc();
				logger::warn(
					"[DLSS-NR Direct] EvaluateFeature failed pass={} result=0x{:08X} color={}x{} guides={}x{} output={}x{} colorResource={}x{} outputResource={}x{} performanceMode={} reset={}; using original color for SR",
					pass + 1,
					static_cast<std::uint32_t>(evaluateResult),
					passParameters.inputWidth,
					passParameters.inputHeight,
					passParameters.guideWidth,
					passParameters.guideHeight,
					passParameters.outputWidth,
					passParameters.outputHeight,
					static_cast<std::uint32_t>(colorDesc.Width),
					colorDesc.Height,
					static_cast<std::uint32_t>(outputDesc.Width),
					outputDesc.Height,
					passParameters.options.performanceMode,
					reset);
				state.failureLatched = true;
				state.failedInputWidth = passParameters.inputWidth;
				state.failedInputHeight = passParameters.inputHeight;
				state.failedOutputWidth = passParameters.outputWidth;
				state.failedOutputHeight = passParameters.outputHeight;
				state.failedGuideWidth = passParameters.guideWidth;
				state.failedGuideHeight = passParameters.guideHeight;
				state.failedPerformanceMode = passParameters.options.performanceMode;
				state.failedPreset = passParameters.options.preset;
				RequestReset();
				lastEvaluatedPassCount_ = 0;
				return false;
			}
			state.forceReset = false;
		}
		lastEvaluatedPassCount_ = passCount;
		return true;
	}

	void D3D12Backend::RequestReset()
	{
		for (auto& state : features_) {
			state.forceReset = true;
		}
	}

	bool D3D12Backend::ReleaseFeature(FeatureState& a_state)
	{
		// A failed release must not make this feature eligible for evaluation.
		a_state.inputWidth = 0;
		if (a_state.feature) {
			if (!a_state.activeReleaseFeature) {
				return false;
			}
			const auto result = a_state.activeReleaseFeature(a_state.feature);
			if (!IsNGXSuccess(result)) {
				logger::warn("[DLSS-NR Direct] ReleaseFeature failed result=0x{:08X}", static_cast<std::uint32_t>(result));
				return false;
			}
		}
		a_state.feature = nullptr;
		a_state.activeEvaluateFeature = nullptr;
		a_state.activeReleaseFeature = nullptr;

		if (a_state.parameters) {
			if (!destroyParameters_) {
				return false;
			}
			const auto result = destroyParameters_(a_state.parameters);
			if (!IsNGXSuccess(result)) {
				logger::warn("[DLSS-NR Direct] DestroyParameters failed result=0x{:08X}", static_cast<std::uint32_t>(result));
				return false;
			}
		}
		a_state = {};
		return true;
	}

	bool D3D12Backend::ReleaseFeature()
	{
		for (auto& state : features_) {
			if (!ReleaseFeature(state)) {
				return false;
			}
		}
		for (auto& resource : intermediateResources_) {
			resource.Reset();
		}
		intermediateWidth_ = 0;
		intermediateHeight_ = 0;
		intermediateFormat_ = DXGI_FORMAT_UNKNOWN;
		intermediateFailureLatched_ = false;
		failedIntermediateWidth_ = 0;
		failedIntermediateHeight_ = 0;
		failedIntermediateFormat_ = DXGI_FORMAT_UNKNOWN;
		failedIntermediatePassCount_ = 0;
		loggedFallbackPassCount_ = 0;
		lastEvaluatedPassCount_ = 0;
		return true;
	}

	void D3D12Backend::Shutdown()
	{
		if (!ReleaseFeature()) {
			return;
		}
		if (initialized_ && shutdown_) {
			const auto result = shutdown_(device_);
			if (!IsNGXSuccess(result)) {
				logger::warn("[DLSS-NR Direct] Shutdown failed result=0x{:08X}", static_cast<std::uint32_t>(result));
				return;
			}
		}
		initialized_ = false;
		{
			std::scoped_lock lock(g_nrAllocationMutex);
			if (g_nrAllocationOwner == this) {
				g_nrAllocationOwner = nullptr;
				g_nrAllocationDevice.Reset();
			}
		}

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
		shutdown_ = nullptr;
		initializationAttempted_ = false;
	}
}
