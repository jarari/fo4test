#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>

namespace ReShadeDepth
{
	void Initialize();
	void SetOutputWindow(HWND window);
	bool IsActive();
	// Same raw, top-left render rectangle as the former D3D11 scene path.
	void Capture(ID3D11ShaderResourceView* depth, UINT width, UINT height, uint64_t frame);
	// Publish color provenance before entering either vendor's asynchronous Present.
	void PublishPresent(UINT slot, uint64_t frame, ID3D12Resource* backbuffer);
	// Invalidate a pending snapshot when this frame's inputs cannot be captured.
	void DiscardCapture(uint64_t frame);
	// FSR's public present callback supplies an independent frame counter and
	// the physical output resource. Keep it distinct from the engine frame counter.
	void TagFSRFrame(uint64_t backendFrame, uint64_t engineFrame);
	void TagFSRPresent(uint64_t backendFrame, ID3D12Resource* output, bool generated);
	// Caller has already drained interop work for swapchain resize/destruction.
	void ResetAfterIdle();
}
