#include "NativeUILayout.h"

#include <array>
#include <cstring>
#include <limits>

#include "Util.h"

namespace
{
	struct ReadPatch
	{
		uintptr_t instruction;
		bool height;
		uint8_t modRM = 0x05;
		bool extendedRegister = false;
		bool screenDimensions = false;
		int32_t original = 0;
		int32_t replacement = 0;
		uint32_t DisplacementOffset() const { return extendedRegister ? 3u : 2u; }
		uint32_t InstructionSize() const { return DisplacementOffset() + sizeof(int32_t); }
	};
	std::array<ReadPatch, 8> patches{};
	size_t patchCount = 0;
	uint32_t* displaySize = nullptr;
	bool installed = false;
	bool edgesInstalled = false;
	size_t layoutPatchCount = 0;
	bool InstallRange(size_t begin, size_t end);

	bool Install()
	{
		const bool og = REX::FModule::IsRuntimeOG();
		const auto movie = REL::ID{ 206895, 4494254 }.address();
		const auto spread = REL::ID{ 1218510, 2220267 }.address();
		patches[0] = { movie + 0x1C, true, 0x0D };
		patches[1] = { movie + (og ? 0x2E : 0x3E), false };
		patches[2] = { spread + (og ? 0x4F : 0x1E), false };
		patchCount = 3;
		if (!og) {
			// AE also inlines UpdateSpreadReticleRadius in UpdateDisplayObject.
			patches[patchCount++] = { REL::ID{ 1525695, 2220263 }.address() + 0x1C2, false };
		}
		layoutPatchCount = patchCount;
		// CursorMoveEvent coordinates are display pixels. Only redirect these
		// edge-rotation reads; the scene allocation dimensions must stay low-res.
		// Verified in OG 1.10.163 and AE 1.11.191 unpacked executables.
		const auto dialogue = REL::ID{ 1222039, 2248681 }.address();
		const auto multiActivate = REL::ID{ 1143827, 2223905 }.address();
		patches[patchCount++] = { dialogue + 0x65, false, 0x05, true, true };  // mov r8d
		patches[patchCount++] = { dialogue + (og ? 0xE4 : 0xE3), true, 0x0D, true, true };  // mov r9d
		patches[patchCount++] = { multiActivate + 0x19, false, 0x15, false, true };  // mov edx
		patches[patchCount++] = { multiActivate + (og ? 0x99 : 0x98), true, 0x0D, true, true };  // mov r9d
		// A rejected edge patch must not disable the existing native movie layout.
		const bool layoutInstalled = InstallRange(0, layoutPatchCount);
		edgesInstalled = InstallRange(layoutPatchCount, patchCount);
		return layoutInstalled;
	}

	bool InstallRange(size_t begin, size_t end)
	{
		// Redirect ONLY verified UI reads, leaving the shared render State intact.
		// In particular, preserve the original safe-zone, letterbox and AE
		// widescreen calculations; do not call Movie::SetViewport from DisplayMenu.
		for (size_t i = begin; i < end; ++i) {
			auto& patch = patches[i];
			if (!stl::is_readable_memory(patch.instruction, patch.InstructionSize())) {
				return false;
			}
			const auto* code = reinterpret_cast<const uint8_t*>(patch.instruction);
			const auto opcodeOffset = patch.extendedRegister ? 1u : 0u;
			std::memcpy(&patch.original, code + patch.DisplacementOffset(), sizeof(patch.original));
			const auto source = static_cast<uintptr_t>(static_cast<intptr_t>(patch.instruction + patch.InstructionSize()) + patch.original);
			const auto* state = Util::State_GetSingleton();
			const auto expected = reinterpret_cast<uintptr_t>(patch.screenDimensions ?
				(patch.height ? &state->screenHeight : &state->screenWidth) :
				(patch.height ? &state->backBufferHeight : &state->backBufferWidth));
			if ((patch.extendedRegister && code[0] != 0x44) ||
				code[opcodeOffset] != 0x8B || code[opcodeOffset + 1] != patch.modRM || source != expected) {
				logger::error("[ENB UI layout] Unexpected size read at {:X}; patch not installed", patch.instruction);
				return false;
			}
			const auto destination = reinterpret_cast<uintptr_t>(displaySize + (patch.height ? 1 : 0));
			const auto distance = static_cast<int64_t>(destination) - static_cast<int64_t>(patch.instruction + patch.InstructionSize());
			if (distance < (std::numeric_limits<int32_t>::min)() || distance > (std::numeric_limits<int32_t>::max)()) {
				logger::error("[ENB UI layout] UI size storage is out of range; patch not installed");
				return false;
			}
			patch.replacement = static_cast<int32_t>(distance);
		}
		// Validate every site before writing any of them. No instruction moves,
		// new detour prologues or changes to the calling convention are needed.
		for (size_t i = begin; i < end; ++i) {
			const auto& patch = patches[i];
			if (!REL::WriteSafeData(patch.instruction + patch.DisplacementOffset(), patch.replacement)) {
				for (size_t j = begin; j < i; ++j) {
					REL::WriteSafeData(patches[j].instruction + patches[j].DisplacementOffset(), patches[j].original);
					::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(patches[j].instruction), patches[j].InstructionSize());
				}
				return false;
			}
			::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(patch.instruction), patch.InstructionSize());
		}
		return true;
	}
}

void NativeUILayout::SetDisplaySize(uint32_t a_width, uint32_t a_height)
{
	if (!a_width || !a_height) {
		return;
	}
	if (!displaySize) {
		// Keep the data in the executable-near trampoline, not the far-away DLL.
		const auto storage = reinterpret_cast<uintptr_t>(REL::GetTrampoline().allocate(15));
		displaySize = reinterpret_cast<uint32_t*>((storage + 7) & ~uintptr_t{ 7 });
	}
	::InterlockedExchange(reinterpret_cast<volatile LONG*>(displaySize), static_cast<LONG>(a_width));
	::InterlockedExchange(reinterpret_cast<volatile LONG*>(displaySize + 1), static_cast<LONG>(a_height));
	if (!installed && !edgesInstalled) {
		installed = Install();
	}
	logger::info("[ENB UI layout] Stable display size {}x{}; Movie/crosshair reads patched={}; menu edge reads patched={}", a_width, a_height, installed, edgesInstalled);
}

void NativeUILayout::Restore()
{
	if (!installed && !edgesInstalled) {
		return;
	}
	for (size_t i = 0; i < patchCount; ++i) {
		if (i < layoutPatchCount ? !installed : !edgesInstalled) {
			continue;
		}
		const auto& patch = patches[i];
		int32_t current = 0;
		std::memcpy(&current, reinterpret_cast<const void*>(patch.instruction + patch.DisplacementOffset()), sizeof(current));
		if (current == patch.replacement) {
			REL::WriteSafeData(patch.instruction + patch.DisplacementOffset(), patch.original);
			::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(patch.instruction), patch.InstructionSize());
		}
	}
	installed = false;
	edgesInstalled = false;
}
