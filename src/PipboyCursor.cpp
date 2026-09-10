#include "PipboyCursor.h"
#include "DX12SwapChain.h"
#include "ENBRenderDomain.h"
#include "Util.h"
#include <Scaleform/G/GFx_Viewport.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	bool constraintsInstalled = false;
	bool viewportInstalled = false;

	void SetDisplayBounds(RE::MenuCursor* cursor, uint32_t width, uint32_t height)
	{
		if (!cursor || !width || !height || width > INT32_MAX || height > INT32_MAX) return;
		// CursorMoveEvent and Win32 cursor positions use client/display pixels.
		// Preserve the position rather than recentering on each quality change.
		cursor->minCursorX = cursor->minCursorY = 0;
		cursor->maxCursorX = static_cast<int32_t>(width);
		cursor->maxCursorY = static_cast<int32_t>(height);
		cursor->cursorPosX = std::clamp(cursor->cursorPosX, 0, cursor->maxCursorX);
		cursor->cursorPosY = std::clamp(cursor->cursorPosY, 0, cursor->maxCursorY);
	}

	struct CursorConstraints
	{
		static void thunk(RE::MenuCursor* cursor, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
		{
			if (!ENBRenderDomain::Get().Active()) {
				func(cursor, left, top, width, height);
				return;
			}
			const auto& display = DX12SwapChain::GetSingleton()->swapChainDesc;
			SetDisplayBounds(cursor, display.Width, display.Height);
			if (cursor->maxCursorX <= cursor->minCursorX || cursor->maxCursorY <= cursor->minCursorY) {
				cursor->ClearConstraints();
				return; // Do not divide by zero in the engine implementation.
			}
			// These settings are cursor pixels, not Pipboy RT pixels. Intersect the
			// requested rectangle in 64 bits: engine unsigned subtraction wraps if
			// left+width exceeds max, producing enormous constraint percentages.
			const auto fit = [](uint32_t origin, uint32_t size, int32_t minimum, int32_t maximum) {
				const int64_t lo = std::max<int64_t>(minimum, 0);
				const int64_t hi = maximum;
				if (hi <= lo) return std::pair<uint32_t, uint32_t>{0, 0};
				const int64_t end = uint64_t{origin} + size;
				if (!size || origin >= hi || end <= lo)
					return std::pair{static_cast<uint32_t>(lo), static_cast<uint32_t>(hi - lo)};
				const auto first = std::clamp<int64_t>(origin, lo, hi - 1);
				const auto last = std::clamp<int64_t>(end, first + 1, hi);
				return std::pair{static_cast<uint32_t>(first), static_cast<uint32_t>(last - first)};
			};
			const auto x = fit(left, width, cursor->minCursorX, cursor->maxCursorX);
			const auto y = fit(top, height, cursor->minCursorY, cursor->maxCursorY);
			func(cursor, x.first, y.first, x.second, y.second);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct AECursorRenderer
	{
		static void thunk(RE::CursorMenu* menu, RE::IUIMessageData* data)
		{
			func(menu, data);
			if (data) CorrectViewport(menu);
		}
		static void CorrectViewport(RE::CursorMenu* menu)
		{
			if (!menu || !ENBRenderDomain::Get().Active() || !menu->uiMovie ||
				menu->customRendererName != "PipboyMenu") return;
			auto* renderer = RE::Interface3D::Renderer::GetByName(menu->customRendererName);
			if (!renderer) return;
			using InitialTarget = uint32_t (*)(RE::Interface3D::Renderer*);
			static REL::Relocation<InitialTarget> initialTarget{REL::ID{1111397, 2222575}};
			const auto target = initialTarget(renderer);
			if (target != 60 && target != 61) return;
			auto* texture = reinterpret_cast<ID3D11Texture2D*>(RE::BSGraphics::GetRendererData()->renderTargets[target].texture);
			if (!texture) return;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			Scaleform::GFx::Viewport viewport{};
			menu->uiMovie->GetViewport(&viewport);
			if (viewport.bufferWidth <= 0 || viewport.bufferHeight <= 0 || !desc.Width || !desc.Height ||
				(desc.Width == static_cast<uint32_t>(viewport.bufferWidth) && desc.Height == static_cast<uint32_t>(viewport.bufferHeight))) return;
			// AE builds a 1920x1080 rectangle over INI-sized buffers. Preserve that
			// normalized rectangle while promoting its physical allocation. Do not
			// change the INI dimensions used for logical screen-to-UV conversion.
			const double sx = static_cast<double>(desc.Width) / viewport.bufferWidth;
			const double sy = static_cast<double>(desc.Height) / viewport.bufferHeight;
			const auto scaled = [](int32_t value, double scale) {
				return static_cast<int32_t>(std::clamp(std::round(value * scale),
					static_cast<double>(INT32_MIN), static_cast<double>(INT32_MAX)));
			};
			viewport.left = scaled(viewport.left, sx); viewport.top = scaled(viewport.top, sy);
			viewport.width = scaled(viewport.width, sx); viewport.height = scaled(viewport.height, sy);
			viewport.scissorLeft = scaled(viewport.scissorLeft, sx); viewport.scissorTop = scaled(viewport.scissorTop, sy);
			viewport.scissorWidth = scaled(viewport.scissorWidth, sx); viewport.scissorHeight = scaled(viewport.scissorHeight, sy);
			viewport.bufferWidth = desc.Width; viewport.bufferHeight = desc.Height;
			menu->uiMovie->SetViewport(viewport);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void PipboyCursor::UpdateDisplayBounds(uint32_t width, uint32_t height)
{
	if (constraintsInstalled && ENBRenderDomain::Get().Active())
		SetDisplayBounds(RE::MenuCursor::GetSingleton(), width, height);
}

void PipboyCursor::RefreshViewport()
{
	// Allocation may occur after UpdateCustomRenderer, or be replaced while the
	// menu stays open. Rebase only when the actual buffer extent changes.
	if (!viewportInstalled || !ENBRenderDomain::Get().Active()) return;
	if (auto* ui = RE::UI::GetSingleton()) {
		const auto menu = ui->GetMenu<RE::CursorMenu>();
		if (menu) AECursorRenderer::CorrectViewport(menu.get());
	}
}

void PipboyCursor::InstallHooks(bool nativeDomains)
{
	if (!nativeDomains || REX::FModule::IsRuntimeNG()) return;
	constraintsInstalled = stl::detour_thunk_gateway<CursorConstraints>(
		REL::ID{907092, 2287480}, 6, "Pipboy cursor constraints");
	if (REX::FModule::IsRuntimeOG()) {
		// OG GameMenuBase::SetViewportRect already delegates to SetViewport,
		// using actual RT dimensions. It has no AE constant to find or rewrite.
		logger::info("[ENB UI] OG Pipboy cursor retains renderer-derived viewport");
	} else {
		// TEST RDX,RDX; JZ rel32. Relocate the conditional branch in the gateway.
		const auto address = REL::ID{1345371, 2248671}.address();
		const uint8_t prefix[]{0x48, 0x85, 0xD2, 0x0F, 0x84};
		if (stl::is_readable_memory(address, 9) && std::memcmp(reinterpret_cast<void*>(address), prefix, sizeof(prefix)) == 0)
			viewportInstalled = stl::detour_thunk_gateway<AECursorRenderer>(REL::ID{1345371, 2248671}, 9,
				"AE Pipboy cursor viewport", {{3, 5, 6}});
		else logger::error("[ENB UI] Unexpected AE cursor setup prologue; viewport correction unavailable");
	}
}
