#pragma once

#include <cstdint>

namespace TextureMemoryReserve
{
	inline constexpr std::uint64_t MiB = 1024ull * 1024ull;

	// Planning headroom, not measured VRAM or a physical allocation. Includes
	// frame-slot color/depth/motion copies, UI/FP16 ReShade surfaces and an
	// allowance for SR/FG/NR internals. Shared D3D11/D3D12 views are not two
	// allocations. Arbitrary ReShade effects and diagnostic captures are excluded.
	// Use output pixels, never the SR/ENB scene size; keep room for feature toggles.
	constexpr std::uint64_t EstimateBytes(std::uint32_t width, std::uint32_t height)
	{
		const auto pixels = std::uint64_t{ width } * height;
		if (!pixels) return 0;  // Defer until the real swapchain has a size.
		if (pixels <= 1920ull * 1200) return 512 * MiB;
		if (pixels <= 2560ull * 1600) return 768 * MiB;
		// Cap headroom at 1 GiB, including 4K/8K and larger outputs.
		return 1024 * MiB;
	}

	constexpr std::uint64_t UpgradeLimit(std::uint64_t original, std::uint64_t reserve)
	{
		// Keep at least half of the original budget for engine textures.
		return original - (reserve < original / 2 ? reserve : original / 2);
	}
}
