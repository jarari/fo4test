#pragma once
#include <cstdint>
namespace PipboyCursor
{
	void RefreshViewport();
	void InstallHooks(bool nativeDomains);
	void UpdateDisplayBounds(uint32_t width, uint32_t height);
}
