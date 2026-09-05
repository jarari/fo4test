#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

class D3D12UIComposite
{
public:
	static D3D12UIComposite* GetSingleton()
	{
		static D3D12UIComposite singleton;
		return &singleton;
	}

	// Null postUI means a spatial-only resolve of baseColor (including R -> D).
	void Render(
		ID3D12Device* a_device,
		ID3D12GraphicsCommandList* a_commandList,
		ID3D12Resource* a_backBuffer,
		ID3D12Resource* a_baseColor,
		ID3D12Resource* a_postUI,
		DXGI_FORMAT a_backBufferFormat,
		uint32_t a_width,
		uint32_t a_height,
		uint32_t a_descriptorSlot,
		uint32_t a_descriptorSlotCount);

private:
	bool EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_descriptorSlotCount);
	void CreateSRV(ID3D12Device* a_device, ID3D12Resource* a_resource, uint32_t a_index);

	static constexpr uint32_t kSRVsPerSlot = 2;

	winrt::com_ptr<ID3D12Device> device;
	winrt::com_ptr<ID3D12RootSignature> rootSignature;
	winrt::com_ptr<ID3D12PipelineState> pipelineState;
	winrt::com_ptr<ID3D12DescriptorHeap> srvHeap;
	winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
	DXGI_FORMAT currentBackBufferFormat = DXGI_FORMAT_UNKNOWN;
	uint32_t currentDescriptorSlotCount = 0;
};
