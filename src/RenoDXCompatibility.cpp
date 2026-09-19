#include "RenoDXCompatibility.h"
#include "../extern/ReShade/include/reshade.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace RenoDXCompatibility
{
	namespace
	{
		// RenoDX Fallout 4's swapchain DeviceData initialization:
		// mov byte ptr [r14+C0h], 0; mov eax, -1; mov [r14+B8h], rax;
		// mov rax, [rip+...]; test rax, rax.
		// The qword store initializes {cb index, register space}; changing EAX
		// to 11 keeps space=0 and makes the final output PS's b11 accessible.
		constexpr std::array<std::uint8_t, 30> signature{
			0x41, 0xC6, 0x86, 0xC0, 0, 0, 0, 0, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF,
			0x49, 0x89, 0x86, 0xB8, 0, 0, 0, 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xC0
		};
		constexpr std::size_t patchOffset = 9;
		enum class PatchResult { applied, missing, ambiguous, invalidImage, protectionFailed };

		PatchResult PatchImage(HMODULE module)
		{
			auto* base = reinterpret_cast<std::uint8_t*>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return PatchResult::invalidImage;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
				nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return PatchResult::invalidImage;
			const auto imageSize = nt->OptionalHeader.SizeOfImage;
			const auto* sections = IMAGE_FIRST_SECTION(nt);
			std::uint8_t* match = nullptr;
			for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
				const auto& section = sections[i];
				if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
				const auto start = section.VirtualAddress;
				const auto size = section.Misc.VirtualSize;
				if (start >= imageSize || size > imageSize - start) return PatchResult::invalidImage;
				if (size < signature.size()) continue;
				for (std::size_t offset = 0; offset <= size - signature.size(); ++offset) {
					auto* candidate = base + start + offset;
					// Only the RIP-relative displacement is wildcarded.
					if (std::memcmp(candidate, signature.data(), 23) != 0 ||
						std::memcmp(candidate + 27, signature.data() + 27, 3) != 0) continue;
					if (match) return PatchResult::ambiguous;
					match = candidate;
				}
			}
			if (!match) return PatchResult::missing;
			auto* address = match + patchOffset;
			DWORD protection{};
			if (!VirtualProtect(address, sizeof(std::uint32_t), PAGE_EXECUTE_READWRITE, &protection))
				return PatchResult::protectionFailed;
			const std::uint32_t slot = 11;
			std::memcpy(address, &slot, sizeof(slot));
			if (!FlushInstructionCache(GetCurrentProcess(), address, sizeof(slot)))
				logger::warn("[RenoDX compatibility] Instruction cache flush failed: {}", GetLastError());
			DWORD ignored{};
			if (!VirtualProtect(address, sizeof(slot), protection, &ignored))
				logger::warn("[RenoDX compatibility] Code protection restore failed: {}", GetLastError());
			logger::info("[RenoDX compatibility] Output constant-buffer slot patched to b11 at RVA 0x{:X}",
				static_cast<std::size_t>(address - base));
			return PatchResult::applied;
		}

		void TryPatch()
		{
			// Limit discovery to this add-on, not ReShade or other RenoDX games.
			HMODULE module{};
			if (!GetModuleHandleExW(0, L"renodx-fallout4.addon64", &module)) return;
			static std::once_flag inspected;
			std::call_once(inspected, [module] {
				const auto result = PatchImage(module);
				if (result != PatchResult::applied)
					logger::warn("[RenoDX compatibility] Output slot patch skipped (reason={})",
						static_cast<unsigned>(result));
			});
			FreeLibrary(module); // Release only the reference acquired above.
		}

		void InitDevice(reshade::api::device*) { TryPatch(); }
	}

	void Initialize()
	{
		// F4SEPlugin_Load runs before engine device creation. Existing add-ons
		// can be patched now. For lazy loading, our callback is registered before
		// ReShade's load_addons() appends RenoDX's callbacks, so it runs first.
		TryPatch();
		reshade::register_event<reshade::addon_event::init_device>(InitDevice);
	}
}
