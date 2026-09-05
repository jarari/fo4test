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
		int32_t original = 0;
		int32_t replacement = 0;
	};
	std::array<ReadPatch, 4> patches{};
	size_t patchCount = 0;
	uint32_t* displaySize = nullptr;
	bool installed = false;

	bool Install()
	{
		const bool og = REX::FModule::IsRuntimeOG();
		const auto movie = REL::ID{ 206895, 4494254 }.address();
		const auto spread = REL::ID{ 1218510, 2220267 }.address();
		patches[0] = { movie + 0x1C, true };
		patches[1] = { movie + (og ? 0x2E : 0x3E), false };
		patches[2] = { spread + (og ? 0x4F : 0x1E), false };
		patchCount = 3;
		if (!og) {
			// AE also inlines UpdateSpreadReticleRadius in UpdateDisplayObject.
			patches[patchCount++] = { REL::ID{ 1525695, 2220263 }.address() + 0x1C2, false };
		}
		// Redirect ONLY verified UI reads, leaving the shared render State intact.
		// In particular, preserve the original safe-zone, letterbox and AE
		// widescreen calculations; do not call Movie::SetViewport from DisplayMenu.
		for (size_t i = 0; i < patchCount; ++i) {
			auto& patch = patches[i];
			if (!stl::is_readable_memory(patch.instruction, 6)) {
				return false;
			}
			const auto* code = reinterpret_cast<const uint8_t*>(patch.instruction);
			std::memcpy(&patch.original, code + 2, sizeof(patch.original));
			const auto source = static_cast<uintptr_t>(static_cast<intptr_t>(patch.instruction + 6) + patch.original);
			const auto* state = Util::State_GetSingleton();
			const auto expected = reinterpret_cast<uintptr_t>(patch.height ? &state->backBufferHeight : &state->backBufferWidth);
			if (code[0] != 0x8B || code[1] != (patch.height ? 0x0D : 0x05) || source != expected) {
				logger::error("[ENB UI layout] Unexpected size read at {:X}; patch not installed", patch.instruction);
				return false;
			}
			const auto destination = reinterpret_cast<uintptr_t>(displaySize + (patch.height ? 1 : 0));
			const auto distance = static_cast<int64_t>(destination) - static_cast<int64_t>(patch.instruction + 6);
			if (distance < (std::numeric_limits<int32_t>::min)() || distance > (std::numeric_limits<int32_t>::max)()) {
				logger::error("[ENB UI layout] UI size storage is out of range; patch not installed");
				return false;
			}
			patch.replacement = static_cast<int32_t>(distance);
		}
		// Validate every site before writing any of them. No instruction moves,
		// new detour prologues or changes to the calling convention are needed.
		for (size_t i = 0; i < patchCount; ++i) {
			const auto& patch = patches[i];
			if (!REL::WriteSafeData(patch.instruction + 2, patch.replacement)) {
				for (size_t j = 0; j < i; ++j) {
					REL::WriteSafeData(patches[j].instruction + 2, patches[j].original);
					::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(patches[j].instruction), 6);
				}
				return false;
			}
			::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(patch.instruction), 6);
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
	if (!installed) {
		installed = Install();
	}
	logger::info("[ENB UI layout] Stable display size {}x{}; Movie/crosshair reads patched={}", a_width, a_height, installed);
}

void NativeUILayout::Restore()
{
	if (!installed) {
		return;
	}
	for (size_t i = 0; i < patchCount; ++i) {
		const auto& patch = patches[i];
		int32_t current = 0;
		std::memcpy(&current, reinterpret_cast<const void*>(patch.instruction + 2), sizeof(current));
		if (current == patch.replacement) {
			REL::WriteSafeData(patch.instruction + 2, patch.original);
			::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(patch.instruction), 6);
		}
	}
	installed = false;
}
