#pragma once

#include <cstdint>

namespace NativeInterfaceUI
{
	void InstallHooks();
	void ReleaseResources();
	bool IsRendering();
	void RenderModelsBeforeUpscale(uint32_t a_target);
}
