#pragma once

#include "Buffer.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <ffx_api.hpp>
#include <ffx_upscale.hpp>
#include <ffx_framegeneration.hpp>
#include <dx12/ffx_api_framegeneration_dx12.hpp>

#include <memory>

/**
 * @class FidelityFX
 * @brief Manager for AMD FidelityFX Super Resolution 3 (FSR3) upscaling
 *
 * Handles initialization, resource management, and execution of FidelityFX
 * upscaling through the DX12 backend exposed by the current SDK.
 */
class FidelityFX
{
public:
	// ========================================
	// Singleton
	// ========================================

	/**
	 * @brief Get the singleton instance
	 * @return Pointer to the global FidelityFX instance
	 */
	static FidelityFX* GetSingleton()
	{
		static FidelityFX singleton;
		return &singleton;
	}

	// ========================================
	// Resource Management
	// ========================================

	/**
	 * @brief Create FSR3 context and resources
	 *
	 * Initializes the FSR3 backend, allocates scratch memory, creates the FSR3 context,
	 * and allocates textures for opaque-only color and reactive mask generation.
	 *
	 * @note Should only be called when FSR is the active upscaling method
	 */
	void CreateFSRResources();

	/**
	 * @brief Destroy FSR3 context and release resources
	 *
	 * Destroys the FSR3 context, frees scratch memory, and releases all FSR-specific textures.
	 * Automatically called when switching to a different upscaling method.
	 */
	void DestroyFSRResources();

	// ========================================
	// FSR3 Operations
	// ========================================

	/**
	 * @brief Copy opaque-only color buffer for reactive mask generation
	 *
	 * Captures the scene color before transparent/alpha-blended objects are rendered.
	 * This is used to generate a reactive mask that improves FSR quality around particles,
	 * effects, and transparent surfaces.
	 *
	 * @note Must be called before rendering transparent objects
	 */
	void CopyOpaqueTexture();

	/**
	 * @brief Generate reactive mask for FSR3
	 *
	 * Compares opaque-only and final color buffers to create a reactive mask.
	 * The reactive mask tells FSR3 which pixels contain particles or effects that
	 * need special handling to avoid temporal artifacts.
	 *
	 * @note Must be called after CopyOpaqueTexture and after transparent rendering
	 */
	void GenerateReactiveMask();

	bool UpscaleD3D12(
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
		float a_sharpness);

	bool CreateFrameGenerationSwapChain(IDXGISwapChain4** a_swapChain, ID3D12CommandQueue* a_commandQueue);
	bool CreateFrameGenerationSwapChainForHwnd(
		IDXGIFactory* a_factory,
		HWND a_hwnd,
		DXGI_SWAP_CHAIN_DESC1* a_desc,
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_fullscreenDesc,
		ID3D12CommandQueue* a_commandQueue,
		IDXGISwapChain4** a_swapChain);
	void DestroyFrameGenerationResources();
	bool ConfigureFrameGeneration(
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
		bool a_enabled);
	bool DisableFrameGeneration(
		ID3D12Device* a_device,
		ID3D12GraphicsCommandList* a_commandList,
		IDXGISwapChain4* a_swapChain,
		float2 a_displaySize,
		DXGI_FORMAT a_backBufferFormat);
	bool IsFrameGenerationSwapChainActive() const { return frameGenSwapChainContext != nullptr; }
	bool IsFrameGenerationEnabled() const { return frameGenerationEnabled; }

	// ========================================
	// Resources
	// ========================================

	std::unique_ptr<Texture2D> colorOpaqueOnlyTexture;  ///< Color before transparent objects
	std::unique_ptr<Texture2D> reactiveMaskTexture;     ///< Generated reactive mask for FSR3

private:
	bool EnsureContext(ID3D12Device* a_device, float2 a_renderSize, float2 a_displaySize);
	bool EnsureFrameGenerationContext(ID3D12Device* a_device, float2 a_displaySize, DXGI_FORMAT a_backBufferFormat);

	ffx::Context context = nullptr;
	ffx::Context frameGenContext = nullptr;
	ffx::Context frameGenSwapChainContext = nullptr;
	ID3D12Device* contextDevice = nullptr;
	ID3D12Device* frameGenDevice = nullptr;
	float2 contextRenderSize = { 0.0f, 0.0f };
	float2 contextDisplaySize = { 0.0f, 0.0f };
	bool contextConsumesReactiveMask = true;
	float2 frameGenDisplaySize = { 0.0f, 0.0f };
	DXGI_FORMAT frameGenBackBufferFormat = DXGI_FORMAT_UNKNOWN;
	bool frameGenerationConfigured = false;
	bool frameGenerationEnabled = false;
};
