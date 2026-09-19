#pragma once

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include "FrameCount.h"

class OSD
{
public:
	static OSD* GetSingleton()
	{
		static OSD singleton;
		return &singleton;
	}

	void Render(
		ID3D12Device* a_device,
		ID3D12GraphicsCommandList* a_commandList,
		winrt::com_ptr<ID3D12Resource>& a_upload,
		ID3D12Resource* a_backBuffer,
		uint32_t a_backBufferIndex,
		DXGI_FORMAT a_backBufferFormat,
		uint32_t a_width,
		uint32_t a_height);
	void Reset();

private:
	struct Vertex
	{
		float position[2];
		float uv[2];
	};

	bool EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_width, uint32_t a_height);
	void EnsureAdapter(ID3D12Device* a_device);
	void UpdateStats();
	void UpdateTexture(ID3D12GraphicsCommandList* a_commandList, winrt::com_ptr<ID3D12Resource>& a_upload);
	void Draw(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_backBuffer, uint32_t a_backBufferIndex);
	std::string BuildText() const;
	std::string BuildCompactText() const;
	std::string BuildDetailedText() const;

	winrt::com_ptr<ID3D12Device> device;
	winrt::com_ptr<IDXGIAdapter3> adapter;
	winrt::com_ptr<ID3D12RootSignature> rootSignature;
	winrt::com_ptr<ID3D12PipelineState> pipelineState;
	winrt::com_ptr<ID3D12DescriptorHeap> srvHeap;
	winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
	winrt::com_ptr<ID3D12Resource> texture;
	winrt::com_ptr<ID3D12Resource> vertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
	DXGI_FORMAT currentBackBufferFormat = DXGI_FORMAT_UNKNOWN;
	uint32_t currentWidth = 0;
	uint32_t currentHeight = 0;
	uint32_t textureWidth = 0;
	uint32_t textureHeight = 0;
	uint32_t uploadRowPitch = 0;
	uint64_t uploadSize = 0;
	std::array<ID3D12Resource*, kDX12FrameCount> rtvBackBuffers{};

	std::chrono::steady_clock::time_point lastFrameTime{};
	std::chrono::steady_clock::time_point sampleStart{};
	double frameTimeAccumMs = 0.0;
	uint32_t renderedFrames = 0;
	uint64_t lastFSRGeneratedFrameCount = 0;
	IDXGISwapChain4* fsrPresentCountChain = nullptr; // Identity only; not retained.
	UINT lastFSRPresentCount = 0;
	bool fsrPresentCountValid = false;
	double renderFPS = 0.0;
	double frameTimeMs = 0.0;
	double generatedFPS = 0.0;
	std::string gpuMemoryTotalText = "N/A";
	std::string gpuMemoryDetailText = "N/A";
	float reflexLatencyMs = 0.0f;
	std::string cachedText;
	uint32_t cachedMode = 0;
	bool textureDirty = true;
};
