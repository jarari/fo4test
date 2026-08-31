#pragma once

#include <array>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

class TaggedTextureDebug
{
public:
	static TaggedTextureDebug* GetSingleton()
	{
		static TaggedTextureDebug singleton;
		return &singleton;
	}

	void Render(
		ID3D12Device* a_device,
		ID3D12GraphicsCommandList* a_commandList,
		ID3D12Resource* a_backBuffer,
		ID3D12Resource* a_color,
		ID3D12Resource* a_depth,
		ID3D12Resource* a_motionVectors,
		ID3D12Resource* a_finalImage,
		DXGI_FORMAT a_backBufferFormat,
		uint32_t a_width,
		uint32_t a_height,
		uint32_t a_descriptorSlot,
		uint32_t a_descriptorSlotCount);

private:
	bool EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_descriptorSlotCount);
	void CreateSRV(ID3D12Device* a_device, ID3D12Resource* a_resource, uint32_t a_index);
	static constexpr uint32_t kSRVsPerSlot = 4;

	winrt::com_ptr<ID3D12Device> device;
	winrt::com_ptr<ID3D12RootSignature> rootSignature;
	winrt::com_ptr<ID3D12PipelineState> pipelineState;
	winrt::com_ptr<ID3D12DescriptorHeap> srvHeap;
	winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
	DXGI_FORMAT currentBackBufferFormat = DXGI_FORMAT_UNKNOWN;
	uint32_t currentDescriptorSlotCount = 0;
};
