#pragma once

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include "FrameCount.h"

class OSD
{
	struct Resources12;

public:
	struct Frame12
	{
		std::shared_ptr<Resources12> resources;
		winrt::com_ptr<ID3D12Resource> upload;
	};

	static OSD* GetSingleton()
	{
		static OSD singleton;
		return &singleton;
	}

	void Render(
		ID3D12Device* a_device,
		ID3D12GraphicsCommandList* a_commandList,
		Frame12& a_frame,
		ID3D12Resource* a_backBuffer,
		uint32_t a_backBufferIndex,
		DXGI_FORMAT a_backBufferFormat,
		uint32_t a_width,
		uint32_t a_height);
	void RenderNative(ID3D11Device* a_device, ID3D11DeviceContext1* a_context, ID3D11Texture2D* a_backBuffer);
	void RenderNative(IDXGISwapChain* a_swapChain);
	void Reset();

private:
	struct Pipeline12
	{
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> pipelineState;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	};

	struct Resources12
	{
		std::shared_ptr<Pipeline12> pipeline;
		winrt::com_ptr<ID3D12DescriptorHeap> srvHeap;
		winrt::com_ptr<ID3D12DescriptorHeap> rtvHeap;
		winrt::com_ptr<ID3D12Resource> texture;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t uploadRowPitch = 0;
		uint64_t uploadSize = 0;
	};

	struct Resources11
	{
		struct Panel
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> view;
		};

		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3DDeviceContextState> contextState;
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		winrt::com_ptr<ID3D11PixelShader> pixelShader;
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		std::array<Panel, 2> panels;
	};

	static void CreatePipeline12(Pipeline12& a_pipeline);
	bool EnsurePipeline12(ID3D12Device* a_device, DXGI_FORMAT a_format);
	bool EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_mode);
	bool EnsureNativeResources(ID3D11Device* a_device, uint32_t a_mode);
	void EnsureAdapter(ID3D12Device* a_device);
	void UpdateStats(uint32_t a_mode);
	bool UpdateTexture(ID3D12GraphicsCommandList* a_commandList, winrt::com_ptr<ID3D12Resource>& a_upload);
	void Draw(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_backBuffer, uint32_t a_backBufferIndex, uint32_t a_width, uint32_t a_height);
	std::string BuildText() const;
	std::string BuildCompactText() const;
	std::string BuildDetailedText() const;

	winrt::com_ptr<IDXGIAdapter3> adapter;
	std::shared_ptr<Pipeline12> pipeline12;
	std::future<std::shared_ptr<Pipeline12>> pendingPipeline12;
	std::chrono::steady_clock::time_point retryPipeline12{};
	std::shared_ptr<Resources12> resources12;
	std::array<std::shared_ptr<Resources12>, 2> modeResources12;
	Resources11 resources11;
	std::vector<uint32_t> pixels;

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
	float appGPULatencyMs = 0.0f;
	std::string cachedText;
	uint32_t cachedMode = 0;
	bool textureDirty = true;
};
