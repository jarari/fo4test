#include "FidelityFX.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <dx12/ffx_api_dx12.hpp>

#include "DX12SwapChain.h"
#include "Util.h"

namespace
{
	bool LoadFidelityFXRuntime()
	{
		static bool loaded = false;
		static bool attempted = false;
		if (attempted) {
			return loaded;
		}

		attempted = true;

		constexpr auto pluginDirectory = L"Data\\F4SE\\Plugins\\";
		const auto loaderPath = std::wstring(pluginDirectory) + L"amd_fidelityfx_loader_dx12.dll";
		const auto upscalerPath = std::wstring(pluginDirectory) + L"amd_fidelityfx_upscaler_dx12.dll";
		const auto frameGenerationPath = std::wstring(pluginDirectory) + L"amd_fidelityfx_framegeneration_dx12.dll";

		auto upscaler = LoadLibraryW(upscalerPath.c_str());
		if (!upscaler) {
			logger::warn("[FidelityFX] Failed to load {}: error {}", std::filesystem::path(upscalerPath).string(), GetLastError());
		}

		auto frameGeneration = LoadLibraryW(frameGenerationPath.c_str());
		if (!frameGeneration) {
			logger::warn("[FidelityFX] Failed to load {}: error {}", std::filesystem::path(frameGenerationPath).string(), GetLastError());
		}

		auto loader = LoadLibraryW(loaderPath.c_str());
		if (!loader) {
			logger::error("[FidelityFX] Failed to load {}: error {}", std::filesystem::path(loaderPath).string(), GetLastError());
			return false;
		}

		loaded = true;
		logger::info("[FidelityFX] Runtime DLLs loaded from {}", std::filesystem::path(pluginDirectory).string());
		return true;
	}

	void FidelityFXMessage(uint32_t a_type, const wchar_t* a_message)
	{
		if (!a_message) {
			return;
		}

		const auto message = std::filesystem::path(a_message).string();
		if (a_type == FFX_API_MESSAGE_TYPE_ERROR) {
			logger::error("[FidelityFX SDK] {}", message);
		} else {
			logger::warn("[FidelityFX SDK] {}", message);
		}
	}

	bool QueryConsumesReactiveMask(ffx::Context& a_context)
	{
		ffx::QueryDescUpscaleGetResourceRequirements resourceRequirements{};
		const auto result = ffx::Query(a_context, resourceRequirements);
		if (result != ffx::ReturnCode::Ok) {
			logger::warn("[FidelityFX] GetResourceRequirements failed: {}; keeping reactive mask enabled", static_cast<uint32_t>(result));
			return true;
		}

		constexpr uint64_t reactiveMask = FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK;
		return ((resourceRequirements.required_resources | resourceRequirements.optional_resources) & reactiveMask) != 0;
	}

}

void FidelityFX::CreateFSRResources()
{
	// The DX12 context is created lazily once the swapchain device and sizes are known.
}

void FidelityFX::DestroyFSRResources()
{
	if (context) {
		if (const auto result = ffx::DestroyContext(context); result != ffx::ReturnCode::Ok) {
			logger::warn("[FidelityFX] DestroyContext failed: {}", static_cast<uint32_t>(result));
		}
	}

	context = nullptr;
	contextDevice = nullptr;
	contextRenderSize = { 0.0f, 0.0f };
	contextDisplaySize = { 0.0f, 0.0f };
	contextConsumesReactiveMask = true;
}

void FidelityFX::DestroyFrameGenerationResources()
{
	if (frameGenContext) {
		ffx::ConfigureDescFrameGeneration config{};
		config.frameGenerationEnabled = false;
		std::ignore = ffx::Configure(frameGenContext, config);
		if (const auto result = ffx::DestroyContext(frameGenContext); result != ffx::ReturnCode::Ok) {
			logger::warn("[FidelityFX] DestroyContext(frame generation) failed: {}", static_cast<uint32_t>(result));
		}
	}

	if (frameGenSwapChainContext) {
		if (const auto result = ffx::DestroyContext(frameGenSwapChainContext); result != ffx::ReturnCode::Ok) {
			logger::warn("[FidelityFX] DestroyContext(frame generation swapchain) failed: {}", static_cast<uint32_t>(result));
		}
	}

	frameGenContext = nullptr;
	frameGenSwapChainContext = nullptr;
	frameGenDevice = nullptr;
	frameGenDisplaySize = { 0.0f, 0.0f };
	frameGenBackBufferFormat = DXGI_FORMAT_UNKNOWN;
	frameGenerationConfigured = false;
	frameGenerationEnabled = false;
}

void FidelityFX::CopyOpaqueTexture()
{
	static auto rendererData = RE::BSGraphics::GetRendererData();
	auto d3d11Context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	auto source = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kMainTemp)].texture);
	if (!source) {
		return;
	}

	D3D11_TEXTURE2D_DESC sourceDesc{};
	source->GetDesc(&sourceDesc);
	sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	sourceDesc.MiscFlags = 0;

	bool recreate = !colorOpaqueOnlyTexture || !colorOpaqueOnlyTexture->resource;
	if (!recreate) {
		D3D11_TEXTURE2D_DESC currentDesc{};
		colorOpaqueOnlyTexture->resource->GetDesc(&currentDesc);
		recreate =
			currentDesc.Width != sourceDesc.Width ||
			currentDesc.Height != sourceDesc.Height ||
			currentDesc.Format != sourceDesc.Format;
	}

	if (recreate) {
		colorOpaqueOnlyTexture = std::make_unique<Texture2D>(sourceDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = sourceDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		colorOpaqueOnlyTexture->CreateSRV(srvDesc);
	}

	d3d11Context->CopyResource(colorOpaqueOnlyTexture->resource.get(), source);
}

void FidelityFX::GenerateReactiveMask()
{
	// The DX12 path generates the mask immediately before dispatching FSR.
}

bool FidelityFX::EnsureContext(ID3D12Device* a_device, float2 a_renderSize, float2 a_displaySize)
{
	if (!a_device || a_renderSize.x <= 0.0f || a_renderSize.y <= 0.0f || a_displaySize.x <= 0.0f || a_displaySize.y <= 0.0f) {
		return false;
	}

	if (!LoadFidelityFXRuntime()) {
		return false;
	}

	const auto renderWidth = static_cast<uint32_t>(std::ceil(a_renderSize.x));
	const auto renderHeight = static_cast<uint32_t>(std::ceil(a_renderSize.y));
	const auto displayWidth = static_cast<uint32_t>(std::ceil(a_displaySize.x));
	const auto displayHeight = static_cast<uint32_t>(std::ceil(a_displaySize.y));

	if (context &&
		contextDevice == a_device &&
		static_cast<uint32_t>(contextRenderSize.x) == renderWidth &&
		static_cast<uint32_t>(contextRenderSize.y) == renderHeight &&
		static_cast<uint32_t>(contextDisplaySize.x) == displayWidth &&
		static_cast<uint32_t>(contextDisplaySize.y) == displayHeight) {
		return true;
	}

	DestroyFSRResources();

	ffx::CreateBackendDX12Desc backendDesc{};
	backendDesc.device = a_device;

	ffx::CreateContextDescUpscale createDesc{};
	createDesc.maxRenderSize = { renderWidth, renderHeight };
	createDesc.maxUpscaleSize = { displayWidth, displayHeight };
	createDesc.flags =
		FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
	createDesc.fpMessage = FidelityFXMessage;

	ffx::CreateContextDescUpscaleVersion versionDesc{};
	versionDesc.version = FFX_UPSCALER_VERSION;

	const auto result = ffx::CreateContext(context, nullptr, createDesc, backendDesc, versionDesc);
	if (result != ffx::ReturnCode::Ok) {
		logger::error("[FidelityFX] CreateContext failed: {}", static_cast<uint32_t>(result));
		context = nullptr;
		return false;
	}

	contextDevice = a_device;
	contextRenderSize = { static_cast<float>(renderWidth), static_cast<float>(renderHeight) };
	contextDisplaySize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
	contextConsumesReactiveMask = QueryConsumesReactiveMask(context);
	logger::info(
		"[FidelityFX] Created DX12 upscaler context render={}x{} display={}x{} reactiveMask={}",
		renderWidth,
		renderHeight,
		displayWidth,
		displayHeight,
		contextConsumesReactiveMask ? "enabled" : "ignored");
	return true;
}

bool FidelityFX::CreateFrameGenerationSwapChain(IDXGISwapChain4** a_swapChain, ID3D12CommandQueue* a_commandQueue)
{
	if (!a_swapChain || !*a_swapChain || !a_commandQueue) {
		return false;
	}

	if (!LoadFidelityFXRuntime()) {
		return false;
	}

	if (frameGenSwapChainContext) {
		return true;
	}

	ffx::CreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
	wrapDesc.swapchain = a_swapChain;
	wrapDesc.gameQueue = a_commandQueue;

	ffx::CreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc{};
	versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

	const auto result = ffx::CreateContext(frameGenSwapChainContext, nullptr, wrapDesc, versionDesc);
	if (result != ffx::ReturnCode::Ok) {
		logger::error("[FidelityFX] CreateContext(frame generation swapchain) failed: {}", static_cast<uint32_t>(result));
		frameGenSwapChainContext = nullptr;
		return false;
	}

	logger::info("[FidelityFX] Created frame generation swapchain context");
	return true;
}

bool FidelityFX::CreateFrameGenerationSwapChainForHwnd(
	IDXGIFactory* a_factory,
	HWND a_hwnd,
	DXGI_SWAP_CHAIN_DESC1* a_desc,
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_fullscreenDesc,
	ID3D12CommandQueue* a_commandQueue,
	IDXGISwapChain4** a_swapChain)
{
	if (!a_factory || !a_hwnd || !a_desc || !a_commandQueue || !a_swapChain) {
		return false;
	}

	if (!LoadFidelityFXRuntime()) {
		return false;
	}

	if (frameGenSwapChainContext) {
		return *a_swapChain != nullptr;
	}

	ffx::CreateContextDescFrameGenerationSwapChainForHwndDX12 createDesc{};
	createDesc.hwnd = a_hwnd;
	createDesc.desc = a_desc;
	createDesc.fullscreenDesc = a_fullscreenDesc;
	createDesc.dxgiFactory = a_factory;
	createDesc.gameQueue = a_commandQueue;
	createDesc.swapchain = a_swapChain;

	ffx::CreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc{};
	versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

	const auto result = ffx::CreateContext(frameGenSwapChainContext, nullptr, createDesc, versionDesc);
	if (result != ffx::ReturnCode::Ok) {
		logger::error("[FidelityFX] CreateContext(frame generation swapchain for hwnd) failed: {}", static_cast<uint32_t>(result));
		frameGenSwapChainContext = nullptr;
		return false;
	}

	logger::info("[FidelityFX] Created frame generation swapchain context for hwnd");
	return true;
}

bool FidelityFX::EnsureFrameGenerationContext(ID3D12Device* a_device, float2 a_displaySize, DXGI_FORMAT a_backBufferFormat)
{
	if (!a_device || a_displaySize.x <= 0.0f || a_displaySize.y <= 0.0f || a_backBufferFormat == DXGI_FORMAT_UNKNOWN) {
		return false;
	}

	if (!LoadFidelityFXRuntime()) {
		return false;
	}

	const auto displayWidth = static_cast<uint32_t>(std::ceil(a_displaySize.x));
	const auto displayHeight = static_cast<uint32_t>(std::ceil(a_displaySize.y));
	if (frameGenContext &&
		frameGenDevice == a_device &&
		static_cast<uint32_t>(frameGenDisplaySize.x) == displayWidth &&
		static_cast<uint32_t>(frameGenDisplaySize.y) == displayHeight &&
		frameGenBackBufferFormat == a_backBufferFormat) {
		return true;
	}

	if (frameGenContext) {
		ffx::ConfigureDescFrameGeneration config{};
		config.frameGenerationEnabled = false;
		std::ignore = ffx::Configure(frameGenContext, config);
		std::ignore = ffx::DestroyContext(frameGenContext);
		frameGenContext = nullptr;
	}

	ffx::CreateBackendDX12Desc backendDesc{};
	backendDesc.device = a_device;

	ffx::CreateContextDescFrameGeneration createDesc{};
	createDesc.displaySize = { displayWidth, displayHeight };
	createDesc.maxRenderSize = { displayWidth, displayHeight };
	createDesc.flags = 0;
	createDesc.backBufferFormat = ffxApiGetSurfaceFormatDX12(a_backBufferFormat);

	ffx::CreateContextDescFrameGenerationVersion versionDesc{};
	versionDesc.version = FFX_FRAMEGENERATION_VERSION;

	const auto result = ffx::CreateContext(frameGenContext, nullptr, createDesc, backendDesc, versionDesc);
	if (result != ffx::ReturnCode::Ok) {
		logger::error("[FidelityFX] CreateContext(frame generation) failed: {}", static_cast<uint32_t>(result));
		frameGenContext = nullptr;
		return false;
	}

	frameGenDevice = a_device;
	frameGenDisplaySize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
	frameGenBackBufferFormat = a_backBufferFormat;
	logger::info("[FidelityFX] Created frame generation context display={}x{} format={}", displayWidth, displayHeight, static_cast<uint32_t>(a_backBufferFormat));
	return true;
}

bool FidelityFX::ConfigureFrameGeneration(
	ID3D12Device* a_device,
	ID3D12GraphicsCommandList* a_commandList,
	IDXGISwapChain4* a_swapChain,
	ID3D12Resource* a_presentColor,
	ID3D12Resource* a_motionVectors,
	ID3D12Resource* a_depth,
	ID3D12Resource* a_hudlessColor,
	ID3D12Resource* a_uiColorAlpha,
	float2 a_jitter,
	float2 a_renderSize,
	float2 a_displaySize,
	DXGI_FORMAT a_backBufferFormat,
	uint64_t a_frameID,
	bool a_enabled)
{
	if (!frameGenSwapChainContext || !a_swapChain || !a_commandList) {
		return false;
	}

	if (!a_enabled && (!frameGenerationConfigured || !frameGenerationEnabled)) {
		return true;
	}

	const auto displayWidth = static_cast<uint32_t>(std::ceil(a_displaySize.x));
	const auto displayHeight = static_cast<uint32_t>(std::ceil(a_displaySize.y));
	const bool recreateFrameGenerationContext =
		frameGenContext &&
		(frameGenDevice != a_device ||
			static_cast<uint32_t>(frameGenDisplaySize.x) != displayWidth ||
			static_cast<uint32_t>(frameGenDisplaySize.y) != displayHeight ||
			frameGenBackBufferFormat != a_backBufferFormat);
	if (recreateFrameGenerationContext) {
		DX12SwapChain::GetSingleton()->WaitForGPUIdle();
	}
	if (!EnsureFrameGenerationContext(a_device, a_displaySize, a_backBufferFormat)) {
		return false;
	}

	static auto lastFrameTime = std::chrono::steady_clock::now();
	const auto currentFrameTime = std::chrono::steady_clock::now();
	const auto frameTimeMs = std::chrono::duration<float, std::milli>(currentFrameTime - lastFrameTime).count();
	lastFrameTime = currentFrameTime;

	ffx::ConfigureDescFrameGeneration config{};
	config.swapChain = a_swapChain;
	config.frameGenerationEnabled = a_enabled;
	config.allowAsyncWorkloads = false;
	config.onlyPresentGenerated = false;
	config.frameID = a_frameID;
	config.generationRect = {
		0,
		0,
		static_cast<int32_t>(a_displaySize.x),
		static_cast<int32_t>(a_displaySize.y)
	};
	config.HUDLessColor = a_hudlessColor ? ffxApiGetResourceDX12(a_hudlessColor, FFX_API_RESOURCE_STATE_COMPUTE_READ) : ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	config.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* pUserCtx) -> ffxReturnCode_t {
		return ffxDispatch(reinterpret_cast<ffxContext*>(pUserCtx), &params->header);
	};
	config.frameGenerationCallbackUserContext = &frameGenContext;

	if (const auto result = ffx::Configure(frameGenContext, config); result != ffx::ReturnCode::Ok) {
		logger::warn("[FidelityFX] Configure(frame generation) failed: {}", static_cast<uint32_t>(result));
		return false;
	}

	frameGenerationConfigured = true;
	frameGenerationEnabled = a_enabled;

	ffx::ConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiConfig{};
	uiConfig.uiResource = a_uiColorAlpha ? ffxApiGetResourceDX12(a_uiColorAlpha, FFX_API_RESOURCE_STATE_COMPUTE_READ) : ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	if (const auto result = ffx::Configure(frameGenSwapChainContext, uiConfig); result != ffx::ReturnCode::Ok) {
		logger::warn("[FidelityFX] Configure(frame generation UI) failed: {}", static_cast<uint32_t>(result));
	}

	if (!a_enabled) {
		return true;
	}

	if (!a_presentColor || !a_motionVectors || !a_depth) {
		return false;
	}

	ffx::DispatchDescFrameGenerationPrepareV2 prepare{};
	prepare.commandList = a_commandList;
	prepare.depth = ffxApiGetResourceDX12(a_depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	prepare.motionVectors = ffxApiGetResourceDX12(a_motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	prepare.jitterOffset = { -a_jitter.x, -a_jitter.y };
	prepare.motionVectorScale = { a_renderSize.x, a_renderSize.y };
	prepare.frameTimeDelta = std::max(frameTimeMs, 0.0f);
	prepare.renderSize = { static_cast<uint32_t>(a_renderSize.x), static_cast<uint32_t>(a_renderSize.y) };
	prepare.cameraNear = *reinterpret_cast<float*>(REL::ID{ 57985, 2712882 }.address());
	prepare.cameraFar = *reinterpret_cast<float*>(REL::ID{ 958877, 2712883 }.address());
	const auto cameraProjection = Util::GetCameraProjection();
	const auto* cameraState = cameraProjection.cameraState;
	if (!cameraState || !cameraProjection.usedMatrixFOV) {
		logger::warn(
			"[FidelityFX] Frame generation prepare has no valid world camera: camera={} fov={} matrixFov={}",
			static_cast<const void*>(cameraState),
			cameraProjection.cameraFOV,
			cameraProjection.usedMatrixFOV);
		return false;
	}
	prepare.cameraFovAngleVertical = cameraProjection.cameraFOV;
	prepare.viewSpaceToMetersFactor = 0.01428222656f;
	prepare.frameID = a_frameID;
	prepare.reset = false;

	Util::CameraBasis cameraBasis{};
	if (!Util::TryGetCameraBasis(cameraState->camViewData, cameraBasis)) {
		logger::warn("[FidelityFX] Frame generation prepare has no valid orthonormal camera basis");
		return false;
	}
	prepare.cameraPosition[0] = cameraState->currentPosAdjust.x;
	prepare.cameraPosition[1] = cameraState->currentPosAdjust.y;
	prepare.cameraPosition[2] = cameraState->currentPosAdjust.z;
	prepare.cameraForward[0] = cameraBasis.forward.x;
	prepare.cameraForward[1] = cameraBasis.forward.y;
	prepare.cameraForward[2] = cameraBasis.forward.z;
	prepare.cameraUp[0] = cameraBasis.up.x;
	prepare.cameraUp[1] = cameraBasis.up.y;
	prepare.cameraUp[2] = cameraBasis.up.z;
	prepare.cameraRight[0] = cameraBasis.right.x;
	prepare.cameraRight[1] = cameraBasis.right.y;
	prepare.cameraRight[2] = cameraBasis.right.z;

	const auto result = ffx::Dispatch(frameGenContext, prepare);
	if (result != ffx::ReturnCode::Ok) {
		logger::warn("[FidelityFX] Dispatch(frame generation prepare) failed: {}", static_cast<uint32_t>(result));
		return false;
	}

	return true;
}

bool FidelityFX::DisableFrameGeneration(
	ID3D12Device* a_device,
	ID3D12GraphicsCommandList* a_commandList,
	IDXGISwapChain4* a_swapChain,
	float2 a_displaySize,
	DXGI_FORMAT a_backBufferFormat)
{
	return ConfigureFrameGeneration(
		a_device,
		a_commandList,
		a_swapChain,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		{ 0.0f, 0.0f },
		{ 0.0f, 0.0f },
		a_displaySize,
		a_backBufferFormat,
		0,
		false);
}

bool FidelityFX::UpscaleD3D12(
	ID3D12Device* a_device,
	ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource* a_color,
	ID3D12Resource* a_output,
	ID3D12Resource* a_motionVectors,
	ID3D12Resource* a_depth,
	ID3D12Resource* a_reactiveMask,
	ID3D12Resource* a_opaqueOnlyColor,
	float2 a_jitter,
	float2 a_renderSize,
	float2 a_displaySize,
	float a_sharpness)
{
	if (!a_commandList || !a_color || !a_output || !a_motionVectors || !a_depth) {
		return false;
	}

	const auto renderWidth = static_cast<uint32_t>(std::ceil(a_renderSize.x));
	const auto renderHeight = static_cast<uint32_t>(std::ceil(a_renderSize.y));
	const auto displayWidth = static_cast<uint32_t>(std::ceil(a_displaySize.x));
	const auto displayHeight = static_cast<uint32_t>(std::ceil(a_displaySize.y));
	const bool recreateUpscaleContext =
		context &&
		(contextDevice != a_device ||
			static_cast<uint32_t>(contextRenderSize.x) != renderWidth ||
			static_cast<uint32_t>(contextRenderSize.y) != renderHeight ||
			static_cast<uint32_t>(contextDisplaySize.x) != displayWidth ||
			static_cast<uint32_t>(contextDisplaySize.y) != displayHeight);
	if (recreateUpscaleContext) {
		DX12SwapChain::GetSingleton()->WaitForGPUIdle();
	}
	if (!EnsureContext(a_device, a_renderSize, a_displaySize)) {
		return false;
	}

	static auto lastFrameTime = std::chrono::steady_clock::now();
	const auto currentFrameTime = std::chrono::steady_clock::now();
	const auto frameTimeMs = std::chrono::duration<float, std::milli>(currentFrameTime - lastFrameTime).count();
	lastFrameTime = currentFrameTime;

	auto reactiveResource = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	if (a_reactiveMask && a_opaqueOnlyColor) {
		bool reactiveMaskGenerated = false;
		if (contextConsumesReactiveMask) {
			ffx::DispatchDescUpscaleGenerateReactiveMask reactiveDispatch{};
			reactiveDispatch.commandList = a_commandList;
			reactiveDispatch.colorOpaqueOnly = ffxApiGetResourceDX12(a_opaqueOnlyColor, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
			reactiveDispatch.colorPreUpscale = ffxApiGetResourceDX12(a_color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
			reactiveDispatch.outReactive = ffxApiGetResourceDX12(a_reactiveMask, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
			reactiveDispatch.renderSize = { static_cast<uint32_t>(a_renderSize.x), static_cast<uint32_t>(a_renderSize.y) };
			reactiveDispatch.scale = 0.5f;
			reactiveDispatch.cutoffThreshold = 0.2f;
			reactiveDispatch.binaryValue = 0.8f;
			reactiveDispatch.flags =
				FFX_UPSCALE_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;

			if (const auto result = ffx::Dispatch(context, reactiveDispatch); result != ffx::ReturnCode::Ok) {
				logger::warn("[FidelityFX] Reactive mask dispatch failed: {}", static_cast<uint32_t>(result));
			} else {
				reactiveMaskGenerated = true;
			}
		}

		const auto reactiveReadBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			a_reactiveMask,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		a_commandList->ResourceBarrier(1, &reactiveReadBarrier);

		if (reactiveMaskGenerated) {
			reactiveResource = ffxApiGetResourceDX12(a_reactiveMask, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		}
	}

	ffx::DispatchDescUpscale dispatch{};
	dispatch.commandList = a_commandList;
	dispatch.color = ffxApiGetResourceDX12(a_color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.depth = ffxApiGetResourceDX12(a_depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.motionVectors = ffxApiGetResourceDX12(a_motionVectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.exposure = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.reactive = reactiveResource;
	dispatch.transparencyAndComposition = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.output = ffxApiGetResourceDX12(a_output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
	dispatch.jitterOffset = { -a_jitter.x, -a_jitter.y };
	dispatch.motionVectorScale = { a_renderSize.x, a_renderSize.y };
	dispatch.renderSize = { static_cast<uint32_t>(a_renderSize.x), static_cast<uint32_t>(a_renderSize.y) };
	dispatch.upscaleSize = { static_cast<uint32_t>(a_displaySize.x), static_cast<uint32_t>(a_displaySize.y) };
	const auto sharpness = std::clamp(a_sharpness, 0.0f, 1.0f);
	dispatch.enableSharpening = sharpness > 0.0f;
	dispatch.sharpness = sharpness;
	dispatch.frameTimeDelta = std::max(frameTimeMs, 0.0f);
	dispatch.preExposure = 1.0f;
	dispatch.reset = false;
	dispatch.cameraNear = *reinterpret_cast<float*>(REL::ID{ 57985, 2712882 }.address());
	dispatch.cameraFar = *reinterpret_cast<float*>(REL::ID{ 958877, 2712883 }.address());
	// Match the original D3D11 FSR path: SR does not need the camera matrices
	// required by frame generation and must not sample the late image-space camera.
	dispatch.cameraFovAngleVertical = 1.0f;
	dispatch.viewSpaceToMetersFactor = 0.01428222656f;
	dispatch.flags = 0;

	const auto result = ffx::Dispatch(context, dispatch);
	if (result != ffx::ReturnCode::Ok) {
		logger::error("[FidelityFX] Dispatch failed: {}", static_cast<uint32_t>(result));
		return false;
	}

	return true;
}
