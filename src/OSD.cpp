#include "XeSS.h"
#include "OSD.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>

#include "DX12SwapChain.h"
#include "FidelityFX.h"
#include "FrameTiming.h"
#include "Streamline.h"
#include "Upscaling.h"

namespace
{
	constexpr uint32_t kCompactTextureWidth = 540;
	constexpr uint32_t kCompactTextureHeight = 32;
	constexpr uint32_t kDetailedTextureWidth = 380;
	constexpr uint32_t kDetailedTextureHeight = 220;
	constexpr auto kSampleInterval = std::chrono::milliseconds(500);

	constexpr char kOSDShader[] = R"(
Texture2D osdTexture : register(t0);
SamplerState osdSampler : register(s0);

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertex : SV_VertexID)
{
	PSInput output;
	output.uv = float2(vertex & 1, vertex >> 1);
	output.position = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	return osdTexture.Sample(osdSampler, input.uv);
}
)";
	constexpr uint32_t kPanelOffset = 12;

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
		if (upscaling->ShouldUseExternalFrameGeneration(true)) {
			return upscaling->GetFGProviderName();
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
		case Upscaling::UpscaleMethod::kXeSS:
			return "XeSS";
		case Upscaling::UpscaleMethod::kSpatialFallback:
			return "Fallback";
		case Upscaling::UpscaleMethod::kDisabled:
		default:
			return "Off";
		}
	}

	Upscaling::UpscaleMethod DisplayedUpscaler(const Upscaling& a_upscaling)
	{
		using Method = Upscaling::UpscaleMethod;
		// ENB uses the fallback state to keep its render domain active even
		// when the scene is already native-sized and needs no scaling pass.
		if (a_upscaling.upscaleMethod == Method::kSpatialFallback &&
			a_upscaling.settings.upscaleMethodPreference == static_cast<uint32_t>(Method::kDisabled) &&
			a_upscaling.osdRenderSize.x == a_upscaling.osdNativeSize.x &&
			a_upscaling.osdRenderSize.y == a_upscaling.osdNativeSize.y) {
			return Method::kDisabled;
		}
		return a_upscaling.upscaleMethod;
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

	struct ShaderPair
	{
		winrt::com_ptr<ID3DBlob> vertex = CompileShader(kOSDShader, "VSMain", "vs_5_0");
		winrt::com_ptr<ID3DBlob> pixel = CompileShader(kOSDShader, "PSMain", "ps_5_0");
	};

	const ShaderPair& GetShaders()
	{
		static const ShaderPair shaders;
		return shaders;
	}

	// Both presenters rasterize on their calling thread. Keep the GDI objects
	// there so a text refresh does not allocate a DC, DIB and font mid-frame.
	struct TextSurface
	{
		HDC dc = CreateCompatibleDC(nullptr);
		HFONT font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
		HBITMAP bitmap = nullptr;
		void* bits = nullptr;
		uint32_t width = 0, height = 0;

		TextSurface()
		{
			if (!dc) return;
			if (font) SelectObject(dc, font);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(230, 245, 230));
		}

		~TextSurface()
		{
			if (dc) DeleteDC(dc);
			if (bitmap) DeleteObject(bitmap);
			if (font) DeleteObject(font);
		}

		bool Resize(uint32_t w, uint32_t h)
		{
			if (width == w && height == h && bitmap) return true;
			if (!dc) return false;
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = static_cast<LONG>(w);
			info.bmiHeader.biHeight = -static_cast<LONG>(h);
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			void* data = nullptr;
			HBITMAP next = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
			if (!next) return false;
			if (!SelectObject(dc, next)) {
				DeleteObject(next);
				return false;
			}
			if (bitmap) DeleteObject(bitmap);
			bitmap = next;
			bits = data;
			width = w;
			height = h;
			return true;
		}
	};

	void DrawTextToBuffer(const std::string& a_text, std::vector<uint32_t>& a_pixels, uint32_t a_width, uint32_t a_height)
	{
		FrameTiming::CpuScope timing("OSD.RasterizeText");
		static thread_local TextSurface surface;
		if (!surface.Resize(a_width, a_height)) {
			std::fill(a_pixels.begin(), a_pixels.end(), 0u);
			return;
		}
		std::memset(surface.bits, 0, a_width * a_height * sizeof(uint32_t));
		RECT textRect{ 8, 6, static_cast<LONG>(a_width - 8), static_cast<LONG>(a_height - 6) };
		DrawTextA(surface.dc, a_text.c_str(), static_cast<int>(a_text.size()), &textRect, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS);
		GdiFlush();

		auto* source = static_cast<const uint32_t*>(surface.bits);
		for (uint32_t i = 0; i < a_width * a_height; ++i) {
			if (source[i] == 0) {
				a_pixels[i] = 120u << 24;  // Translucent black panel background.
				continue;
			}
			const uint8_t b = source[i] & 0xFF;
			const uint8_t g = (source[i] >> 8) & 0xFF;
			const uint8_t r = (source[i] >> 16) & 0xFF;
			const uint8_t alpha = static_cast<uint8_t>(std::min(220u, 120u + static_cast<uint32_t>(std::max({ r, g, b }))));
			a_pixels[i] = (static_cast<uint32_t>(alpha) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
		}
	}
}

void OSD::CreatePipeline12(Pipeline12& a_pipeline)
{
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
	ThrowIfFailed(a_pipeline.device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(a_pipeline.rootSignature.put())));

	const auto& shaders = GetShaders();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.pRootSignature = a_pipeline.rootSignature.get();
	psoDesc.VS = { shaders.vertex->GetBufferPointer(), shaders.vertex->GetBufferSize() };
	psoDesc.PS = { shaders.pixel->GetBufferPointer(), shaders.pixel->GetBufferSize() };
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
	psoDesc.RTVFormats[0] = a_pipeline.format;
	psoDesc.SampleDesc.Count = 1;
	ThrowIfFailed(a_pipeline.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(a_pipeline.pipelineState.put())));
}

bool OSD::EnsurePipeline12(ID3D12Device* a_device, DXGI_FORMAT a_format)
{
	const auto matches = [&] {
		return pipeline12 && pipeline12->device.get() == a_device && pipeline12->format == a_format;
	};
	if (matches()) { return true; }
	try {
		if (pendingPipeline12.valid()) {
			if (pendingPipeline12.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }
			pipeline12 = pendingPipeline12.get();
			if (matches()) { return true; }
		}
		if (std::chrono::steady_clock::now() < retryPipeline12) { return false; }

		auto pipeline = std::make_shared<Pipeline12>();
		pipeline->device.copy_from(a_device);
		pipeline->format = a_format;
		// Pipeline creation includes synchronous driver compilation. The
		// render thread consumes it only after the worker has finished.
		pendingPipeline12 = std::async(std::launch::async, [pipeline = std::move(pipeline)] {
			CreatePipeline12(*pipeline);
			return pipeline;
		});
	} catch (const std::exception& e) {
		retryPipeline12 = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		logger::warn("[OSD] D3D12 pipeline creation failed: {}", e.what());
	}
	return false;
}

bool OSD::EnsureResources(ID3D12Device* a_device, DXGI_FORMAT a_backBufferFormat, uint32_t a_mode)
{
	if (!a_device || a_backBufferFormat == DXGI_FORMAT_UNKNOWN) { return false; }
	const auto width = GetOSDTextureWidth(a_mode);
	const auto height = GetOSDTextureHeight(a_mode);
	auto& cached = modeResources12[a_mode - 1];
	if (cached && cached->pipeline->device.get() == a_device && cached->pipeline->format == a_backBufferFormat) {
		if (cached != resources12) textureDirty = true;
		resources12 = cached;
		return true;
	}
	if (resources12 && resources12->pipeline->device.get() == a_device && resources12->pipeline->format == a_backBufferFormat) {
		pipeline12 = resources12->pipeline;
	} else if (!EnsurePipeline12(a_device, a_backBufferFormat)) {
		return false;
	}

	// Submitted lists retain their own descriptors and texture through
	// the command-context fence. Replacing this bundle needs no queue drain.
	try {
		auto next = std::make_shared<Resources12>();
		next->pipeline = pipeline12;
		next->width = width;
		next->height = height;

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.NumDescriptors = 1;
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(next->srvHeap.put())));

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.NumDescriptors = kDX12FrameCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		ThrowIfFailed(a_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(next->rtvHeap.put())));

		D3D12_RESOURCE_DESC textureDesc{};
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		textureDesc.Width = next->width;
		textureDesc.Height = next->height;
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
			IID_PPV_ARGS(next->texture.put())));

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		a_device->CreateShaderResourceView(next->texture.get(), &srvDesc, next->srvHeap->GetCPUDescriptorHandleForHeapStart());

		uint64_t rowSize = 0;
		uint32_t rowCount = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		a_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rowCount, &rowSize, &next->uploadSize);
		next->uploadRowPitch = footprint.Footprint.RowPitch;
		cached = std::move(next);
	} catch (const std::exception& e) {
		logger::warn("[OSD] D3D12 resource creation failed: {}", e.what());
		return false;
	}

	resources12 = cached;
	textureDirty = true;
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

void OSD::UpdateStats(uint32_t a_mode)
{
	if (cachedMode != a_mode) {
		cachedMode = a_mode;
		cachedText = BuildText();
		textureDirty = true;
	}

	const auto now = std::chrono::steady_clock::now();
	if (lastFrameTime.time_since_epoch().count() != 0) {
		frameTimeAccumMs += std::chrono::duration<double, std::milli>(now - lastFrameTime).count();
		++renderedFrames;
	}
	lastFrameTime = now;
	const auto presentationCount = [] {
		return XeSS::GetSingleton()->OwnsSwapChain() ? XeSS::GetSingleton()->PresentedFrames() : FidelityFX::GetSingleton()->GetGeneratedFrameCount();
	};

	if (sampleStart.time_since_epoch().count() == 0) {
		sampleStart = now;
		lastFSRGeneratedFrameCount = presentationCount();
		return;
	}

	const auto elapsed = now - sampleStart;
	if (elapsed < kSampleInterval) {
		return;
	}

	FrameTiming::CpuScope timing("OSD.SampleStats");
	const auto fsrGeneratedFrameCount = presentationCount();
	auto streamline = Streamline::GetSingleton();
	const auto elapsedSeconds = std::chrono::duration<double>(elapsed).count();
	renderFPS = renderedFrames / elapsedSeconds;
	frameTimeMs = renderedFrames > 0 ? frameTimeAccumMs / renderedFrames : 0.0;
	if (streamline->dlssgActive) {
		generatedFPS = renderFPS * static_cast<double>(streamline->GetDLSSGPresentedFrameMultiplier());
	} else {
		// Fall back to successful SDK dispatches only when DXGI statistics are
		// unavailable. A dispatch alone does not prove the frame was presented.
		const auto interpolated = fsrGeneratedFrameCount >= lastFSRGeneratedFrameCount ? fsrGeneratedFrameCount - lastFSRGeneratedFrameCount : 0;
		generatedFPS = (XeSS::GetSingleton()->OwnsSwapChain() ? interpolated : renderedFrames + interpolated) / elapsedSeconds;
	}
	if (FidelityFX::GetSingleton()->IsFrameGenerationEnabled()) {
		auto* chain = DX12SwapChain::GetSingleton()->swapChain.get();
		UINT count = 0;
		const bool valid = chain && SUCCEEDED(chain->GetLastPresentCount(&count));
		const auto delta = static_cast<UINT>(count - lastFSRPresentCount);
		// FSR's wrapper forwards this to the native swapchain, including BOTH
		// generated and real submissions. It does not measure unique motion or
		// guarantee scan-out (the compositor may still drop a submitted frame).
		if (valid && fsrPresentCountValid && fsrPresentCountChain == chain && delta < 100000) {
			generatedFPS = delta / elapsedSeconds;
		}
		fsrPresentCountChain = chain;
		fsrPresentCountValid = valid;
		lastFSRPresentCount = count;
	} else {
		fsrPresentCountValid = false;
		fsrPresentCountChain = nullptr;
	}
	lastFSRGeneratedFrameCount = fsrGeneratedFrameCount;

	// DXGI reports this process's usage on the rendering adapter. Shared
	// D3D11/D3D12 resources are already accounted for; do not add SDK totals.
	gpuMemoryTotalText = "N/A";
	gpuMemoryDetailText = "N/A";
	if (adapter) {
		DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
		const bool localValid = SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
		const bool nonlocalValid = SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal));
		constexpr double MiB = 1024.0 * 1024.0;
		const auto localText = localValid ? std::format("{:.1f} MiB", local.CurrentUsage / MiB) : "N/A";
		const auto nonlocalText = nonlocalValid ? std::format("{:.1f} MiB", nonlocal.CurrentUsage / MiB) : "N/A";
		if (localValid && nonlocalValid) {
			gpuMemoryTotalText = std::format("{:.1f} MiB", local.CurrentUsage / MiB + nonlocal.CurrentUsage / MiB);
		}
		gpuMemoryDetailText = std::format("{} (local {})\nNon-local: {}", gpuMemoryTotalText, localText, nonlocalText);
	}

	reflexLatencyMs = 0.0f;
	appGPULatencyMs = 0.0f;
	if (streamline->UsesReflexLatencyReport()) {
		reflexLatencyMs = streamline->GetReflexLatencyMs();
	} else {
		DX12SwapChain::GetSingleton()->GetSimulationToAppGPUCompletionMs(appGPULatencyMs);
	}

	const auto newText = BuildText();
	if (newText != cachedText) {
		cachedText = newText;
		textureDirty = true;
	}

	sampleStart = now;
	renderedFrames = 0;
	frameTimeAccumMs = 0.0;
}

std::string OSD::BuildText() const
{
	return GetOSDMode() == 1 ? BuildCompactText() : BuildDetailedText();
}

std::string OSD::BuildCompactText() const
{
	auto upscaling = Upscaling::GetSingleton();
	const auto fgFPS = upscaling->ShouldUseFrameGeneration(true) || upscaling->ShouldUseExternalFrameGeneration(true) ? generatedFPS : 0.0;
	char latency[32]{};
	const bool reflexOwner = Streamline::GetSingleton()->UsesReflexLatencyReport();
	const auto latencyMs = reflexOwner ? reflexLatencyMs : appGPULatencyMs;
	if (latencyMs > 0.0f) {
		std::snprintf(latency, sizeof(latency), "%.1fms", latencyMs);
	} else {
		std::snprintf(latency, sizeof(latency), "N/A");
	}

	auto memoryText = gpuMemoryTotalText;
	if (memoryText.ends_with(" MiB")) {
		memoryText.replace(memoryText.size() - 4, 4, "MB");
	}
	char line[192]{};
	std::snprintf(
		line,
		sizeof(line),
		"%s %.1f\t%s %.1f\tLatency %s\tVRAM %s",
		UpscalerMethodName(DisplayedUpscaler(*upscaling)),
		renderFPS,
		FGMethodName(),
		fgFPS,
		latency,
		memoryText.c_str());
	return line;
}

std::string OSD::BuildDetailedText() const
{
	auto upscaling = Upscaling::GetSingleton();
	auto streamline = Streamline::GetSingleton();
	const auto activeMethod = DisplayedUpscaler(*upscaling);
	const auto jitter = upscaling->jitter;

	std::string text;
	char line[160]{};
	std::snprintf(line, sizeof(line), "FPS: %.1f  %.2f ms\n", renderFPS, frameTimeMs);
	text += line;
	std::snprintf(line, sizeof(line), "Upscaler: %s\n", UpscalerMethodName(activeMethod));
	text += line;
	std::snprintf(line, sizeof(line), "FG: %s\n", FGMethodName());
	text += line;
	if (upscaling->ShouldUseFrameGeneration(true) || upscaling->ShouldUseExternalFrameGeneration(true)) {
		std::snprintf(line, sizeof(line), "Generated FPS: %.1f\n", generatedFPS);
		text += line;
	}
	text += "GPU Mem: " + gpuMemoryDetailText + "\n";
	const bool reflexOwner = streamline->UsesReflexLatencyReport();
	const auto latencyMs = reflexOwner ? reflexLatencyMs : appGPULatencyMs;
	if (latencyMs > 0.0f) {
		std::snprintf(line, sizeof(line), "PC Latency: %.2f ms\n", latencyMs);
	} else if (reflexOwner && !streamline->IsPCLLatencyReportAvailable()) {
		std::snprintf(line, sizeof(line), "PC Latency: no report\n");
	} else {
		std::snprintf(line, sizeof(line), "PC Latency: timing N/A\n");
	}
	text += line;

	if (activeMethod == Upscaling::UpscaleMethod::kDLSS) {
		std::snprintf(line, sizeof(line), "Res: %.0fx%.0f -> %.0fx%.0f\n", upscaling->osdRenderSize.x, upscaling->osdRenderSize.y, upscaling->osdNativeSize.x, upscaling->osdNativeSize.y);
		text += line;
		std::snprintf(line, sizeof(line), "Reflex: %s\n", ReflexModeName(streamline->GetCurrentReflexMode()));
		text += line;
		std::snprintf(line, sizeof(line), "DLSS Quality: %s\n", QualityName(streamline->GetCurrentDLSSQualityMode()));
		text += line;
		std::snprintf(line, sizeof(line), "DLSS Preset: %s\n", DLSSModelPresetName(streamline->GetCurrentDLSSModelPreset()));
		text += line;
	} else if (Upscaling::IsSharedTemporalSR(activeMethod)) {
		std::snprintf(line, sizeof(line), "Res: %.0fx%.0f -> %.0fx%.0f\n", upscaling->osdRenderSize.x, upscaling->osdRenderSize.y, upscaling->osdNativeSize.x, upscaling->osdNativeSize.y);
		text += line;
		std::snprintf(line, sizeof(line), "%s Quality: %s\n", activeMethod == Upscaling::UpscaleMethod::kXeSS ? "XeSS" : "FSR", QualityName(upscaling->settings.qualityMode));
		text += line;
	} else if (activeMethod == Upscaling::UpscaleMethod::kSpatialFallback) {
		std::snprintf(line, sizeof(line), "Res: %.0fx%.0f -> %.0fx%.0f\n", upscaling->osdRenderSize.x, upscaling->osdRenderSize.y, upscaling->osdNativeSize.x, upscaling->osdNativeSize.y);
		text += line;
		std::snprintf(line, sizeof(line), "Fallback: Spatial\n");
		text += line;
	}

	std::snprintf(line, sizeof(line), "Jitter: %.4f, %.4f\n", jitter.x, jitter.y);
	text += line;
	return text;
}

bool OSD::UpdateTexture(ID3D12GraphicsCommandList* a_commandList, winrt::com_ptr<ID3D12Resource>& a_upload)
{
	const auto& resources = *resources12;
	if (!textureDirty) {
		return true;
	}
	// The caller owns this allocation in a completed command-context slot.
	// Keep it cached there; updating text never waits for another slot's copy.
	if (!a_upload || a_upload->GetDesc().Width < resources.uploadSize) {
		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		const auto desc = CD3DX12_RESOURCE_DESC::Buffer(resources.uploadSize);
		winrt::com_ptr<ID3D12Resource> replacement;
		if (FAILED(resources.pipeline->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(replacement.put())))) {
			return false;
		}
		a_upload = std::move(replacement);
	}

	pixels.resize(resources.width * resources.height);
	DrawTextToBuffer(cachedText, pixels, resources.width, resources.height);

	uint8_t* mapped = nullptr;
	if (FAILED(a_upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
		return false;
	}
	for (uint32_t y = 0; y < resources.height; ++y) {
		std::memcpy(mapped + y * resources.uploadRowPitch, pixels.data() + y * resources.width, resources.width * sizeof(uint32_t));
	}
	a_upload->Unmap(0, nullptr);

	auto before = CD3DX12_RESOURCE_BARRIER::Transition(resources.texture.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	a_commandList->ResourceBarrier(1, &before);

	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = resources.texture.get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = a_upload.get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint.Offset = 0;
	source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	source.PlacedFootprint.Footprint.Width = resources.width;
	source.PlacedFootprint.Footprint.Height = resources.height;
	source.PlacedFootprint.Footprint.Depth = 1;
	source.PlacedFootprint.Footprint.RowPitch = resources.uploadRowPitch;

	a_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

	auto after = CD3DX12_RESOURCE_BARRIER::Transition(resources.texture.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	a_commandList->ResourceBarrier(1, &after);
	textureDirty = false;
	return true;
}

void OSD::Draw(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_backBuffer,
	uint32_t a_backBufferIndex, uint32_t a_width, uint32_t a_height)
{
	const auto& resources = *resources12;
	const auto increment = resources.pipeline->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto rtv = resources.rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(a_backBufferIndex) * increment;
	resources.pipeline->device->CreateRenderTargetView(a_backBuffer, nullptr, rtv);

	const D3D12_VIEWPORT viewport{ kPanelOffset, kPanelOffset,
		static_cast<float>(resources.width), static_cast<float>(resources.height), 0.0f, 1.0f };
	const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(std::min(a_width, kPanelOffset + resources.width)),
		static_cast<LONG>(std::min(a_height, kPanelOffset + resources.height)) };
	ID3D12DescriptorHeap* heaps[] = { resources.srvHeap.get() };
	a_commandList->SetDescriptorHeaps(1, heaps);
	a_commandList->SetGraphicsRootSignature(resources.pipeline->rootSignature.get());
	a_commandList->SetPipelineState(resources.pipeline->pipelineState.get());
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	a_commandList->SetGraphicsRootDescriptorTable(0, resources.srvHeap->GetGPUDescriptorHandleForHeapStart());
	a_commandList->DrawInstanced(4, 1, 0, 0);
}

void OSD::Render(
	ID3D12Device* a_device,
	ID3D12GraphicsCommandList* a_commandList,
	Frame12& a_frame,
	ID3D12Resource* a_backBuffer,
	uint32_t a_backBufferIndex,
	DXGI_FORMAT a_backBufferFormat,
	uint32_t a_width,
	uint32_t a_height)
{
	const auto mode = GetOSDMode();
	if (!mode || !a_commandList || !a_backBuffer || a_backBufferIndex >= kDX12FrameCount || !a_width || !a_height) { return; }
	FrameTiming::CpuScope cpu("OSD.Render");
	FrameTiming::Gpu12Scope gpu(a_commandList, "OSD.Render");
	if (!EnsureResources(a_device, a_backBufferFormat, mode)) { return; }
	a_frame.resources = resources12;
	EnsureAdapter(a_device);
	UpdateStats(mode);
	if (!UpdateTexture(a_commandList, a_frame.upload)) { return; }
	Draw(a_commandList, a_backBuffer, a_backBufferIndex, a_width, a_height);
}

bool OSD::EnsureNativeResources(ID3D11Device* a_device, uint32_t a_mode)
{
	const auto width = GetOSDTextureWidth(a_mode);
	const auto height = GetOSDTextureHeight(a_mode);
	try {
		if (resources11.device.get() != a_device) {
			Resources11 next;
			next.device.copy_from(a_device);
			auto device1 = next.device.as<ID3D11Device1>();
			const auto level = a_device->GetFeatureLevel();
			ThrowIfFailed(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
				__uuidof(ID3D11Device), nullptr, next.contextState.put()));
			const auto& shaders = GetShaders();
			ThrowIfFailed(a_device->CreateVertexShader(shaders.vertex->GetBufferPointer(), shaders.vertex->GetBufferSize(), nullptr, next.vertexShader.put()));
			ThrowIfFailed(a_device->CreatePixelShader(shaders.pixel->GetBufferPointer(), shaders.pixel->GetBufferSize(), nullptr, next.pixelShader.put()));

			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0] = { TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
				D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
			ThrowIfFailed(a_device->CreateBlendState(&blend, next.blend.put()));
			D3D11_SAMPLER_DESC sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			ThrowIfFailed(a_device->CreateSamplerState(&sampler, next.sampler.put()));
			resources11 = std::move(next);
			winrt::com_ptr<IDXGIDevice> dxgi;
			winrt::com_ptr<IDXGIAdapter> base;
			adapter = nullptr;
			if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(dxgi.put()))) &&
				SUCCEEDED(dxgi->GetAdapter(base.put()))) {
				std::ignore = base->QueryInterface(IID_PPV_ARGS(adapter.put()));
			}
		}
		auto& panel = resources11.panels[a_mode - 1];
		if (panel.texture) { return true; }

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		ThrowIfFailed(a_device->CreateTexture2D(&desc, nullptr, texture.put()));
		ThrowIfFailed(a_device->CreateShaderResourceView(texture.get(), nullptr, view.put()));
		panel.texture = std::move(texture);
		panel.view = std::move(view);
		textureDirty = true;
		return true;
	} catch (const std::exception& e) {
		logger::warn("[OSD] D3D11 resource creation failed: {}", e.what());
		return false;
	}
}

void OSD::RenderNative(IDXGISwapChain* a_swapChain)
{
	if (!GetOSDMode()) { return; }
	winrt::com_ptr<ID3D11Texture2D> buffer;
	if (FAILED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(buffer.put())))) { return; }
	winrt::com_ptr<ID3D11Device> device;
	buffer->GetDevice(device.put());
	auto device1 = device.try_as<ID3D11Device1>();
	if (!device1) { return; }
	winrt::com_ptr<ID3D11DeviceContext1> context;
	device1->GetImmediateContext1(context.put());
	RenderNative(device.get(), context.get(), buffer.get());
}

void OSD::RenderNative(ID3D11Device* a_device, ID3D11DeviceContext1* a_context, ID3D11Texture2D* a_backBuffer)
{
	const auto mode = GetOSDMode();
	if (!mode || !a_device || !a_context || !a_backBuffer) { return; }
	FrameTiming::CpuScope cpu("OSD.Render");
	FrameTiming::Gpu11Scope gpu("OSD.Render");
	if (!EnsureNativeResources(a_device, mode)) { return; }
	UpdateStats(mode);
	const auto& resources = resources11;
	const auto& panel = resources.panels[mode - 1];
	const auto width = GetOSDTextureWidth(mode);
	const auto height = GetOSDTextureHeight(mode);
	if (textureDirty) {
		pixels.resize(width * height);
		DrawTextToBuffer(cachedText, pixels, width, height);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(panel.texture.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { return; }
		for (uint32_t y = 0; y < height; ++y) {
			std::memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
				pixels.data() + y * width, width * sizeof(uint32_t));
		}
		a_context->Unmap(panel.texture.get(), 0);
		textureDirty = false;
	}

	// The physical backbuffer and these bindings belong only to this draw.
	// Unbind the RTV before storing the isolated state so ResizeBuffers can
	// release the physical backbuffer.
	winrt::com_ptr<ID3D11RenderTargetView> rtv;
	if (FAILED(a_device->CreateRenderTargetView(a_backBuffer, nullptr, rtv.put()))) { return; }
	winrt::com_ptr<ID3DDeviceContextState> saved;
	a_context->SwapDeviceContextState(resources.contextState.get(), saved.put());
	const D3D11_VIEWPORT viewport{ kPanelOffset, kPanelOffset,
		static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
	auto* target = rtv.get();
	auto* view = panel.view.get();
	auto* sampler = resources.sampler.get();
	a_context->OMSetRenderTargets(1, &target, nullptr);
	a_context->OMSetBlendState(resources.blend.get(), nullptr, UINT_MAX);
	a_context->RSSetViewports(1, &viewport);
	a_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	a_context->VSSetShader(resources.vertexShader.get(), nullptr, 0);
	a_context->PSSetShader(resources.pixelShader.get(), nullptr, 0);
	a_context->PSSetShaderResources(0, 1, &view);
	a_context->PSSetSamplers(0, 1, &sampler);
	a_context->Draw(4, 0);
	a_context->OMSetRenderTargets(0, nullptr, nullptr);
	a_context->SwapDeviceContextState(saved.get(), nullptr);
}

void OSD::Reset()
{
	fsrPresentCountValid = false;
	fsrPresentCountChain = nullptr;
	lastFrameTime = {};
	sampleStart = {};
	frameTimeAccumMs = 0.0;
	renderedFrames = 0;
	lastFSRGeneratedFrameCount = 0;
	renderFPS = 0.0;
	frameTimeMs = 0.0;
	generatedFPS = 0.0;
	reflexLatencyMs = 0.0f;
	appGPULatencyMs = 0.0f;
	cachedText.clear();
	cachedMode = 0;
	textureDirty = true;
}
