#pragma once

#include <array>
#include <cstdint>

namespace nvngx::dlss_nr
{
	inline constexpr std::uint32_t kNativePerformanceMode = 6;
	struct Options
	{
		std::uint32_t performanceMode = kNativePerformanceMode;
		std::uint32_t preset = 0;
		std::uint32_t style = 0;
		float intensity = 1.0f;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = 1.0f;
		bool useAutoMask = false;
		bool operator==(const Options&) const = default;
	};

	constexpr std::array<Options, 2> DefaultAdditionalPassOptions()
	{
		std::array<Options, 2> result{};
		for (auto& options : result) options.localToneStrength = 0.0f;
		return result;
	}

	constexpr Options ResolvePassOptions(const Options& common, bool separate,
		const std::array<Options, 2>& additional, std::uint32_t pass)
	{
		if (pass > 0 && pass <= additional.size() && separate) return additional[pass - 1];
		auto result = common;
		// Preserve the legacy shared-settings behavior exactly.
		if (pass > 0) result.localToneStrength = 0.0f;
		return result;
	}
}
