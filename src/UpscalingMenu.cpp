#include "UpscalingMenu.h"

#include "F4SEMenuFramework.h"
#include "Streamline.h"
#include "Upscaling.h"

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

	void __stdcall OnMenuEvent(F4SEMenuFramework::Events::Type a_type)
	{
		if (a_type == F4SEMenuFramework::Events::kOpenMenu) {
			InitializeEditState();
			return;
		}

		if (a_type != F4SEMenuFramework::Events::kCloseMenu) {
			return;
		}

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
		const auto streamline = Streamline::GetSingleton();
		if (streamline->initialized) {
			ImGuiMCP::TextDisabled(
				"Runtime: DLSS %s | Frame Generation %s | Reflex %s",
				streamline->featureDLSS ? "available" : "unavailable",
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

		ImGuiMCP::EndDisabled();

		ImGuiMCP::SeparatorText("Diagnostics");
		static constexpr std::array osdModes{ "Disabled", "Compact", "Detailed" };
		changed |= ComboSetting(
			"On-Screen Display",
			settings.osdMode,
			osdModes,
			"Shows D3D12 swapchain and upscaler status while DLSS or FSR is active.");
		changed |= CheckboxSetting(
			"Tagged Texture Debug View",
			settings.taggedTextureDebug,
			"Shows the color, depth, motion-vector, and final-image resources used by the D3D12 upscaler.");
		changed |= CheckboxSetting(
			"Image-Space Effect Log",
			settings.imageSpaceEffectLog,
			"Logs unique image-space effects dispatched inside the ENB native-resolution scope.");

		g_dirty |= changed;
	}
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

	g_registered = true;
	logger::info("[Menu] Registered Upscaling settings with F4SE Menu Framework");
}
