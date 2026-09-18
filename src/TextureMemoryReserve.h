#pragma once

#include <algorithm>
#include <cstdint>

namespace TextureMemoryReserve
{
	inline constexpr std::uint64_t MiB = 1024ull * 1024ull;

	inline constexpr std::uint64_t MaxReserve = 2048 * MiB;
	enum class SR { None, DLSS, FSR };
	enum class FG { None, DLSS, FSR };
	struct Configuration
	{
		std::uint32_t width{}, height{}, renderWidth{}, renderHeight{};
		std::uint32_t colorBytes = 4, slots = 3, backbuffers = 3;
		SR sr = SR::None;
		FG fg = FG::None;
		std::uint32_t generatedFrames = 1, nrPasses = 0;
		bool nrAfterSR = false, sharpen = false, enb = false, reshadeDepth = false;
	};
	struct Estimate
	{
		std::uint64_t resident{}, sr{}, fg{}, nr{}, ui{}, depth{}, reserve{};
	};

	// Planning estimates, not residency measurements. Exclude engine-owned world
	// textures and count shared D3D11/D3D12 views once. SDK costs are allowances,
	// not known texture inventories. Third-party effects/captures are excluded.
	constexpr Estimate Calculate(const Configuration& c)
	{
		Estimate e{};
		if (!c.width || !c.height || !c.renderWidth || !c.renderHeight) return e;
		const auto pixels = [](std::uint32_t w, std::uint32_t h) {
			return std::uint64_t{ std::min(w, 32768u) } * std::min(h, 32768u);
		};
		const auto output = pixels(c.width, c.height);
		const auto input = pixels(c.renderWidth, c.renderHeight);
		const auto color = std::clamp(c.colorBytes, 1u, 16u);
		const auto slots = std::clamp(c.slots, 1u, 8u);
		// Presentation buffers, per-slot interop snapshots, proxy and fixed slack.
		e.resident = 128 * MiB + output * color * (std::clamp(c.backbuffers, 1u, 16u) + slots + 1);
		if (c.sr != SR::None) {
			// Color, RG16F motion, R32F depth, R8 mask and full-size output slots.
			e.sr = slots * (input * (color + 4 + 4 + 1) + output * color);
			if (c.sr == SR::FSR) e.sr += slots * input * color; // opaque-only input
			if (c.sharpen || c.nrPasses) e.sr += slots * output * color;
			e.sr += (c.sr == SR::DLSS ? 64 : 48) * MiB + input * 32 + output * 16;
		}
		if (c.fg != FG::None) {
			const auto generated = c.fg == FG::FSR ? 1u : std::clamp(c.generatedFrames, 1u, 5u);
			// HUD-less color, independent FG guides and generated output allowance.
			e.fg = slots * (output * color + input * 8) + generated * output * color;
			e.fg += (c.fg == FG::DLSS ? 96 : 64) * MiB + output * 24 + input * 8;
		}
		if (c.sr == SR::DLSS && c.nrPasses) {
			const auto passes = std::clamp(c.nrPasses, 1u, 3u);
			const auto nrPixels = c.nrAfterSR ? output : input;
			// Callback samples: ~221 MiB at 1129x635, ~377 at 1080p, one pass.
			// No unmeasured preset/style/intensity multipliers.
			e.nr = passes * (160 * MiB + nrPixels * 110);
			// Color/output already counted in SR. Only post-SR needs private depth.
			e.nr += slots * nrPixels * (c.nrAfterSR ? 8 : 4);
			e.nr += (passes - 1) * nrPixels * color;
		}
		if (c.enb) {
			e.ui = output * color * (slots + 1);
			if (input != output) {
				// Shared model cache allowance: 8 RGBA8, 4 FP16, two 32-bit depths.
				// Never multiply by Interface3D renderer count.
				e.ui += output * (8 * 4 + 4 * 8 + 2 * 4);
			}
		}
		if (c.reshadeDepth) e.depth = output * 4 * (slots + 1);
		const auto total = e.resident + e.sr + e.fg + e.nr + e.ui + e.depth;
		// Quantize to 64 MiB; this remains a bounded headroom policy.
		e.reserve = std::clamp((total + 64 * MiB - 1) / (64 * MiB) * (64 * MiB), 256 * MiB, MaxReserve);
		return e;
	}

	constexpr std::uint64_t UpgradeLimit(std::uint64_t original, std::uint64_t reserve)
	{
		// Keep at least half of the original budget for engine textures.
		return original - std::min({ reserve, MaxReserve, original / 2 });
	}
}
