// Adapted from RTX40MFG-Unlock by Michael Robles.
// Upstream: https://github.com/dashdogy/RTX40MFG-Unlock
// Licensed under the MIT License; see LICENSE in this directory.
#include "integration.h"

#include "dlssg_provider_policy.h"
#include "midpoint_fix.h"

#include <TlHelp32.h>

#include <algorithm>
#include <array>
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
			bool ngx = false;
			bool ngxPatched = false;
			bool midpointPatched = false;
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

		std::mutex g_mutex;
		std::vector<ModuleRecord> g_modules;
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
					if (prefixMatches && suffixMatches && patchBytesMatch) {
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
				if (!result.candidate) {
					logger::warn("[RTX40MFGUnlock] Streamline wrapper signature is unsupported: {}", Narrow(path.c_str()));
				}
			}
			if (record.ngx) {
				const auto result = PatchUniqueExecutablePattern(a_module, path, kNgxPatch);
				record.ngxPatched = result.patched;
				if (!result.candidate) {
					logger::warn("[RTX40MFGUnlock] NGX provider signature is unsupported: {}", Narrow(path.c_str()));
				}
				if (record.ngxPatched && midpoint_fix::AdapterVerified()) {
					record.midpointPatched = midpoint_fix::PatchProvider(a_module, path.c_str());
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
					const auto module = reinterpret_cast<HMODULE>(entry.modBaseAddr);
					auto existing = std::find_if(g_modules.begin(), g_modules.end(), [module](const ModuleRecord& a_record) {
						return a_record.module == module;
					});
					if (existing == g_modules.end()) {
						auto record = InspectModule(module);
						if (record.wrapper || record.ngx) {
							g_modules.push_back(record);
						}
					} else if (existing->ngxPatched && !existing->midpointPatched && midpoint_fix::AdapterVerified()) {
						const auto path = LoadedModulePath(module);
						existing->midpointPatched = midpoint_fix::PatchProvider(module, path.c_str());
					}
					entry.dwSize = sizeof(entry);
				} while (Module32NextW(snapshot, &entry));
			}
			CloseHandle(snapshot);

			const auto wrapperReady = std::any_of(g_modules.begin(), g_modules.end(), [](const ModuleRecord& a_record) {
				return a_record.wrapperPatched;
			});
			const auto ngxReady = std::any_of(g_modules.begin(), g_modules.end(), [](const ModuleRecord& a_record) {
				return a_record.ngxPatched;
			});
			return wrapperReady && ngxReady && midpoint_fix::Ready();
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

	bool Ready() noexcept
	{
		std::lock_guard lock(g_mutex);
		const auto wrapperReady = std::any_of(g_modules.begin(), g_modules.end(), [](const ModuleRecord& a_record) {
			return a_record.wrapperPatched;
		});
		const auto ngxReady = std::any_of(g_modules.begin(), g_modules.end(), [](const ModuleRecord& a_record) {
			return a_record.ngxPatched;
		});
		return wrapperReady && ngxReady && midpoint_fix::Ready();
	}
}
