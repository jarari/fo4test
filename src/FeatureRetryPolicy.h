#pragma once

#include <algorithm>
#include <cstdint>

namespace FeatureRetryPolicy
{
	// Transient pressure may outlive a loading screen. Bound the retry rate,
	// not the number of attempts; only explicit permanent errors latch off.
	constexpr std::uint32_t NextAttempt(std::uint32_t attempts)
	{
		return std::min(attempts, 4u) + 1;
	}
	constexpr std::uint64_t DelayMilliseconds(std::uint32_t attempts)
	{
		return std::min<std::uint64_t>(1000ull << std::min(attempts, 5u), 30000);
	}
}
