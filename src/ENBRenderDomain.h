#pragma once

#include <algorithm>
#include <cstdint>

// ENB and the game see a full render-resolution allocation. The
// real swapchain stays display-sized; SR output never returns to D3D11.
class ENBRenderDomain
{
public:
	struct Extent
	{
		uint32_t width;
		uint32_t height;
	};
	static constexpr Extent CalculateExtent(uint32_t a_width, uint32_t a_height, uint32_t a_quality)
	{
		constexpr float ratios[]{ 1.0f, 1.5f, 1.7f, 2.0f, 3.0f };
		const auto ratio = ratios[std::min(a_quality, 4u)];
		return { std::max(1u, static_cast<uint32_t>(a_width / ratio)),
			std::max(1u, static_cast<uint32_t>(a_height / ratio)) };
	}

	static ENBRenderDomain& Get()
	{
		static ENBRenderDomain instance;
		return instance;
	}

	void Initialize(uint32_t a_displayWidth, uint32_t a_displayHeight);
	void ApplySceneDimensions();
	void SetQuality(uint32_t a_quality, uint32_t a_displayWidth, uint32_t a_displayHeight);
	bool qualityChangePending = false;
	void CancelInitialization();

	bool Active() const { return active; }
	uint32_t Width() const { return width; }
	uint32_t Height() const { return height; }
	uint32_t Quality() const { return quality; }

private:
	bool active = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t quality = 0;
};
