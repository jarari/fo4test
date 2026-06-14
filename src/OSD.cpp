#include "OSD.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>

#include "FidelityFX.h"
#include "Streamline.h"
#include "Upscaling.h"

namespace
{
	constexpr uint32_t kCompactTextureWidth = 540;
	constexpr uint32_t kCompactTextureHeight = 32;
	constexpr uint32_t kDetailedTextureWidth = 380;
	constexpr uint32_t kDetailedTextureHeight = 220;
	constexpr auto kSampleInterval = std::chrono::milliseconds(500);

	uint32_t GetOSDMode()
	{
		return std::clamp<uint32_t>(Upscaling::GetSingleton()->settings.osdMode, 0, 2);
	}

	uint32_t GetOSDTextureWidth(uint32_t a_mode)
	{
		return a_mode == 1 ? kCompactTextureWidth : kDetailedTextureWidth;
	}

	uint32_t GetOSDTextureHeight(uint32_t a_mode)
	{
		return a_mode == 1 ? kCompactTextureHeight : kDetailedTextureHeight;
	}

	const char* QualityName(uint a_qualityMode)
	{
		switch (a_qualityMode) {
		case 0:
			return "Native AA";
		case 1:
			return "Quality";
		case 2:
			return "Balanced";
		case 3:
			return "Performance";
		case 4:
			return "Ultra Performance";
		default:
			return "Unknown";
		}
	}

	const char* DLSSModelPresetName(uint a_preset)
	{
		switch (a_preset) {
		case 1:
			return "Default";
		case 2:
			return "K";
		case 3:
			return "M";
		case 4:
			return "L";
		case 0:
		default:
			return "Recommended";
		}
	}

	const char* ReflexModeName(sl::ReflexMode a_mode)
	{
		switch (a_mode) {
		case sl::ReflexMode::eOff:
			return "Off";
		case sl::ReflexMode::eLowLatency:
			return "On";
		case sl::ReflexMode::eLowLatencyWithBoost:
			return "On + Boost";
		default:
			return "Unavailable";
		}
	}

	const char* FGMethodName()
	{
		auto upscaling = Upscaling::GetSingleton();
		if (upscaling->ShouldUseFrameGeneration(true)) {
			return "DLSS-G";
		}
		if (upscaling->ShouldUseFSRFrameGeneration(true)) {
			return "FSR FG";
		}
		return "Off";
	}

	const char* UpscalerMethodName(Upscaling::UpscaleMethod a_method)
	{
		switch (a_method) {
		case Upscaling::UpscaleMethod::kDLSS:
			return "DLSS";
		case Upscaling::UpscaleMethod::kFSR:
			return "FSR";
		case Upscaling::UpscaleMethod::kDisabled:
		default:
			return "Off";
		}
	}

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
			logger::warn("[OSD] Shader compile failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
		}
		ThrowIfFailed(result);
		return shader;
	}

	void DrawTextToBuffer(const std::string& a_text, std::vector<uint32_t>& a_pixels, uint32_t a_width, uint32_t a_height)
	{
		std::fill(a_pixels.begin(), a_pixels.end(), 0u);

		BITMAPINFO bitmapInfo{};
		bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(a_width);
		bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(a_height);
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biBitCount = 32;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;

		void* bits = nullptr;
		HDC screenDC = GetDC(nullptr);
		HDC memoryDC = CreateCompatibleDC(screenDC);
		HBITMAP bitmap = CreateDIBSection(screenDC, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
		ReleaseDC(nullptr, screenDC);
		if (!memoryDC || !bitmap || !bits) {
			if (bitmap) {
				DeleteObject(bitmap);
			}
			if (memoryDC) {
				DeleteDC(memoryDC);
			}
			return;
		}

		auto oldBitmap = SelectObject(memoryDC, bitmap);
		std::memset(bits, 0, a_width * a_height * sizeof(uint32_t));
		SetBkMode(memoryDC, TRANSPARENT);
		SetTextColor(memoryDC, RGB(230, 245, 230));
		HFONT font = CreateFontA(
			16,
			0,
			0,
			0,
			FW_NORMAL,
			FALSE,
			FALSE,
			FALSE,
			ANSI_CHARSET,
			OUT_DEFAULT_PRECIS,
			CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY,
			FIXED_PITCH | FF_MODERN,
			"Consolas");
		auto oldFont = font ? SelectObject(memoryDC, font) : nullptr;

		RECT background{ 0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height) };
		HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
		FillRect(memoryDC, &background, brush);
		DeleteObject(brush);

		RECT textRect{ 8, 6, static_cast<LONG>(a_width - 8), static_cast<LONG>(a_height - 6) };
		DrawTextA(memoryDC, a_text.c_str(), static_cast<int>(a_text.size()), &textRect, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS);

		auto* source = static_cast<uint32_t*>(bits);
		for (uint32_t i = 0; i < a_width * a_height; ++i) {
			const uint8_t b = source[i] & 0xFF;
			const uint8_t g = (source[i] >> 8) & 0xFF;
			const uint8_t r = (source[i] >> 16) & 0xFF;
			const uint8_t alpha = static_cast<uint8_t>(std::min(220u, 120u + static_cast<uint32_t>(std::max({ r, g, b }))));
			a_pixels[i] = (static_cast<uint32_t>(alpha) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
		}

		if (oldFont) {
			SelectObject(memoryDC, oldFont);
		}
		if (font) {
			DeleteObject(font);
		}
		SelectObject(memoryDC, oldBitmap);
		DeleteObject(bitmap);
		DeleteDC(memoryDC);
	}
}

bool OSD::EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_width, uint32_t a_height)
{
	if (!a_device || a_backBufferFormat == DXGI_FORMAT_UNKNOWN || a_width == 0 || a_height == 0) {
		return false;
	}

	const auto mode = GetOSDMode();
	const auto targetTextureWidth = GetOSDTextureWidth(mode);
	const auto targetTextureHeight = GetOSDTextureHeight(mode);
	if (device.get() == a_device &&
		pipelineState &&
		currentBackBufferFormat == a_backBufferFormat &&
		currentWidth == a_width &&
		currentHeight == a_height &&
		textureWidth == targetTextureWidth &&
		textureHeight == targetTextureHeight) {
		return true;
	}

	device.copy_from(a_device);
	currentBackBufferFormat = a_backBufferFormat;
	currentWidth = a_width;
	currentHeight = a_height;
	textureWidth = targetTextureWidth;
	textureHeight = targetTextureHeight;
	rtvBackBuffers = {};
	textureDirty = true;

	try {
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		range.BaseShaderRegister = 0;
		range.RegisterSpace = 0;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParameter{};
		rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameter.DescriptorTable.NumDescriptorRanges = 1;
		rootParameter.DescriptorTable.pDescriptorRanges = &range;
		rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.ShaderRegister = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 1;
		rootDesc.pParameters = &rootParameter;
		rootDesc.NumStaticSamplers = 1;
		rootDesc.pStaticSamplers = &sampler;
		rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		winrt::com_ptr<ID3DBlob> rootBlob;
		winrt::com_ptr<ID3DBlob> rootError;
		ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.put(), rootError.put()));
		ThrowIfFailed(a_device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(rootSignature.put())));

		const char* shaderSource = R"(
Texture2D osdTexture : register(t0);
SamplerState osdSampler : register(s0);

struct VSInput
{
	float2 position : POSITION;
	float2 uv : TEXCOORD0;
};

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
	PSInput output;
	output.position = float4(input.position, 0.0f, 1.0f);
	output.uv = input.uv;
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	return osdTexture.Sample(osdSampler, input.uv);
}
)";
		auto vertexShader = CompileShader(shaderSource, "VSMain", "vs_5_0");
		auto pixelShader = CompileShader(shaderSource, "PSMain", "ps_5_0");

		D3D12_INPUT_ELEMENT_DESC inputElements[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.InputLayout = { inputElements, static_cast<UINT>(std::size(inputElements)) };
		psoDesc.pRootSignature = rootSignature.get();
		psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
		psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		auto& blend = psoDesc.BlendState.RenderTarget[0];
		blend.BlendEnable = TRUE;
		blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOp = D3D12_BLEND_OP_ADD;
		blend.SrcBlendAlpha = D3D12_BLEND_ONE;
		blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = a_backBufferFormat;
		psoDesc.SampleDesc.Count = 1;
		ThrowIfFailed(a_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineState.put())));

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.NumDescriptors = 1;
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(srvHeap.put())));

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.NumDescriptors = kDX12FrameCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(rtvHeap.put())));

		D3D12_RESOURCE_DESC textureDesc{};
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		textureDesc.Width = textureWidth;
		textureDesc.Height = textureHeight;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		auto heapDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		ThrowIfFailed(a_device->CreateCommittedResource(
			&heapDefault,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(texture.put())));

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		a_device->CreateShaderResourceView(texture.get(), &srvDesc, srvHeap->GetCPUDescriptorHandleForHeapStart());

		uint64_t rowSize = 0;
		uint32_t rowCount = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		a_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rowCount, &rowSize, &uploadSize);
		uploadRowPitch = footprint.Footprint.RowPitch;
		auto heapUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
		ThrowIfFailed(a_device->CreateCommittedResource(
			&heapUpload,
			D3D12_HEAP_FLAG_NONE,
			&uploadDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(textureUpload.put())));

		const float left = 12.0f;
		const float top = 12.0f;
		const float right = left + static_cast<float>(textureWidth);
		const float bottom = top + static_cast<float>(textureHeight);
		const auto ndcLeft = (left / static_cast<float>(a_width)) * 2.0f - 1.0f;
		const auto ndcRight = (right / static_cast<float>(a_width)) * 2.0f - 1.0f;
		const auto ndcTop = 1.0f - (top / static_cast<float>(a_height)) * 2.0f;
		const auto ndcBottom = 1.0f - (bottom / static_cast<float>(a_height)) * 2.0f;
		Vertex vertices[] = {
			{ { ndcLeft, ndcTop }, { 0.0f, 0.0f } },
			{ { ndcRight, ndcTop }, { 1.0f, 0.0f } },
			{ { ndcLeft, ndcBottom }, { 0.0f, 1.0f } },
			{ { ndcLeft, ndcBottom }, { 0.0f, 1.0f } },
			{ { ndcRight, ndcTop }, { 1.0f, 0.0f } },
			{ { ndcRight, ndcBottom }, { 1.0f, 1.0f } }
		};

		auto vertexDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(vertices));
		ThrowIfFailed(a_device->CreateCommittedResource(
			&heapUpload,
			D3D12_HEAP_FLAG_NONE,
			&vertexDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(vertexBuffer.put())));
		void* mapped = nullptr;
		ThrowIfFailed(vertexBuffer->Map(0, nullptr, &mapped));
		std::memcpy(mapped, vertices, sizeof(vertices));
		vertexBuffer->Unmap(0, nullptr);

		vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
		vertexBufferView.SizeInBytes = sizeof(vertices);
		vertexBufferView.StrideInBytes = sizeof(Vertex);
	} catch (const std::exception& e) {
		logger::warn("[OSD] Resource creation failed: {}", e.what());
		rootSignature = nullptr;
		pipelineState = nullptr;
		return false;
	}

	return true;
}

void OSD::EnsureAdapter(ID3D12Device* a_device)
{
	if (adapter || !a_device) {
		return;
	}

	LUID luid = a_device->GetAdapterLuid();
	winrt::com_ptr<IDXGIFactory6> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
		return;
	}

	winrt::com_ptr<IDXGIAdapter> baseAdapter;
	if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(baseAdapter.put())))) {
		return;
	}

	std::ignore = baseAdapter->QueryInterface(IID_PPV_ARGS(adapter.put()));
}

void OSD::UpdateStats()
{
	const auto now = std::chrono::steady_clock::now();
	if (lastFrameTime.time_since_epoch().count() != 0) {
		frameTimeAccumMs += std::chrono::duration<double, std::milli>(now - lastFrameTime).count();
	}
	lastFrameTime = now;
	++renderedFrames;

	auto streamline = Streamline::GetSingleton();
	if (!streamline->dlssgActive && Upscaling::GetSingleton()->ShouldUseFSRFrameGeneration(true)) {
		generatedFrames += 1;
	}

	if (sampleStart.time_since_epoch().count() == 0) {
		sampleStart = now;
		cachedText = BuildText();
		textureDirty = true;
		return;
	}

	const auto elapsed = now - sampleStart;
	if (elapsed < kSampleInterval) {
		return;
	}

	const auto elapsedSeconds = std::chrono::duration<double>(elapsed).count();
	renderFPS = renderedFrames / elapsedSeconds;
	frameTimeMs = renderedFrames > 0 ? frameTimeAccumMs / renderedFrames : 0.0;
	if (streamline->dlssgActive) {
		generatedFPS = renderFPS * static_cast<double>(streamline->GetDLSSGPresentedFrameMultiplier());
	} else {
		generatedFPS = (renderedFrames + generatedFrames) / elapsedSeconds;
	}

	if (adapter) {
		DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo{};
		if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo))) {
			vramUsageMB = memoryInfo.CurrentUsage / (1024ull * 1024ull);
		}
	}

	reflexLatencyMs = streamline->GetReflexLatencyMs();

	const auto newText = BuildText();
	if (newText != cachedText) {
		cachedText = newText;
		textureDirty = true;
	}

	sampleStart = now;
	renderedFrames = 0;
	generatedFrames = 0;
	frameTimeAccumMs = 0.0;
}

std::string OSD::BuildText() const
{
	return GetOSDMode() == 1 ? BuildCompactText() : BuildDetailedText();
}

std::string OSD::BuildCompactText() const
{
	auto upscaling = Upscaling::GetSingleton();
	const auto fgFPS = upscaling->ShouldUseFrameGeneration(true) || upscaling->ShouldUseFSRFrameGeneration(true) ? generatedFPS : 0.0;
	const auto latency = reflexLatencyMs > 0.0f ? reflexLatencyMs : 0.0f;

	char line[192]{};
	std::snprintf(
		line,
		sizeof(line),
		"%s %.1f\t%s %.1f\tLatency %.1fms\tVRAM %.1fMB",
		UpscalerMethodName(upscaling->upscaleMethod),
		renderFPS,
		FGMethodName(),
		fgFPS,
		latency,
		static_cast<double>(vramUsageMB));
	return line;
}

std::string OSD::BuildDetailedText() const
{
	auto upscaling = Upscaling::GetSingleton();
	auto streamline = Streamline::GetSingleton();
	const auto activeMethod = upscaling->upscaleMethod;
	const auto jitter = upscaling->jitter;

	std::string text;
	char line[160]{};
	std::snprintf(line, sizeof(line), "FPS: %.1f  %.2f ms\n", renderFPS, frameTimeMs);
	text += line;
	std::snprintf(line, sizeof(line), "Upscaler: %s\n", UpscalerMethodName(activeMethod));
	text += line;
	std::snprintf(line, sizeof(line), "FG: %s\n", FGMethodName());
	text += line;
	if (upscaling->ShouldUseFrameGeneration(true) || upscaling->ShouldUseFSRFrameGeneration(true)) {
		std::snprintf(line, sizeof(line), "Generated FPS: %.1f\n", generatedFPS);
		text += line;
	}
	std::snprintf(line, sizeof(line), "VRAM: %llu MB\n", static_cast<unsigned long long>(vramUsageMB));
	text += line;

	if (activeMethod == Upscaling::UpscaleMethod::kDLSS) {
		std::snprintf(line, sizeof(line), "Res: %.0fx%.0f -> %.0fx%.0f\n", upscaling->osdRenderSize.x, upscaling->osdRenderSize.y, upscaling->osdNativeSize.x, upscaling->osdNativeSize.y);
		text += line;
		std::snprintf(line, sizeof(line), "Reflex: %s\n", ReflexModeName(streamline->GetCurrentReflexMode()));
		text += line;
		if (streamline->GetCurrentReflexMode() != sl::ReflexMode::ReflexMode_eCount) {
			if (reflexLatencyMs > 0.0f) {
				std::snprintf(line, sizeof(line), "PC Latency: %.2f ms\n", reflexLatencyMs);
			} else if (!streamline->IsPCLLatencyReportAvailable()) {
				std::snprintf(line, sizeof(line), "PC Latency: no report\n");
			} else {
				std::snprintf(line, sizeof(line), "PC Latency: input N/A\n");
			}
			text += line;
		}
		std::snprintf(line, sizeof(line), "DLSS Quality: %s\n", QualityName(streamline->GetCurrentDLSSQualityMode()));
		text += line;
		std::snprintf(line, sizeof(line), "DLSS Preset: %s\n", DLSSModelPresetName(streamline->GetCurrentDLSSModelPreset()));
		text += line;
	} else if (activeMethod == Upscaling::UpscaleMethod::kFSR) {
		std::snprintf(line, sizeof(line), "Res: %.0fx%.0f -> %.0fx%.0f\n", upscaling->osdRenderSize.x, upscaling->osdRenderSize.y, upscaling->osdNativeSize.x, upscaling->osdNativeSize.y);
		text += line;
		std::snprintf(line, sizeof(line), "FSR Quality: %s\n", QualityName(upscaling->settings.qualityMode));
		text += line;
	}

	std::snprintf(line, sizeof(line), "Jitter: %.4f, %.4f\n", jitter.x, jitter.y);
	text += line;
	return text;
}

void OSD::UpdateTexture(ID3D12GraphicsCommandList* a_commandList)
{
	if (!textureDirty || !texture || !textureUpload || !a_commandList) {
		return;
	}

	std::vector<uint32_t> pixels(textureWidth * textureHeight);
	DrawTextToBuffer(cachedText, pixels, textureWidth, textureHeight);

	uint8_t* mapped = nullptr;
	if (FAILED(textureUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
		return;
	}
	for (uint32_t y = 0; y < textureHeight; ++y) {
		std::memcpy(mapped + y * uploadRowPitch, pixels.data() + y * textureWidth, textureWidth * sizeof(uint32_t));
	}
	textureUpload->Unmap(0, nullptr);

	auto before = CD3DX12_RESOURCE_BARRIER::Transition(texture.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	a_commandList->ResourceBarrier(1, &before);

	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = texture.get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = textureUpload.get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint.Offset = 0;
	source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	source.PlacedFootprint.Footprint.Width = textureWidth;
	source.PlacedFootprint.Footprint.Height = textureHeight;
	source.PlacedFootprint.Footprint.Depth = 1;
	source.PlacedFootprint.Footprint.RowPitch = uploadRowPitch;

	a_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

	auto after = CD3DX12_RESOURCE_BARRIER::Transition(texture.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	a_commandList->ResourceBarrier(1, &after);
	textureDirty = false;
}

void OSD::Draw(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_backBuffer, uint32_t a_backBufferIndex)
{
	if (!a_commandList || !a_backBuffer || !pipelineState || !rtvHeap || a_backBufferIndex >= rtvBackBuffers.size()) {
		return;
	}

	const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtvHandle.ptr += static_cast<SIZE_T>(a_backBufferIndex) * increment;
	if (rtvBackBuffers[a_backBufferIndex] != a_backBuffer) {
		device->CreateRenderTargetView(a_backBuffer, nullptr, rtvHandle);
		rtvBackBuffers[a_backBufferIndex] = a_backBuffer;
	}

	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(currentWidth), static_cast<float>(currentHeight), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(currentWidth), static_cast<LONG>(currentHeight) };
	ID3D12DescriptorHeap* heaps[] = { srvHeap.get() };
	a_commandList->SetDescriptorHeaps(1, heaps);
	a_commandList->SetGraphicsRootSignature(rootSignature.get());
	a_commandList->SetPipelineState(pipelineState.get());
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	a_commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
	a_commandList->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
	a_commandList->DrawInstanced(6, 1, 0, 0);
}

void OSD::Render(
	ID3D12Device* a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource* a_backBuffer,
	uint32_t a_backBufferIndex,
	DXGI_FORMAT a_backBufferFormat,
	uint32_t a_width,
	uint32_t a_height)
{
	const auto mode = GetOSDMode();
	if (mode == 0) {
		return;
	}

	const auto activeMethod = Upscaling::GetSingleton()->upscaleMethod;
	if (activeMethod != Upscaling::UpscaleMethod::kDLSS && activeMethod != Upscaling::UpscaleMethod::kFSR) {
		return;
	}

	if (!EnsureResources(a_device, a_backBufferFormat, a_width, a_height)) {
		return;
	}

	EnsureAdapter(a_device);
	UpdateStats();
	if (cachedMode != mode) {
		cachedMode = mode;
		cachedText = BuildText();
		textureDirty = true;
	}
	UpdateTexture(a_commandList);
	Draw(a_commandList, a_backBuffer, a_backBufferIndex);
}

void OSD::Reset()
{
	lastFrameTime = {};
	sampleStart = {};
	frameTimeAccumMs = 0.0;
	renderedFrames = 0;
	generatedFrames = 0;
	renderFPS = 0.0;
	frameTimeMs = 0.0;
	generatedFPS = 0.0;
	reflexLatencyMs = 0.0f;
	cachedText.clear();
	cachedMode = 0;
	textureDirty = true;
}
