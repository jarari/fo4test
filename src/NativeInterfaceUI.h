#pragma once

#include <cstdint>

namespace NativeInterfaceUI
{
	void ScaleLegacyNGPipboyLogicalSpace(uint32_t displayHeight);
	void InstallHooks(bool a_nativeDomains);
	void ReleaseResources();
	bool IsRendering();
	void RenderModelsBeforeUpscale(uint32_t a_target);
}
