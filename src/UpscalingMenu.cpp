#include "UpscalingMenu.h"

#include "F4SEMenuFramework.h"
#include <atomic>
#include "Streamline.h"
#include "Upscaling.h"
#include "ENBRenderDomain.h"

#include <array>
#include <mutex>
#include <optional>

namespace
{
	using Settings = Upscaling::Settings;

	std::mutex g_stateMutex;
	Settings g_editSettings;
	bool g_editInitialized = false;
	bool g_dirty = false;
	bool g_registered = false;
	std::int64_t g_menuEventHandle = -1;
	std::int64_t g_dlssNRHotkeyHandle = -1;
	bool g_dlssNRHotkeyCapturing = false;
	std::array<bool, 256> g_dlssNRHotkeyPreviousState{};
	constexpr auto kDLSSNRHotkeyID = "Upscaling.ToggleDLSSNR";

	std::string GetHotkeyKeyName(unsigned int a_code)
	{
		if (a_code == 0) {
			return "Unbound";
		}

		switch (a_code) {
		case 256:
			return "Left Mouse";
		case 257:
			return "Right Mouse";
		case 258:
			return "Middle Mouse";
		case 259:
			return "Mouse X1";
		case 260:
			return "Mouse X2";
		default:
			break;
		}

		// GetKeyNameText expects the scan code in bits 16-23 and the extended
		// flag in bit 24. Framework DIK codes encode that flag as bit 0x80.
		LONG keyData = static_cast<LONG>((a_code & 0x7F) << 16);
		if ((a_code & 0x80) != 0) {
			keyData |= 1 << 24;
		}

		char name[64]{};
		if (::GetKeyNameTextA(keyData, name, static_cast<int>(std::size(name))) > 0) {
			return name;
		}
		return std::format("Key 0x{:02X}", a_code);
	}

	unsigned int VirtualKeyToFrameworkHotkeyCode(int a_virtualKey)
	{
		// The framework uses the F4SE mouse-button range alongside DIK keyboard
		// scan codes.
		switch (a_virtualKey) {
		case VK_LBUTTON:
			return 256;
		case VK_RBUTTON:
			return 257;
		case VK_MBUTTON:
			return 258;
		case VK_XBUTTON1:
			return 259;
		case VK_XBUTTON2:
			return 260;
		default:
			break;
		}

		auto scanCode = ::MapVirtualKeyA(static_cast<UINT>(a_virtualKey), MAPVK_VK_TO_VSC);
		if (scanCode == 0) {
			return 0;
		}

		// DirectInput folds the E0 extended-key prefix into bit 0x80.
		switch (a_virtualKey) {
		case VK_LEFT:
		case VK_UP:
		case VK_RIGHT:
		case VK_DOWN:
		case VK_PRIOR:
		case VK_NEXT:
		case VK_END:
		case VK_HOME:
		case VK_INSERT:
		case VK_DELETE:
		case VK_DIVIDE:
		case VK_NUMLOCK:
		case VK_RCONTROL:
		case VK_RMENU:
			scanCode |= 0x80;
			break;
		default:
			break;
		}
		return scanCode;
	}

	void ShowHelp(const char* a_help)
	{
		if (ImGuiMCP::IsItemHovered()) {
			ImGuiMCP::SetTooltip("%s", a_help);
		}
	}

	template <std::size_t N>
	bool ComboSetting(const char* a_label, uint& a_value, const std::array<const char*, N>& a_items, const char* a_help)
	{
		int value = static_cast<int>(a_value);
		const bool changed = ImGuiMCP::Combo(a_label, &value, a_items.data(), static_cast<int>(a_items.size()));
		ShowHelp(a_help);
		if (changed) {
			a_value = static_cast<uint>(value);
		}
		return changed;
	}

	bool CheckboxSetting(const char* a_label, uint& a_value, const char* a_help)
	{
		bool value = a_value != 0;
		const bool changed = ImGuiMCP::Checkbox(a_label, &value);
		ShowHelp(a_help);
		if (changed) {
			a_value = value ? 1u : 0u;
		}
		return changed;
	}

	bool SliderIntSetting(const char* a_label, uint& a_value, int a_min, int a_max, const char* a_format, const char* a_help)
	{
		int value = static_cast<int>(a_value);
		const bool changed = ImGuiMCP::SliderInt(a_label, &value, a_min, a_max, a_format);
		ShowHelp(a_help);
		if (changed) {
			a_value = static_cast<uint>(value);
		}
		return changed;
	}

	bool SliderFloatSetting(const char* a_label, float& a_value, float a_min, float a_max, const char* a_format, const char* a_help)
	{
		const bool changed = ImGuiMCP::SliderFloat(a_label, &a_value, a_min, a_max, a_format);
		ShowHelp(a_help);
		return changed;
	}

	void BeginHotkeyCapture(bool& a_capturing, std::array<bool, 256>& a_previousState)
	{
		// Snapshot every VK when capture begins. The Bind click, held modifiers,
		// and persistent IME states (for example VK_HANGUL on Korean layouts)
		// must be released before they can produce a new press edge.
		for (int virtualKey = 0; virtualKey < static_cast<int>(a_previousState.size()); ++virtualKey) {
			a_previousState[virtualKey] = (::GetAsyncKeyState(virtualKey) & 0x8000) != 0;
		}
		a_capturing = true;
	}

	void HotkeySetting(
		const char* a_label,
		const char* a_id,
		bool& a_capturing,
		std::array<bool, 256>& a_previousState,
		const char* a_help)
	{
		const auto binding = F4SEMenuFramework::Hotkeys::GetBinding(a_id);
		const auto keyName = GetHotkeyKeyName(binding);
		ImGuiMCP::Text("%s: %s", a_label, keyName.c_str());
		ImGuiMCP::SameLine();

		if (a_capturing) {
			ImGuiMCP::TextDisabled("Press a key... (Esc cancels)");

			// Accept only an up -> down edge observed after capture started. This
			// prevents the Bind click or an already-active IME VK from being bound.
			for (int virtualKey = VK_LBUTTON; virtualKey <= 0xFE; ++virtualKey) {
				const bool isDown = (::GetAsyncKeyState(virtualKey) & 0x8000) != 0;
				const bool pressed = isDown && !a_previousState[virtualKey];
				a_previousState[virtualKey] = isDown;
				if (!pressed) {
					continue;
				}

				if (virtualKey == VK_ESCAPE) {
					a_capturing = false;
					break;
				}

				// Prefer the left/right-specific modifier VKs so Right Ctrl/Alt and
				// Right Shift do not collapse to their left-hand DIK codes.
				if (virtualKey == VK_SHIFT || virtualKey == VK_CONTROL || virtualKey == VK_MENU) {
					continue;
				}

				const auto frameworkCode = VirtualKeyToFrameworkHotkeyCode(virtualKey);
				if (frameworkCode == 0) {
					continue;
				}

				F4SEMenuFramework::Hotkeys::SetBinding(a_id, frameworkCode);
				a_capturing = false;
				break;
			}
			return;
		}

		if (ImGuiMCP::Button("Bind##dlssNRHotkey")) {
			BeginHotkeyCapture(a_capturing, a_previousState);
		}
		ShowHelp(a_help);

		if (binding != 0) {
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Clear##dlssNRHotkey")) {
				F4SEMenuFramework::Hotkeys::SetBinding(a_id, 0);
			}
			ShowHelp("Unbind the DLSS Neural Rendering toggle hotkey.");
		}
	}

	void InitializeEditState()
	{
		std::scoped_lock lock(g_stateMutex);
		g_editSettings = Upscaling::GetSingleton()->settings;
		g_editInitialized = true;
		g_dirty = false;
	}

	void QueueSettingsReload(bool a_force)
	{
		if (const auto tasks = F4SE::GetTaskInterface()) {
			tasks->AddTask([a_force] {
				if (a_force) {
					Upscaling::GetSingleton()->LoadSettings();
				} else {
					Upscaling::GetSingleton()->ReloadSettingsIfChanged();
				}
			});
		} else if (a_force) {
			Upscaling::GetSingleton()->LoadSettings();
		} else {
			Upscaling::GetSingleton()->ReloadSettingsIfChanged();
		}
	}

	void ToggleDLSSNR()
	{
		auto* upscaling = Upscaling::GetSingleton();
		auto updatedSettings = upscaling->settings;
		updatedSettings.dlssNREnabled = updatedSettings.dlssNREnabled == 0 ? 1u : 0u;
		if (!upscaling->SaveSettings(updatedSettings)) {
			logger::error("[Menu] Could not save DLSS-NR hotkey state");
			return;
		}

		upscaling->LoadSettings();
		logger::info("[Menu] DLSS Neural Rendering {} by hotkey", updatedSettings.dlssNREnabled != 0 ? "enabled" : "disabled");
	}

	void __stdcall OnToggleDLSSNRHotkey()
	{
		if (const auto tasks = F4SE::GetTaskInterface()) {
			tasks->AddTask([] { ToggleDLSSNR(); });
		} else {
			ToggleDLSSNR();
		}
	}

	std::atomic<bool> g_frameworkMenuOpen{ false };

	void __stdcall OnMenuEvent(F4SEMenuFramework::Events::Type a_type)
	{
		if (a_type == F4SEMenuFramework::Events::kOpenMenu) {
			g_frameworkMenuOpen.store(true, std::memory_order_relaxed);
			InitializeEditState();
			return;
		}

		if (a_type != F4SEMenuFramework::Events::kCloseMenu) {
			return;
		}
		g_frameworkMenuOpen.store(false, std::memory_order_relaxed);
		g_dlssNRHotkeyCapturing = false;

		std::optional<Settings> settingsToSave;
		{
			std::scoped_lock lock(g_stateMutex);
			if (g_editInitialized && g_dirty) {
				settingsToSave = g_editSettings;
			}
			g_editInitialized = false;
			g_dirty = false;
		}

		if (settingsToSave) {
			if (!Upscaling::GetSingleton()->SaveSettings(*settingsToSave)) {
				logger::error("[Menu] Could not save Upscaling settings");
				return;
			}
			QueueSettingsReload(true);
		} else {
			// Also catches settings written by another UI while the framework was open.
			QueueSettingsReload(false);
		}
	}

	void __stdcall RenderSettings()
	{
		std::scoped_lock lock(g_stateMutex);
		if (!g_editInitialized) {
			g_editSettings = Upscaling::GetSingleton()->settings;
			g_editInitialized = true;
			g_dirty = false;
		}

		auto& settings = g_editSettings;
		bool changed = false;

		ImGuiMCP::TextWrapped("Changes are saved and applied when the F4SE Menu Framework closes.");
		if (ENBRenderDomain::Get().Active()) {
			ImGuiMCP::TextWrapped("Resolution quality changes apply live after outstanding frames drain. Only scene/game/ENB buffers are resized; the window, real display swapchain and native UI target remain unchanged.");
			ImGuiMCP::TextDisabled("Active ENB scene: %u x %u (active quality %u)",
				ENBRenderDomain::Get().Width(), ENBRenderDomain::Get().Height(), ENBRenderDomain::Get().Quality());
		}
		const auto streamline = Streamline::GetSingleton();
		if (streamline->initialized) {
			ImGuiMCP::TextDisabled(
				"Runtime: DLSS %s | DLSS-NR %s | Frame Generation %s | Reflex %s",
				streamline->featureDLSS ? "available" : "unavailable",
				streamline->featureDLSSNR ? "available" : "direct path",
				streamline->featureDLSSG ? "available" : "unavailable",
				streamline->featureReflex ? "available" : "unavailable");
		}

		ImGuiMCP::SeparatorText("Upscaling");
		static constexpr std::array upscaleMethods{ "Disabled", "AMD FSR", "NVIDIA DLSS" };
		changed |= ComboSetting(
			"Upscale Method",
			settings.upscaleMethodPreference,
			upscaleMethods,
			"Selects the preferred temporal upscaler. DLSS falls back to FSR when unavailable.");

		static constexpr std::array qualityModes{ "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance" };
		changed |= ComboSetting(
			"Quality Mode",
			settings.qualityMode,
			qualityModes,
			"Controls the render resolution used by the temporal upscaler.");
		changed |= SliderFloatSetting(
			"Sharpness",
			settings.sharpness,
			0.0f,
			1.0f,
			"%.2f",
			"Controls NVIDIA Image Scaling sharpen for DLSS and RCAS for FSR.");

		ImGuiMCP::SeparatorText("Frame Generation and Latency");
		const bool upscalingDisabled = settings.upscaleMethodPreference == static_cast<uint>(Upscaling::UpscaleMethod::kDisabled);
		ImGuiMCP::BeginDisabled(upscalingDisabled);
		static constexpr std::array frameGenerationModes{ "Disabled", "On", "Auto" };
		changed |= ComboSetting(
			"Frame Generation",
			settings.frameGenerationMode,
			frameGenerationModes,
			"Uses the selected vendor's frame generation path when supported.");
		ImGuiMCP::EndDisabled();

		const bool frameGenerationDisabled = upscalingDisabled || settings.frameGenerationMode == 0;
		const bool dlssSelected = settings.upscaleMethodPreference == static_cast<uint>(Upscaling::UpscaleMethod::kDLSS);
		ImGuiMCP::BeginDisabled(frameGenerationDisabled || !dlssSelected);
		static constexpr std::array generatedFrameCounts{ "1 (2x)", "2 (3x)", "3 (4x)", "4 (5x)", "5 (6x)" };
		changed |= ComboSetting(
			"Generated Frames",
			settings.dlssgGeneratedFrames,
			generatedFrameCounts,
			"Controls the requested DLSS generated-frame multiplier. The runtime clamps unsupported values.");
		changed |= CheckboxSetting(
			"Dynamic Multi Frame Generation",
			settings.dynamicMFGEnabled,
			"Lets Streamline dynamically select the generated-frame multiplier when supported.");
		ImGuiMCP::BeginDisabled(settings.dynamicMFGEnabled == 0);
		changed |= SliderIntSetting(
			"Dynamic Target FPS",
			settings.dynamicMFGTargetFPS,
			0,
			500,
			"%d FPS",
			"Target output frame rate. Zero lets Streamline use the display refresh rate.");
		ImGuiMCP::EndDisabled();
		ImGuiMCP::EndDisabled();

		static constexpr std::array reflexModes{ "Off", "On", "On + Boost" };
		changed |= ComboSetting(
			"NVIDIA Reflex",
			settings.reflexMode,
			reflexModes,
			"Controls NVIDIA Reflex low-latency mode. Frame generation forces at least On while active.");

		ImGuiMCP::SeparatorText("DLSS");
		ImGuiMCP::BeginDisabled(!dlssSelected);
		static constexpr std::array dlssPresets{ "Recommended", "Default", "K", "M", "L" };
		changed |= ComboSetting(
			"Model Preset",
			settings.dlssModelPreset,
			dlssPresets,
			"Recommended uses K for DLAA/Quality/Balanced, M for Performance, and L for Ultra Performance.");

		changed |= CheckboxSetting(
			"DLSS Neural Rendering",
			settings.dlssNREnabled,
			"Runs the experimental DLSS-NR pass after DLSS SR. If NR fails, the prepared SR result is retained.");
		ImGuiMCP::BeginDisabled(settings.dlssNREnabled == 0);
		static constexpr std::array nrStyles{ "Default", "Natural", "Cinematic" };
		changed |= ComboSetting(
			"NR Style",
			settings.dlssNRStyle,
			nrStyles,
			"Selects the Neural Rendering appearance style.");
		changed |= SliderFloatSetting("NR Intensity", settings.dlssNRIntensity, 0.0f, 1.0f, "%.2f", "Controls overall Neural Rendering strength.");
		changed |= SliderFloatSetting("Local Tone Strength", settings.dlssNRLocalToneStrength, 0.0f, 1.0f, "%.2f", "Controls local tone enhancement.");
		changed |= SliderFloatSetting("Local Structure Strength", settings.dlssNRLocalStructureStrength, 0.0f, 1.0f, "%.2f", "Controls local structure enhancement.");
		changed |= CheckboxSetting(
			"Automatic Mask",
			settings.dlssNRUseAutoMask,
			"Lets DLSS-NR infer its own screen-space control mask when no application mask is supplied.");
		ImGuiMCP::BeginDisabled(settings.dlssNRUseAutoMask == 0);
		changed |= SliderFloatSetting(
			"Skin Structure Strength",
			settings.dlssNRSkinStructureStrength,
			-1.0f,
			1.0f,
			"%.2f",
			"Controls structure enhancement in inferred skin regions. -1 inherits Local Structure Strength.");
		ImGuiMCP::EndDisabled();
		ImGuiMCP::EndDisabled();
		ImGuiMCP::EndDisabled();

		ImGuiMCP::SeparatorText("Hotkeys");
		ImGuiMCP::BeginDisabled(g_dlssNRHotkeyHandle < 0);
		HotkeySetting(
			"DLSS NR Toggle Hotkey",
			kDLSSNRHotkeyID,
			g_dlssNRHotkeyCapturing,
			g_dlssNRHotkeyPreviousState,
			"Press Bind, then press a keyboard key or mouse button. Conflicts are confirmed by F4SE Menu Framework.");
		ImGuiMCP::EndDisabled();
		if (g_dlssNRHotkeyHandle < 0) {
			ImGuiMCP::TextDisabled("Hotkey binding requires a F4SE Menu Framework version with the plugin hotkey API.");
		}

		static constexpr std::array osdModes{ "Disabled", "Compact", "Detailed" };
		changed |= ComboSetting(
			"On-Screen Display",
			settings.osdMode,
			osdModes,
			"Shows D3D12 swapchain and upscaler status while DLSS or FSR is active.");

		g_dirty |= changed;
	}
}

bool UpscalingMenu::IsOpen()
{
	return g_frameworkMenuOpen.load(std::memory_order_relaxed);
}

void UpscalingMenu::Register()
{
	if (g_registered) {
		return;
	}

	if (!F4SEMenuFramework::IsInstalled()) {
		logger::warn("[Menu] F4SE Menu Framework is not installed; native settings page is unavailable");
		return;
	}

	F4SEMenuFramework::SetSection("Upscaling");
	if (!F4SEMenuFramework::AddSectionItem("Settings", RenderSettings)) {
		logger::error("[Menu] F4SE Menu Framework did not expose AddSectionItem");
		return;
	}

	g_menuEventHandle = F4SEMenuFramework::Events::Register(OnMenuEvent);
	if (g_menuEventHandle < 0) {
		logger::error("[Menu] F4SE Menu Framework did not expose menu lifecycle events");
		return;
	}

	// Scan code 0 intentionally leaves the action unbound until the player uses
	// the capture button in Upscaling's Hotkeys section.
	g_dlssNRHotkeyHandle = F4SEMenuFramework::Hotkeys::Register(kDLSSNRHotkeyID, 0, OnToggleDLSSNRHotkey);
	if (g_dlssNRHotkeyHandle < 0) {
		logger::warn("[Menu] F4SE Menu Framework did not expose the plugin hotkey API; DLSS-NR hotkey is unavailable");
	}

	g_registered = true;
	logger::info(
		"[Menu] Registered Upscaling settings{} with F4SE Menu Framework",
		g_dlssNRHotkeyHandle >= 0 ? " and DLSS-NR hotkey" : "");
}
