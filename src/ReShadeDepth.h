#pragma once

#include <winrt/base.h>

namespace ReShadeDepth
{
	void Initialize();
	bool IsRequested();
	// Publishes the D3D11-produced shared resource directly. The ready fence is
	// signaled by the dedicated D3D11 bridge, not by the SR/FG/NR queue.
	void PublishCapturedDepth(ID3D12Resource* a_resource, uint32_t a_sourceSlot,
		uint64_t a_sourceFrame, uint64_t a_readyFence);
	void Invalidate();
	void AdvancePresent();
}
