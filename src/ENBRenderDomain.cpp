#include "ENBRenderDomain.h"
#include "ENBEffectDiagnostics.h"

#include <algorithm>

#include "Upscaling.h"
#include "Util.h"
#include "NativeUILayout.h"

extern bool enbLoaded;

static_assert(ENBRenderDomain::CalculateExtent(1600, 900, 1).width == 1066);
static_assert(ENBRenderDomain::CalculateExtent(1600, 900, 1).height == 600);
static_assert(ENBRenderDomain::CalculateExtent(1600, 900, 0).width == 1600);
static_assert(ENBRenderDomain::CalculateExtent(3840, 2160, 3).width == 1920);
static_assert(ENBRenderDomain::CalculateExtent(1, 1, 4).height == 1);

void ENBRenderDomain::Initialize(uint32_t a_displayWidth, uint32_t a_displayHeight)
{
	if (!enbLoaded || !a_displayWidth || !a_displayHeight) {
		return;
	}
	NativeUILayout::SetDisplaySize(a_displayWidth, a_displayHeight);
	if (active) {
		return;
	}
	// Read settings before ENB queries GetDesc and allocates private RTs.
	auto* upscaling = Upscaling::GetSingleton();
	upscaling->LoadSettings();
	ENBEffectDiagnostics::Install();
	quality = upscaling->settings.upscaleMethodPreference == 0 ? 0 :
		std::min(upscaling->settings.qualityMode, 4u);
	const auto extent = CalculateExtent(a_displayWidth, a_displayHeight, quality);
	width = extent.width;
	height = extent.height;
	active = true;
	logger::info("[ENB domain] scene={}x{} display={}x{} quality={}; low-resolution ENB -> LDR NR -> SR -> native screen-space UI. Live quality changes resize scene resources only; HWND and real swapchain unchanged",
		width, height, a_displayWidth, a_displayHeight, quality);
}

void ENBRenderDomain::ApplySceneDimensions()
{
	if (!active) {
		return;
	}
	auto* state = Util::State_GetSingleton();
	state->screenWidth = state->backBufferWidth = width;
	state->screenHeight = state->backBufferHeight = height;
	// Only own allocation dimensions here. The engine owns camera/viewport state;
	// a pixel-sized State rectangle is not a replacement for its camera viewport.
}

void ENBRenderDomain::SetQuality(uint32_t a_quality, uint32_t a_displayWidth, uint32_t a_displayHeight)
{
	quality = std::min(a_quality, 4u);
	const auto extent = CalculateExtent(a_displayWidth, a_displayHeight, quality);
	width = extent.width;
	height = extent.height;
	ApplySceneDimensions();
}

void ENBRenderDomain::CancelInitialization()
{
	NativeUILayout::Restore();
	active = false;
}
