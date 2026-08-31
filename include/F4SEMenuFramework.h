#pragma once

// Minimal consumer-side bridge for the F4SE Menu Framework 3 API used by this
// plugin. The framework intentionally exposes a C ABI, so consumers resolve it
// at runtime and do not link against an import library.

#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>
#include <Windows.h>

namespace F4SEMenuFramework
{
	using RenderFunction = void(__stdcall*)();

	namespace detail
	{
		inline HMODULE GetModule() noexcept
		{
			auto module = ::GetModuleHandleW(L"F4SEMenuFramework.dll");
			return module ? module : ::GetModuleHandleW(L"F4SEMenuFramework");
		}

		template <class T>
		T GetFunction(const char* a_name) noexcept
		{
			const auto module = GetModule();
			return module ? reinterpret_cast<T>(::GetProcAddress(module, a_name)) : nullptr;
		}

		inline std::string section;
	}

	inline bool IsInstalled() noexcept
	{
		return detail::GetModule() != nullptr;
	}

	inline void SetSection(std::string_view a_section)
	{
		detail::section.assign(a_section);
	}

	inline bool AddSectionItem(std::string_view a_name, RenderFunction a_render)
	{
		using Function = void (*)(const char*, RenderFunction);
		const auto function = detail::GetFunction<Function>("AddSectionItem");
		if (!function) {
			return false;
		}

		std::string path = detail::section;
		path.push_back('/');
		path.append(a_name);
		function(path.c_str(), a_render);
		return true;
	}

	namespace Events
	{
		enum Type : std::int32_t
		{
			kNone = 0,
			kOpenMenu = 1,
			kCloseMenu = 2,
			kBeforeRender = 3,
			kAfterRender = 4
		};

		using Callback = void(__stdcall*)(Type);

		inline std::int64_t Register(Callback a_callback)
		{
			using Function = std::int64_t (*)(Callback);
			const auto function = detail::GetFunction<Function>("RegisterEvent");
			return function ? function(a_callback) : -1;
		}
	}
}

namespace ImGuiMCP
{
	using ImGuiHoveredFlags = std::int32_t;
	using ImGuiSliderFlags = std::int32_t;

	struct ImVec2
	{
		float x;
		float y;

		constexpr ImVec2(float a_x = 0.0f, float a_y = 0.0f) noexcept :
			x(a_x), y(a_y)
		{}
	};

	inline void Text(const char* a_format, ...)
	{
		using Function = void (*)(const char*, va_list);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igTextV");
		if (!function) {
			return;
		}

		va_list args;
		va_start(args, a_format);
		function(a_format, args);
		va_end(args);
	}

	inline void TextDisabled(const char* a_format, ...)
	{
		using Function = void (*)(const char*, va_list);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igTextDisabledV");
		if (!function) {
			return;
		}

		va_list args;
		va_start(args, a_format);
		function(a_format, args);
		va_end(args);
	}

	inline void TextWrapped(const char* a_format, ...)
	{
		using Function = void (*)(const char*, va_list);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igTextWrappedV");
		if (!function) {
			return;
		}

		va_list args;
		va_start(args, a_format);
		function(a_format, args);
		va_end(args);
	}

	inline void SeparatorText(const char* a_label)
	{
		using Function = void (*)(const char*);
		if (const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igSeparatorText")) {
			function(a_label);
		}
	}

	inline bool Checkbox(const char* a_label, bool* a_value)
	{
		using Function = bool (*)(const char*, bool*);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igCheckbox");
		return function && function(a_label, a_value);
	}

	inline bool Combo(const char* a_label, int* a_currentItem, const char* const a_items[], int a_itemCount, int a_popupHeight = -1)
	{
		using Function = bool (*)(const char*, int*, const char* const*, int, int);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igCombo_Str_arr");
		return function && function(a_label, a_currentItem, a_items, a_itemCount, a_popupHeight);
	}

	inline bool SliderFloat(const char* a_label, float* a_value, float a_min, float a_max, const char* a_format = "%.3f", ImGuiSliderFlags a_flags = 0)
	{
		using Function = bool (*)(const char*, float*, float, float, const char*, ImGuiSliderFlags);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igSliderFloat");
		return function && function(a_label, a_value, a_min, a_max, a_format, a_flags);
	}

	inline bool SliderInt(const char* a_label, int* a_value, int a_min, int a_max, const char* a_format = "%d", ImGuiSliderFlags a_flags = 0)
	{
		using Function = bool (*)(const char*, int*, int, int, const char*, ImGuiSliderFlags);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igSliderInt");
		return function && function(a_label, a_value, a_min, a_max, a_format, a_flags);
	}

	inline void BeginDisabled(bool a_disabled = true)
	{
		using Function = void (*)(bool);
		if (const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igBeginDisabled")) {
			function(a_disabled);
		}
	}

	inline void EndDisabled()
	{
		using Function = void (*)();
		if (const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igEndDisabled")) {
			function();
		}
	}

	inline bool IsItemHovered(ImGuiHoveredFlags a_flags = 0)
	{
		using Function = bool (*)(ImGuiHoveredFlags);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igIsItemHovered");
		return function && function(a_flags);
	}

	inline void SetTooltip(const char* a_format, ...)
	{
		using Function = void (*)(const char*, va_list);
		const auto function = F4SEMenuFramework::detail::GetFunction<Function>("igSetTooltipV");
		if (!function) {
			return;
		}

		va_list args;
		va_start(args, a_format);
		function(a_format, args);
		va_end(args);
	}
}
