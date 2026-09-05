#pragma once

namespace ENBEffectDiagnostics
{
	void RegisterModule(HMODULE a_module);
	// Called once during domain initialization, after LoadSettings completes.
	void Install();
	void BeforeResize();
	bool RequestCapture();
}
