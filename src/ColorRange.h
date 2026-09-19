#pragma once

#include <dxgiformat.h>

namespace ColorRange
{
	// A floating-point scene buffer must not be processed as bounded [0, 1]
	// color. This describes its numeric range, NOT its transfer function:
	// an add-on may encode HDR in gamma space before its final Present pass.
	constexpr bool IsExtended(DXGI_FORMAT format)
	{
		switch (format) {
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R11G11B10_FLOAT:
			return true;
		default:
			return false;
		}
	}
}
