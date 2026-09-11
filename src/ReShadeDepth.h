#pragma once

#include <winrt/base.h>

namespace ReShadeDepth
{
	void Initialize();
	bool IsRequested();
	// Called on the proxy queue after the D3D11 input-ready wait.
	bool Prepare(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_source, uint32_t a_slot,
		winrt::com_ptr<ID3D12Resource>& a_retainedSnapshot, bool a_beforePresent = false);
	// Call immediately after submitting the command list that recorded Prepare.
	void PublishSubmittedDepth();
	void EndPresent();
	void Invalidate();
	// Operational snapshot publication serial, not diagnostic instrumentation.
	void AdvancePresent();
}
