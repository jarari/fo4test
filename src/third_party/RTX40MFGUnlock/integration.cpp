// Adapted from RTX40MFG-Unlock by Michael Robles.
// Upstream: https://github.com/dashdogy/RTX40MFG-Unlock
// Licensed under the MIT License; see LICENSE in this directory.
#include "integration.h"

#include "dlssg_provider_policy.h"
#include "midpoint_fix.h"
#include "universal_wrapper_profile.h"

#include <TlHelp32.h>
#include <nvsdk_ngx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace RTX40MFGUnlock
{
	namespace
	{
		static_assert(universal_wrapper_profile::SafeMaximumMultiplier(1) == 2);
		static_assert(universal_wrapper_profile::SafeMaximumMultiplier(3) == 4);
		static_assert(universal_wrapper_profile::SafeMaximumMultiplier(5) == 6);
		static_assert(universal_wrapper_profile::SafeMaximumMultiplier(7) == 2);
		static_assert(universal_wrapper_profile::ClampMultiplier(6, 3) == 4);
		static_assert(universal_wrapper_profile::ClampMultiplier(0, 5) == 2);

		struct PatternPatch
		{
			const char* label;
			const std::uint8_t* pattern;
			std::size_t patternSize;
			std::size_t patchOffset;
			const std::uint8_t* original;
			const std::uint8_t* replacement;
			std::size_t patchSize;
		};

		struct PatternPatchResult
		{
			bool candidate = false;
			bool patched = false;
			std::uint8_t* match = nullptr;
		};

		struct ModuleRecord
		{
			HMODULE module = nullptr;
			bool wrapper = false;
			bool wrapperPatched = false;
			std::uint32_t compiledMaximum = 0;
			bool ngx = false;
			bool ngxPatched = false;
			bool createHookInstalled = false;
			std::int32_t createHookSlot = -1;
			bool midpointPatched = false;
		};

		// NGX can expose the same provider ABI from the game-local DLL and from
		// an NVIDIA NGX model image with an opaque .bin name.  The old integration
		// patched the first image found by module enumeration.  Keep a small set of
		// per-image entry detours instead, so the provider which actually receives
		// the DLSS-G CreateFeature call can be selected before the call continues.
		using NgxCreateFeature_t = NVSDK_NGX_Result(NVSDK_CONV*)(
			ID3D12GraphicsCommandList*,
			NVSDK_NGX_Feature,
			NVSDK_NGX_Parameter*,
			NVSDK_NGX_Handle**);
		constexpr std::size_t kMaxNgxCreateHooks = 16;

		struct NgxCreateHookSlot
		{
			HMODULE module = nullptr;
			NgxCreateFeature_t original = nullptr;
			std::uintptr_t target = 0;
			std::atomic<bool> installing = false;
			std::atomic<bool> frameGenerationObserved = false;
		};

		constexpr std::array<std::uint8_t, 10> kWrapperPattern{
			0xBA, 0x05, 0x00, 0x00, 0x00, 0x3B, 0xCA, 0x0F, 0x42, 0xD1
		};
		constexpr std::array<std::uint8_t, 3> kWrapperOriginal{ 0x0F, 0x42, 0xD1 };
		constexpr std::array<std::uint8_t, 3> kWrapperReplacement{ 0x90, 0x90, 0x90 };
		const PatternPatch kWrapperPatch{
			"Streamline maximum",
			kWrapperPattern.data(),
			kWrapperPattern.size(),
			7,
			kWrapperOriginal.data(),
			kWrapperReplacement.data(),
			kWrapperOriginal.size()
		};

		constexpr std::array<std::uint8_t, 13> kNgxPattern{
			0x84, 0xD2, 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00, 0xBE, 0x05, 0x00, 0x00, 0x00
		};
		constexpr std::array<std::uint8_t, 6> kNgxOriginal{ 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00 };
		constexpr std::array<std::uint8_t, 6> kNgxReplacement{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		const PatternPatch kNgxPatch{
			"NGX device support",
			kNgxPattern.data(),
			kNgxPattern.size(),
			2,
			kNgxOriginal.data(),
			kNgxReplacement.data(),
			kNgxOriginal.size()
		};

		std::recursive_mutex g_mutex;
		HMODULE g_activeWrapper = nullptr;
		HMODULE g_activeNgxProvider = nullptr;
		std::vector<ModuleRecord> g_modules;
		std::array<NgxCreateHookSlot, kMaxNgxCreateHooks> g_ngxCreateHooks{};
		bool g_midpointLogConnected = false;

		std::string Narrow(const wchar_t* a_text)
		{
			if (!a_text || !*a_text) {
				return {};
			}

			const auto length = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
			if (length <= 1) {
				return {};
			}
			std::string result(static_cast<std::size_t>(length), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_text, -1, result.data(), length, nullptr, nullptr);
			result.pop_back();
			return result;
		}

		void MidpointLog(const wchar_t* a_message)
		{
			logger::info("[RTX40MFGUnlock] {}", Narrow(a_message));
		}

		const IMAGE_NT_HEADERS64* ImageHeaders(HMODULE a_module)
		{
			const auto* base = reinterpret_cast<const std::uint8_t*>(a_module);
			if (!base) {
				return nullptr;
			}

			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
				static_cast<std::size_t>(dos->e_lfanew) > 1024 * 1024) {
				return nullptr;
			}

			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
				return nullptr;
			}
			return nt;
		}

		bool RvaRangeIsValid(const IMAGE_NT_HEADERS64* a_nt, DWORD a_rva, std::size_t a_size)
		{
			return a_nt && a_rva < a_nt->OptionalHeader.SizeOfImage &&
				a_size <= static_cast<std::size_t>(a_nt->OptionalHeader.SizeOfImage - a_rva);
		}

		bool ModuleExportsFunction(HMODULE a_module, const char* a_expected)
		{
			const auto* nt = ImageHeaders(a_module);
			if (!nt || !a_expected) {
				return false;
			}

			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
			if (!directory.VirtualAddress ||
				!RvaRangeIsValid(nt, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) {
				return false;
			}

			const auto* base = reinterpret_cast<const std::uint8_t*>(a_module);
			const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
			const auto namesSize = static_cast<std::size_t>(exports->NumberOfNames) * sizeof(DWORD);
			if (!exports->AddressOfNames || !RvaRangeIsValid(nt, exports->AddressOfNames, namesSize)) {
				return false;
			}

			const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
			for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
				const auto nameRva = names[index];
				if (!RvaRangeIsValid(nt, nameRva, 1)) {
					continue;
				}
				const auto* name = reinterpret_cast<const char*>(base + nameRva);
				const auto remaining = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage - nameRva);
				const auto length = strnlen_s(name, remaining);
				if (length < remaining && std::strcmp(name, a_expected) == 0) {
					return true;
				}
			}
			return false;
		}

		std::wstring LoadedModulePath(HMODULE a_module)
		{
			wchar_t path[32768]{};
			const auto length = GetModuleFileNameW(a_module, path, static_cast<DWORD>(std::size(path)));
			return length > 0 && length < std::size(path) ? std::wstring(path, length) : std::wstring{};
		}

		PatternPatchResult PatchUniqueExecutablePattern(
			HMODULE a_module,
			const std::wstring& a_path,
			const PatternPatch& a_patch)
		{
			const auto* base = reinterpret_cast<const std::uint8_t*>(a_module);
			const auto* nt = ImageHeaders(a_module);
			if (!nt) {
				return {};
			}

			const auto* section = IMAGE_FIRST_SECTION(nt);
			std::uint8_t* match = nullptr;
			std::size_t matchCount = 0;
			for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
				if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
					section->VirtualAddress >= nt->OptionalHeader.SizeOfImage) {
					continue;
				}

				auto* begin = const_cast<std::uint8_t*>(base + section->VirtualAddress);
				const auto available = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage - section->VirtualAddress);
				const auto size = std::min<std::size_t>(
					available,
					std::max<std::size_t>(section->Misc.VirtualSize, section->SizeOfRawData));
				if (size < a_patch.patternSize) {
					continue;
				}

				const auto suffixOffset = a_patch.patchOffset + a_patch.patchSize;
				for (std::size_t offset = 0; offset + a_patch.patternSize <= size; ++offset) {
					const bool prefixMatches = a_patch.patchOffset == 0 ||
						std::memcmp(begin + offset, a_patch.pattern, a_patch.patchOffset) == 0;
					const bool suffixMatches = suffixOffset == a_patch.patternSize ||
						std::memcmp(
							begin + offset + suffixOffset,
							a_patch.pattern + suffixOffset,
							a_patch.patternSize - suffixOffset) == 0;
					const auto* candidate = begin + offset + a_patch.patchOffset;
					const bool patchBytesMatch =
						std::memcmp(candidate, a_patch.original, a_patch.patchSize) == 0 ||
						std::memcmp(candidate, a_patch.replacement, a_patch.patchSize) == 0;
					if ((&a_patch == &kWrapperPatch)
						? universal_wrapper_profile::Matches(begin + offset, size - offset)
						: (prefixMatches && suffixMatches && patchBytesMatch)) {
						match = begin + offset;
						++matchCount;
					}
				}
			}

			if (matchCount == 0) {
				return {};
			}
			if (matchCount != 1 || !match) {
				logger::warn(
					"[RTX40MFGUnlock] {} expected one pattern, found {}: {}",
					a_patch.label,
					matchCount,
					Narrow(a_path.c_str()));
				return { true, false, nullptr };
			}

			auto* address = match + a_patch.patchOffset;
			if (std::memcmp(address, a_patch.replacement, a_patch.patchSize) == 0) {
				return { true, true, match };
			}
			if (std::memcmp(address, a_patch.original, a_patch.patchSize) != 0) {
				return { true, false, match };
			}

			DWORD oldProtection = 0;
			if (!VirtualProtect(address, a_patch.patchSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
				logger::warn(
					"[RTX40MFGUnlock] {} VirtualProtect failed error={}: {}",
					a_patch.label,
					GetLastError(),
					Narrow(a_path.c_str()));
				return { true, false, match };
			}

			std::memcpy(address, a_patch.replacement, a_patch.patchSize);
			FlushInstructionCache(GetCurrentProcess(), address, a_patch.patchSize);
			DWORD ignoredProtection = 0;
			if (!VirtualProtect(address, a_patch.patchSize, oldProtection, &ignoredProtection)) {
				logger::warn(
					"[RTX40MFGUnlock] {} protection restore failed error={}: {}",
					a_patch.label,
					GetLastError(),
					Narrow(a_path.c_str()));
				return { true, false, match };
			}

			logger::info(
				"[RTX40MFGUnlock] {} patched RVA=0x{:X}: {}",
				a_patch.label,
				static_cast<std::size_t>(address - const_cast<std::uint8_t*>(base)),
				Narrow(a_path.c_str()));
			return { true, true, match };
		}

		void ObserveActiveNgxProvider(std::size_t a_slotIndex) noexcept
		{
			if (a_slotIndex >= g_ngxCreateHooks.size()) {
				return;
			}

			auto& slot = g_ngxCreateHooks[a_slotIndex];
			while (slot.installing.load(std::memory_order_acquire)) {
				YieldProcessor();
			}
			bool expected = false;
			if (!slot.frameGenerationObserved.compare_exchange_strong(
					expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
				return;
			}

			const auto module = slot.module;
			if (!module || !midpoint_fix::AdapterVerified()) {
				return;
			}

			std::lock_guard lock(g_mutex);
			const auto record = std::find_if(g_modules.begin(), g_modules.end(),
				[module](const ModuleRecord& a_record) { return a_record.module == module; });
			if (record == g_modules.end() || !record->ngx || !record->ngxPatched) {
				logger::warn("[RTX40MFGUnlock] Active NGX provider was not a patched candidate: {}",
					Narrow(LoadedModulePath(module).c_str()));
				return;
			}

			if (g_activeNgxProvider && g_activeNgxProvider != module) {
				// A provider change after the first FG pipeline cannot be switched
				// safely because the midpoint publication is process-global.  Keep
				// the first confirmed route and fail closed until the game recreates.
				const auto path = LoadedModulePath(module);
				// Let midpoint_fix record the same transition as a restart-required
				// failure.  It will reject the second image without touching it.
				(void)midpoint_fix::PatchProvider(module, path.c_str());
				logger::warn(
					"[RTX40MFGUnlock] Active NGX provider changed after selection: active={} observed={}; restart required",
					Narrow(LoadedModulePath(g_activeNgxProvider).c_str()),
					Narrow(path.c_str()));
				return;
			}

			const auto path = LoadedModulePath(module);
			if (!midpoint_fix::PatchProvider(module, path.c_str())) {
				logger::warn("[RTX40MFGUnlock] Active NGX provider midpoint publication failed: {}",
					Narrow(path.c_str()));
				return;
			}

			g_activeNgxProvider = module;
			record->midpointPatched = true;
			logger::info("[RTX40MFGUnlock] Active NGX provider selected from DLSS-G CreateFeature: {}",
				Narrow(path.c_str()));
		}

		template <std::size_t Index>
		NVSDK_NGX_Result NVSDK_CONV NgxCreateFeatureHook(
			ID3D12GraphicsCommandList* a_commandList,
			NVSDK_NGX_Feature a_feature,
			NVSDK_NGX_Parameter* a_parameters,
			NVSDK_NGX_Handle** a_handle) noexcept
		{
			static_assert(Index < kMaxNgxCreateHooks);
			auto& slot = g_ngxCreateHooks[Index];
			const auto original = slot.original;
			if (!original) {
				return NVSDK_NGX_Result_FAIL_NotInitialized;
			}
			if (a_feature == NVSDK_NGX_Feature_FrameGeneration) {
				ObserveActiveNgxProvider(Index);
			}
			return original(a_commandList, a_feature, a_parameters, a_handle);
		}

		using NgxCreateHook_t = NgxCreateFeature_t;
		constexpr std::array<NgxCreateHook_t, kMaxNgxCreateHooks> kNgxCreateHookFunctions{
			&NgxCreateFeatureHook<0>,
			&NgxCreateFeatureHook<1>,
			&NgxCreateFeatureHook<2>,
			&NgxCreateFeatureHook<3>,
			&NgxCreateFeatureHook<4>,
			&NgxCreateFeatureHook<5>,
			&NgxCreateFeatureHook<6>,
			&NgxCreateFeatureHook<7>,
			&NgxCreateFeatureHook<8>,
			&NgxCreateFeatureHook<9>,
			&NgxCreateFeatureHook<10>,
			&NgxCreateFeatureHook<11>,
			&NgxCreateFeatureHook<12>,
			&NgxCreateFeatureHook<13>,
			&NgxCreateFeatureHook<14>,
			&NgxCreateFeatureHook<15>
		};

		bool InstallNgxCreateFeatureHook(ModuleRecord& a_record, const std::wstring& a_path)
		{
			if (!a_record.ngx || a_record.createHookInstalled) {
				return a_record.createHookInstalled;
			}

			const auto target = reinterpret_cast<std::uintptr_t>(
				GetProcAddress(a_record.module, "NVSDK_NGX_D3D12_CreateFeature"));
			if (!target) {
				logger::warn("[RTX40MFGUnlock] NGX CreateFeature export missing: {}",
					Narrow(a_path.c_str()));
				return false;
			}

			HMODULE owner = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
				reinterpret_cast<LPCWSTR>(target), &owner)) {
				return false;
			}
			const bool ownedByModule = owner == a_record.module;
			FreeLibrary(owner);
			if (!ownedByModule) {
				logger::warn("[RTX40MFGUnlock] NGX CreateFeature export is forwarded: {}",
					Narrow(a_path.c_str()));
				return false;
			}

			std::size_t slotIndex = 0;
			for (; slotIndex < g_ngxCreateHooks.size(); ++slotIndex) {
				if (g_ngxCreateHooks[slotIndex].module == a_record.module) {
					a_record.createHookInstalled = true;
					a_record.createHookSlot = static_cast<std::int32_t>(slotIndex);
					return true;
				}
				if (!g_ngxCreateHooks[slotIndex].module) {
					break;
				}
			}
			if (slotIndex == g_ngxCreateHooks.size()) {
				logger::warn("[RTX40MFGUnlock] Too many NGX providers for CreateFeature routing: {}",
					Narrow(a_path.c_str()));
				return false;
			}

			auto& slot = g_ngxCreateHooks[slotIndex];
			slot.module = a_record.module;
			slot.target = target;
			slot.installing.store(true, std::memory_order_release);
			const auto trampoline = Detours::X64::DetourFunction(
				target,
				reinterpret_cast<std::uintptr_t>(kNgxCreateHookFunctions[slotIndex]),
				Detours::X64Option::USE_RAX_JUMP);
			if (!trampoline) {
				slot.module = nullptr;
				slot.original = nullptr;
				slot.target = 0;
				slot.installing.store(false, std::memory_order_release);
				slot.frameGenerationObserved.store(false, std::memory_order_release);
				logger::warn("[RTX40MFGUnlock] NGX CreateFeature detour failed: {}",
					Narrow(a_path.c_str()));
				return false;
			}
			slot.original = reinterpret_cast<NgxCreateFeature_t>(trampoline);
			slot.installing.store(false, std::memory_order_release);
			a_record.createHookInstalled = true;
			a_record.createHookSlot = static_cast<std::int32_t>(slotIndex);
			logger::info("[RTX40MFGUnlock] NGX CreateFeature route installed slot={} path={}",
				slotIndex, Narrow(a_path.c_str()));
			return true;
		}

		ModuleRecord InspectModule(HMODULE a_module)
		{
			ModuleRecord record{};
			record.module = a_module;
			const auto path = LoadedModulePath(a_module);
			record.wrapper = ModuleExportsFunction(a_module, "slGetPluginFunction");
			record.ngx =
				dlssg_provider_policy::IsDlssgImplementationModule(a_module) &&
				ModuleExportsFunction(a_module, "NVSDK_NGX_D3D12_CreateFeature") &&
				ModuleExportsFunction(a_module, "NVSDK_NGX_GetGPUArchitecture");

			if (record.wrapper) {
				const auto result = PatchUniqueExecutablePattern(a_module, path, kWrapperPatch);
				record.wrapperPatched = result.patched;
				if (result.patched) {
					std::memcpy(&record.compiledMaximum, result.match + universal_wrapper_profile::kMaximumOffset, sizeof(record.compiledMaximum));
				}
				// Generic Streamline plugins share this export. Only the active
				// DLSS-G route is required to have a supported wrapper signature.
			}
			if (record.ngx) {
				const auto result = PatchUniqueExecutablePattern(a_module, path, kNgxPatch);
				record.ngxPatched = result.patched;
				if (!result.candidate) {
					logger::warn("[RTX40MFGUnlock] NGX provider signature is unsupported: {}", Narrow(path.c_str()));
				}
				if (record.ngxPatched && midpoint_fix::AdapterVerified()) {
					// Do not publish the process-global midpoint descriptor while
					// this is only a passive module candidate.  The provider may be
					// an inactive game-local DLL while NGX selects the ProgramData
					// image, or vice versa.  The entry detour below resolves that
					// choice at the first DLSS-G CreateFeature call.
					record.createHookInstalled = InstallNgxCreateFeatureHook(record, path);
				}
			}
			return record;
		}

		bool PatchLoadedModulesLocked()
		{
			if (!g_midpointLogConnected) {
				midpoint_fix::SetLogCallback(&MidpointLog);
				g_midpointLogConnected = true;
			}
			if (!midpoint_fix::AdapterVerified()) {
				return false;
			}

			const auto snapshot = CreateToolhelp32Snapshot(
				TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
				GetCurrentProcessId());
			if (snapshot == INVALID_HANDLE_VALUE) {
				logger::warn("[RTX40MFGUnlock] Could not enumerate loaded modules error={}", GetLastError());
				return false;
			}

			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			if (Module32FirstW(snapshot, &entry)) {
				do {
					InspectLoadedModule(reinterpret_cast<HMODULE>(entry.modBaseAddr));
					entry.dwSize = sizeof(entry);
				} while (Module32NextW(snapshot, &entry));
			}
			CloseHandle(snapshot);

			return Ready();
		}
	}

	bool PatchLoadedModules() noexcept
	{
		try {
			std::lock_guard lock(g_mutex);
			return PatchLoadedModulesLocked();
		} catch (const std::exception& e) {
			logger::warn("[RTX40MFGUnlock] Module patching failed: {}", e.what());
		} catch (...) {
			logger::warn("[RTX40MFGUnlock] Module patching failed with an unknown exception");
		}
		return false;
	}

	bool ObserveD3D12Device(ID3D12Device* a_device) noexcept
	{
		if (!a_device || !midpoint_fix::ObserveD3D12Device(a_device)) {
			return false;
		}
		return PatchLoadedModules();
	}

	bool AdaAdapterVerified() noexcept
	{
		return midpoint_fix::AdapterVerified();
	}

	void InspectLoadedModule(HMODULE a_module) noexcept
	{
		// Retain inspected code: cached patch addresses and upstream PTX descriptors
		// must not outlive their DLL. Only relevant modules keep this reference.
		if (!a_module || (reinterpret_cast<std::uintptr_t>(a_module) & 3)) {
			return;
		}
		HMODULE retained = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCWSTR>(a_module), &retained)) {
			return;
		}
		try {
			InstallLoaderDiscovery(retained);
			std::lock_guard lock(g_mutex);
			if (midpoint_fix::AdapterVerified()) {
				auto existing = std::find_if(g_modules.begin(), g_modules.end(),
					[retained](const ModuleRecord& record) { return record.module == retained; });
				if (existing == g_modules.end()) {
					auto record = InspectModule(retained);
					if (record.wrapper || record.ngx) {
						g_modules.push_back(record);
						retained = nullptr; // Deliberately held for plugin lifetime.
					}
				} else if (existing->ngxPatched && !existing->createHookInstalled) {
					const auto path = LoadedModulePath(retained);
					existing->createHookInstalled = InstallNgxCreateFeatureHook(*existing, path);
				}
			}
		} catch (...) {
			logger::warn("[RTX40MFGUnlock] Loaded module inspection failed");
		}
		if (retained) {
			FreeLibrary(retained);
		}
	}

	void ObserveWrapper(const void* a_function) noexcept
	{
		HMODULE module = nullptr;
		if (a_function && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCWSTR>(a_function), &module)) {
			InspectLoadedModule(module);
			{
				std::lock_guard lock(g_mutex);
				g_activeWrapper = module;
			}
			logger::info("[RTX40MFGUnlock] Active DLSS-G wrapper: {}", Narrow(LoadedModulePath(module).c_str()));
			FreeLibrary(module);
		}
	}

	bool Ready() noexcept
	{
		std::lock_guard lock(g_mutex);
		const auto wrapper = std::find_if(g_modules.begin(), g_modules.end(),
			[](const ModuleRecord& record) { return record.module == g_activeWrapper; });
		const auto provider = std::find_if(g_modules.begin(), g_modules.end(),
			[](const ModuleRecord& record) { return record.module == g_activeNgxProvider; });
		return midpoint_fix::AdapterVerified() && midpoint_fix::Ready() &&
			wrapper != g_modules.end() &&
			wrapper->wrapperPatched && provider != g_modules.end() &&
			provider->ngx && provider->ngxPatched && provider->createHookInstalled &&
			provider->midpointPatched;
	}

	std::uint32_t MaximumGeneratedFrames() noexcept
	{
		std::lock_guard lock(g_mutex);
		if (!Ready()) {
			return 1;
		}
		const auto wrapper = std::find_if(g_modules.begin(), g_modules.end(),
			[](const ModuleRecord& record) { return record.module == g_activeWrapper; });
		return universal_wrapper_profile::SafeMaximumMultiplier(wrapper->compiledMaximum) - 1;
	}
}
