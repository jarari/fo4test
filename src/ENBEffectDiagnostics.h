#pragma once

namespace ENBEffectDiagnostics
{
	void RegisterModule(HMODULE a_module);
	// Called once during domain initialization, after LoadSettings completes.
	void Install();
	void BeforeResize();
	bool RequestCapture();
	// Null when available; otherwise a user-facing reason, including install failures.
	const char* CaptureUnavailableReason();
}
