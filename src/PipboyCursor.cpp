#include "PipboyCursor.h"
#include "DX12SwapChain.h"
#include "ENBRenderDomain.h"
#include "Util.h"
#include <Scaleform/G/GFx_Viewport.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	bool constraintsInstalled = false;
	bool viewportInstalled = false;

	struct WristCursorViewport
	{
		Scaleform::GFx::Movie* movie = nullptr;
		Scaleform::GFx::Viewport original{};
		Scaleform::GFx::Viewport applied{};
		std::array<double, 2> originalProjection{};
		std::array<double, 2> originalScale{};
		std::array<double, 2> appliedScale{};
		uint32_t left = 0;
		uint32_t top = 0;
		uint32_t right = 0;
		uint32_t bottom = 0;
		int mapTab = -1;
		bool hasMapBounds = false;
		ULONGLONG nextMapProbe = 0;
	};

	// RenderAll owns the viewport. A constraint notification delivered from
	// another thread uses the engine's rectangle until the next RenderAll.
	thread_local WristCursorViewport wristViewport;

	int MapTab(const RE::PipboyMenu& menu)
	{
		const auto index = [&](const char* name) -> double {
			Scaleform::GFx::Value value;
			if (!menu.dataObj.IsObject() || !menu.dataObj.GetMember(name, &value)) return -1;
			if (value.IsUInt()) return value.GetUInt();
			if (value.IsInt()) return value.GetInt();
			return value.IsNumber() ? value.GetNumber() : -1;
		};
		if (index("CurrentPage") != 3) return -1;
		return index("CurrentTab") == 1 ? 1 : 0;
	}

	bool ReadMapBounds(const RE::PipboyMenu& menu, int tab, Scaleform::Render::Rect<float>& rect)
	{
		Scaleform::GFx::Value page, mask, stage, bounds;
		if (!menu.menuObj.IsDisplayObject() || !menu.menuObj.GetMember("CurrentPage", &page) || !page.IsDisplayObject() ||
			!page.GetMember(tab == 1 ? "LocalMapMask_mc" : "WorldMapMask_mc", &mask) || !mask.IsDisplayObject() ||
			!mask.GetMember("stage", &stage) || !stage.IsDisplayObject() ||
			!mask.Invoke("getBounds", &bounds, &stage, 1) || !bounds.IsObject()) return false;
		const auto coord = [&](const char* key, float& dst) {
			Scaleform::GFx::Value value;
			if (!bounds.GetMember(key, &value) || !value.IsNumber()) return false;
			dst = static_cast<float>(value.GetNumber());
			return std::isfinite(dst);
		};
		return coord("left", rect.x1) && coord("top", rect.y1) && coord("right", rect.x2) &&
			coord("bottom", rect.y2) && rect.x1 < rect.x2 && rect.y1 < rect.y2;
	}

	bool IsWristGamepadCursor()
	{
		using IsInPowerArmor = bool (*)(const RE::Actor&);
		using IsUsingGamepad = bool (*)(const RE::BSInputDeviceManager*);
		static REL::Relocation<IsInPowerArmor> inPowerArmor{ REL::ID{ 1176757, 2219437 } };
		static REL::Relocation<IsUsingGamepad> usingGamepad{ REL::ID{ 875683, 2268386 } };
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* input = RE::BSInputDeviceManager::GetSingleton();
		return player && input && usingGamepad(input) && !inPowerArmor(*player);
	}

	bool SameViewport(const Scaleform::GFx::Viewport& lhs, const Scaleform::GFx::Viewport& rhs)
	{
		return lhs.bufferWidth == rhs.bufferWidth && lhs.bufferHeight == rhs.bufferHeight &&
			lhs.left == rhs.left && lhs.top == rhs.top && lhs.width == rhs.width && lhs.height == rhs.height &&
			lhs.scissorLeft == rhs.scissorLeft && lhs.scissorTop == rhs.scissorTop &&
			lhs.scissorWidth == rhs.scissorWidth && lhs.scissorHeight == rhs.scissorHeight &&
			lhs.flags == rhs.flags && lhs.scale == rhs.scale && lhs.aspectRatio == rhs.aspectRatio;
	}

	std::array<double, 2> ProjectStageUnits(Scaleform::GFx::Movie& movie)
	{
		const Scaleform::Render::Rect<float> area{{0, 0, 100, 100}};
		const auto projected = movie.TranslateToScreen(area);
		return {(projected.x2 - projected.x1) / 100.0, (projected.y2 - projected.y1) / 100.0};
	}

	bool ReadSpriteScale(const RE::CursorMenu& menu, std::array<double, 2>& scale)
	{
		if (!menu.cursor || !menu.cursor->IsDisplayObject()) return false;
		Scaleform::GFx::Value x, y;
		if (!menu.cursor->GetMember("scaleX", &x) || !x.IsNumber() ||
			!menu.cursor->GetMember("scaleY", &y) || !y.IsNumber()) return false;
		scale = {x.GetNumber(), y.GetNumber()};
		return std::isfinite(scale[0]) && std::isfinite(scale[1]);
	}

	bool SetSpriteScale(RE::CursorMenu& menu, const std::array<double, 2>& scale, double previousX)
	{
		if (!menu.cursor->SetMember("scaleX", Scaleform::GFx::Value(scale[0]))) return false;
		if (menu.cursor->SetMember("scaleY", Scaleform::GFx::Value(scale[1]))) return true;
		menu.cursor->SetMember("scaleX", Scaleform::GFx::Value(previousX));
		return false;
	}

	bool HasAppliedSpriteScale(const std::array<double, 2>& scale)
	{
		return wristViewport.appliedScale[0] != 0 && wristViewport.appliedScale[1] != 0 &&
			std::abs(scale[0] - wristViewport.appliedScale[0]) <= 0.0001 &&
			std::abs(scale[1] - wristViewport.appliedScale[1]) <= 0.0001;
	}

	void PreserveSpriteSize(RE::CursorMenu& menu, const Scaleform::GFx::Viewport& target)
	{
		std::array<double, 2> scale{};
		if (!ReadSpriteScale(menu, scale)) return;
		if (!HasAppliedSpriteScale(scale)) wristViewport.originalScale = scale;

		const auto cursorProjection = ProjectStageUnits(*menu.uiMovie);
		// The wrist mesh samples the whole RT. Keep the arrow's UV footprint
		// equal to the stock 876x700 target and compensate for the change in
		// CursorMenu's stage scale. Different RT aspect ratios need each axis.
		const std::array targetDensity{target.bufferWidth / 876.0, target.bufferHeight / 700.0};
		auto desired = wristViewport.originalScale;
		for (size_t axis = 0; axis < desired.size(); ++axis) {
			const double compensation = wristViewport.originalProjection[axis] * targetDensity[axis] / cursorProjection[axis];
			if (!std::isfinite(compensation) || compensation <= 0) return;
			desired[axis] *= compensation;
			if (!std::isfinite(desired[axis])) return;
		}
		if (SetSpriteScale(menu, desired, scale[0])) wristViewport.appliedScale = desired;
	}

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
			if (wristViewport.movie && IsWristGamepadCursor()) {
				ApplyWristViewport(cursor, true);
				return;
			}
			SetDisplayBounds(cursor, display.Width, display.Height);
			if (cursor->maxCursorX <= cursor->minCursorX || cursor->maxCursorY <= cursor->minCursorY) {
				cursor->ClearConstraints();
				return; // Do not divide by zero in the engine implementation.
			}
			// The engine divides by the bounds and subtracts the requested
			// rectangle from the maximum. Intersect in 64 bits before calling it.
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

		static std::array<float, 4> WristMargins(const RE::MenuCursor& cursor)
		{
			const float width = static_cast<float>(cursor.maxCursorX);
			const float height = static_cast<float>(cursor.maxCursorY);
			return {wristViewport.left / width, wristViewport.top / height,
				(width - wristViewport.right) / width, (height - wristViewport.bottom) / height};
		}

		static bool HasWristMargins(const RE::MenuCursor& cursor)
		{
			if (cursor.maxCursorX <= 0 || cursor.maxCursorY <= 0) return false;
			const auto margins = WristMargins(cursor);
			return std::abs(cursor.leftConstraintPct - margins[0]) < 0.00001f &&
				std::abs(cursor.topConstraintPct - margins[1]) < 0.00001f &&
				std::abs(cursor.rightConstraintPct - margins[2]) < 0.00001f &&
				std::abs(cursor.bottomConstraintPct - margins[3]) < 0.00001f;
		}

		static void ApplyWristViewport(RE::MenuCursor* cursor, bool force = false)
		{
			const auto& display = DX12SwapChain::GetSingleton()->swapChainDesc;
			const auto& vp = wristViewport.applied;
			SetDisplayBounds(cursor, std::max(display.Width, static_cast<uint32_t>(vp.bufferWidth)),
				std::max(display.Height, static_cast<uint32_t>(vp.bufferHeight)));
			if (!cursor || cursor->maxCursorX <= 0 || cursor->maxCursorY <= 0) return;

			if (force || !HasWristMargins(*cursor)) {
				func(cursor, wristViewport.left, wristViewport.top, wristViewport.right - wristViewport.left,
					wristViewport.bottom - wristViewport.top);
			}
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
				menu->customRendererName != "PipboyMenu" || menu->uiMovie.get() == wristViewport.movie) return;
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
	if (constraintsInstalled && ENBRenderDomain::Get().Active() && !wristViewport.movie)
		SetDisplayBounds(RE::MenuCursor::GetSingleton(), width, height);
}

void PipboyCursor::RefreshViewport()
{
	if (!constraintsInstalled || !ENBRenderDomain::Get().Active()) return;
	auto* ui = RE::UI::GetSingleton();
	if (!ui) return;
	const auto cursorMenu = ui->GetMenu<RE::CursorMenu>();
	auto* movie = cursorMenu ? cursorMenu->uiMovie.get() : nullptr;
	auto* cursor = RE::MenuCursor::GetSingleton();
	const auto pipboy = movie && cursor && cursorMenu->customRendererName == "PipboyMenu" ?
		ui->GetMenu<RE::PipboyMenu>() : Scaleform::Ptr<RE::PipboyMenu>{};
	if (pipboy && pipboy->OnStack() && pipboy->uiMovie && IsWristGamepadCursor()) {
		Scaleform::GFx::Viewport target{};
		pipboy->uiMovie->GetViewport(&target);
		if (target.width > 0 && target.height > 0 &&
			target.bufferWidth >= target.width && target.bufferHeight >= target.height) {
			Scaleform::GFx::Viewport current{};
			movie->GetViewport(&current);
			const bool changed = movie != wristViewport.movie || !SameViewport(target, wristViewport.applied);
			const bool newOriginal = movie != wristViewport.movie || !SameViewport(current, wristViewport.applied);
			const auto mapTab = MapTab(*pipboy);
			bool boundsChanged = changed || mapTab != wristViewport.mapTab;
			if (boundsChanged || (mapTab >= 0 && !wristViewport.hasMapBounds &&
					GetTickCount64() >= wristViewport.nextMapProbe)) {
				// These constraints are authored in the Pip-Boy movie's 876x700
				// space. GFx applies the actual scale mode and letterbox offset
				// when mapping them to the displayed portion of the render target.
				const auto* left = RE::GetINISetting("uPipboyConstraintTLX:Pipboy");
				const auto* top = RE::GetINISetting("uPipboyConstraintTLY:Pipboy");
				const auto* width = RE::GetINISetting("uPipboyConstraintWidth:Pipboy");
				const auto* height = RE::GetINISetting("uPipboyConstraintHeight:Pipboy");
				if (!left || !top || !width || !height) return;
				const float x = static_cast<float>(left->GetUInt());
				const float y = static_cast<float>(top->GetUInt());
				Scaleform::Render::Rect<float> authored{{ x, y,
					x + static_cast<float>(width->GetUInt()), y + static_cast<float>(height->GetUInt()) }};
				Scaleform::Render::Rect<float> mask{};
				const bool hasMask = mapTab >= 0 && ReadMapBounds(*pipboy, mapTab, mask);
				if (mapTab >= 0 && !hasMask) wristViewport.nextMapProbe = GetTickCount64() + 200;
				if (hasMask) authored = mask;
				const auto visible = pipboy->uiMovie->TranslateToScreen(authored);
				if (!std::isfinite(visible.x1) || !std::isfinite(visible.y1) ||
					!std::isfinite(visible.x2) || !std::isfinite(visible.y2)) return;
				const auto bound = [](float value, int32_t extent) {
					return static_cast<uint32_t>(std::clamp(value, 0.0f, static_cast<float>(extent - 1)));
				};
				// GFx projects the mask relative to the viewport origin. Input
				// events use buffer pixels, including the letterbox offset.
				const auto dx = hasMask ? target.left : 0;
				const auto dy = hasMask ? target.top : 0;
				const auto extentX = hasMask ? target.bufferWidth : target.width;
				const auto extentY = hasMask ? target.bufferHeight : target.height;
				const auto x1 = bound(std::ceil(visible.x1) + dx, extentX);
				const auto y1 = bound(std::ceil(visible.y1) + dy, extentY);
				const auto x2 = bound(std::floor(visible.x2) + dx, extentX);
				const auto y2 = bound(std::floor(visible.y2) + dy, extentY);
				if (x2 <= x1 || y2 <= y1) return;
				wristViewport.mapTab = mapTab;
				wristViewport.hasMapBounds = hasMask;
				boundsChanged = boundsChanged || wristViewport.left != x1 || wristViewport.right != x2 ||
					wristViewport.top != y1 || wristViewport.bottom != y2;
				wristViewport.left = x1;
				wristViewport.top = y1;
				wristViewport.right = x2;
				wristViewport.bottom = y2;
				if (boundsChanged) logger::info("[ENB UI] Wrist Pipboy cursor uses viewport {}x{} in {}x{}, bounds {},{} to {},{} mapTab={} mapMask={}",
					target.width, target.height, target.bufferWidth, target.bufferHeight, x1, y1, x2, y2, mapTab, hasMask);
			}
			if (newOriginal) {
				if (movie != wristViewport.movie) wristViewport.appliedScale = {};
				wristViewport.original = current;
				wristViewport.originalProjection = ProjectStageUnits(*movie);
			}
			if (!SameViewport(current, target)) movie->SetViewport(target);
			if (changed || newOriginal) PreserveSpriteSize(*cursorMenu, target);
			wristViewport.movie = movie;
			wristViewport.applied = target;
			CursorConstraints::ApplyWristViewport(cursor, boundsChanged);
			return;
		}
	}

	if (wristViewport.movie) {
		if (movie == wristViewport.movie) {
			std::array<double, 2> scale{};
			if (ReadSpriteScale(*cursorMenu, scale) && HasAppliedSpriteScale(scale))
				SetSpriteScale(*cursorMenu, wristViewport.originalScale, scale[0]);
			Scaleform::GFx::Viewport current{};
			movie->GetViewport(&current);
			if (SameViewport(current, wristViewport.applied)) movie->SetViewport(wristViewport.original);
		}
		if (cursor && CursorConstraints::HasWristMargins(*cursor)) cursor->ClearConstraints();
		wristViewport = {};
		const auto& display = DX12SwapChain::GetSingleton()->swapChainDesc;
		SetDisplayBounds(cursor, display.Width, display.Height);
	}
	if (viewportInstalled && cursorMenu) AECursorRenderer::CorrectViewport(cursorMenu.get());
}

void PipboyCursor::InstallHooks(bool nativeDomains)
{
	if (!nativeDomains || REX::FModule::IsRuntimeNG()) return;
	constraintsInstalled = stl::detour_thunk_gateway<CursorConstraints>(
		REL::ID{907092, 2287480}, 6, "Pipboy cursor constraints");
	if (REX::FModule::IsRuntimeOG()) {
		logger::info("[ENB UI] OG Pipboy cursor viewport will follow the wrist movie while using a gamepad");
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
