#pragma once

#include <chrono>

#include "Upscaling.h"
#include "Util.h"

// CPU wall time only: recording GPU work or enqueueing a queue wait does not
// measure its GPU duration. Scopes may nest; their inclusive times must not be summed.
class ScopedRenderCPUProfile
{
public:
	explicit ScopedRenderCPUProfile(const char* a_stage, const char* a_detail = nullptr) : stage_(a_stage), detail_(a_detail)
	{
		const auto* upscaling = Upscaling::GetSingleton();
		if (!upscaling->settings.enbGPUTiming) {
			return;
		}
		const auto* state = Util::State_GetSingleton();
		if (!state || state->frameCount % 120 != 0) {
			return;
		}
		frame_ = state->frameCount;
		quality_ = upscaling->settings.qualityMode;
		active_ = true;
		start_ = Clock::now();
	}

	~ScopedRenderCPUProfile()
	{
		if (active_) {
			const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
			if (detail_) {
				logger::info("[Render CPU] {} [{}]: {:.3f} ms (quality={}, frame={})", stage_, detail_, elapsed, quality_, frame_);
			} else {
				logger::info("[Render CPU] {}: {:.3f} ms (quality={}, frame={})", stage_, elapsed, quality_, frame_);
			}
		}
	}

	ScopedRenderCPUProfile(const ScopedRenderCPUProfile&) = delete;
	ScopedRenderCPUProfile& operator=(const ScopedRenderCPUProfile&) = delete;

private:
	using Clock = std::chrono::steady_clock;
	const char* stage_;
	const char* detail_;
	Clock::time_point start_{};
	std::uint64_t frame_ = 0;
	uint quality_ = 0;
	bool active_ = false;
};
