#include "TaggedTextureDebug.h"

#include <cstring>
#include <d3dcompiler.h>

#include "Buffer.h"

namespace
{
	void ThrowIfFailed(HRESULT a_result)
	{
		if (FAILED(a_result)) {
			throw DX::com_exception(a_result);
		}
	}

	winrt::com_ptr<ID3DBlob> CompileShader(const char* a_source, const char* a_entry, const char* a_target)
	{
		winrt::com_ptr<ID3DBlob> shader;
		winrt::com_ptr<ID3DBlob> errors;
		const auto flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
		const auto result = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr, a_entry, a_target, flags, 0, shader.put(), errors.put());
		if (FAILED(result) && errors) {
			logger::warn("[TaggedTextureDebug] Shader compile failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
		}
		ThrowIfFailed(result);
		return shader;
	}

	DXGI_FORMAT SRVFormat(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R32_TYPELESS:
			return DXGI_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_R24G8_TYPELESS:
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_R16_TYPELESS:
			return DXGI_FORMAT_R16_UNORM;
		case DXGI_FORMAT_R16G16_TYPELESS:
			return DXGI_FORMAT_R16G16_FLOAT;
		case DXGI_FORMAT_R32G32_TYPELESS:
			return DXGI_FORMAT_R32G32_FLOAT;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		default:
			return a_format;
		}
	}
}

bool TaggedTextureDebug::EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_descriptorSlotCount)
{
	if (!a_device || a_backBufferFormat == DXGI_FORMAT_UNKNOWN || a_descriptorSlotCount == 0) {
		return false;
	}

	if (device.get() == a_device &&
		pipelineState &&
		currentBackBufferFormat == a_backBufferFormat &&
		currentDescriptorSlotCount >= a_descriptorSlotCount) {
		return true;
	}

	device.copy_from(a_device);
	currentBackBufferFormat = a_backBufferFormat;
	currentDescriptorSlotCount = a_descriptorSlotCount;

	try {
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 4;
		range.BaseShaderRegister = 0;
		range.RegisterSpace = 0;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParameters[2]{};
		rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
		rootParameters[0].DescriptorTable.pDescriptorRanges = &range;
		rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rootParameters[1].Constants.ShaderRegister = 0;
		rootParameters[1].Constants.RegisterSpace = 0;
		rootParameters[1].Constants.Num32BitValues = 2;
		rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.ShaderRegister = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = static_cast<UINT>(std::size(rootParameters));
		rootDesc.pParameters = rootParameters;
		rootDesc.NumStaticSamplers = 1;
		rootDesc.pStaticSamplers = &sampler;
		rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		winrt::com_ptr<ID3DBlob> rootBlob;
		winrt::com_ptr<ID3DBlob> rootError;
		ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.put(), rootError.put()));
		ThrowIfFailed(a_device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put())));

		const char* shaderSource = R"(
Texture2D colorTexture : register(t0);
Texture2D depthTexture : register(t1);
Texture2D motionTexture : register(t2);
Texture2D finalTexture : register(t3);
SamplerState linearSampler : register(s0);
cbuffer DebugConstants : register(b0)
{
	float2 screenSize;
};

struct PSInput
{
	float4 position : SV_POSITION;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
	static const float2 positions[6] = {
		float2(-1.0f,  1.0f),
		float2( 1.0f,  1.0f),
		float2(-1.0f, -1.0f),
		float2(-1.0f, -1.0f),
		float2( 1.0f,  1.0f),
		float2( 1.0f, -1.0f)
	};

	PSInput output;
	output.position = float4(positions[vertexId], 0.0f, 1.0f);
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	float2 halfSize = max(screenSize * 0.5f, float2(1.0f, 1.0f));
	bool right = input.position.x >= halfSize.x;
	bool bottom = input.position.y >= halfSize.y;
	float2 uv = float2(
		right ? (input.position.x - halfSize.x) / halfSize.x : input.position.x / halfSize.x,
		bottom ? (input.position.y - halfSize.y) / halfSize.y : input.position.y / halfSize.y);
	uv = saturate(uv);

	if (!right && !bottom) {
		return float4(colorTexture.Sample(linearSampler, uv).rgb, 1.0f);
	}
	if (right && !bottom) {
		float depth = depthTexture.Sample(linearSampler, uv).r;
		depth = saturate(depth / max(depth + 1.0f, 0.0001f));
		return float4(depth.xxx, 1.0f);
	}
	if (!right && bottom) {
		float2 mv = motionTexture.Sample(linearSampler, uv).rg;
		return float4(saturate(mv * 0.5f + 0.5f), 0.0f, 1.0f);
	}
	return float4(finalTexture.Sample(linearSampler, uv).rgb, 1.0f);
}
)";
		auto vertexShader = CompileShader(shaderSource, "VSMain", "vs_5_0");
		auto pixelShader = CompileShader(shaderSource, "PSMain", "ps_5_0");

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.InputLayout = {};
		psoDesc.pRootSignature = rootSignature.get();
		psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
		psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = a_backBufferFormat;
		psoDesc.SampleDesc.Count = 1;
		ThrowIfFailed(a_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineState.put())));

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.NumDescriptors = kSRVsPerSlot * currentDescriptorSlotCount;
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(srvHeap.put())));

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.NumDescriptors = currentDescriptorSlotCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(rtvHeap.put())));
	} catch (const std::exception& e) {
		logger::warn("[TaggedTextureDebug] Resource creation failed: {}", e.what());
		rootSignature = nullptr;
		pipelineState = nullptr;
		currentDescriptorSlotCount = 0;
		return false;
	}

	return true;
}

void TaggedTextureDebug::CreateSRV(ID3D12Device* a_device, ID3D12Resource* a_resource, uint32_t a_index)
{
	auto handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<SIZE_T>(a_index) * a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	const auto desc = a_resource->GetDesc();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = SRVFormat(desc.Format);
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	a_device->CreateShaderResourceView(a_resource, &srvDesc, handle);
}

void TaggedTextureDebug::Render(
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
	uint32_t a_descriptorSlotCount)
{
	if (!a_device || !a_commandList || !a_backBuffer || !a_color || !a_depth || !a_motionVectors || !a_finalImage || a_width == 0 || a_height == 0) {
		return;
	}
	if (!EnsureResources(a_device, a_backBufferFormat, a_descriptorSlotCount) || a_descriptorSlot >= currentDescriptorSlotCount) {
		return;
	}

	const auto srvBaseIndex = a_descriptorSlot * kSRVsPerSlot;
	CreateSRV(a_device, a_color, srvBaseIndex);
	CreateSRV(a_device, a_depth, srvBaseIndex + 1);
	CreateSRV(a_device, a_motionVectors, srvBaseIndex + 2);
	CreateSRV(a_device, a_finalImage, srvBaseIndex + 3);

	const auto rtvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(a_descriptorSlot) * rtvIncrement;
	a_device->CreateRenderTargetView(a_backBuffer, nullptr, rtv);
	a_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	ID3D12DescriptorHeap* heaps[] = { srvHeap.get() };
	a_commandList->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
	a_commandList->SetGraphicsRootSignature(rootSignature.get());
	a_commandList->SetPipelineState(pipelineState.get());
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	const auto srvIncrement = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	auto srvTable = srvHeap->GetGPUDescriptorHandleForHeapStart();
	srvTable.ptr += static_cast<UINT64>(srvBaseIndex) * srvIncrement;
	a_commandList->SetGraphicsRootDescriptorTable(0, srvTable);
	const float screenSize[] = { static_cast<float>(a_width), static_cast<float>(a_height) };
	a_commandList->SetGraphicsRoot32BitConstants(1, 2, screenSize, 0);
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(a_width), static_cast<float>(a_height), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height) };
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->DrawInstanced(6, 1, 0, 0);
}
