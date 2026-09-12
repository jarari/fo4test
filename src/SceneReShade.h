#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>

namespace SceneReShade
{
	void Initialize();
	// Native device for ALL output/FG work. Retain addonLifetime until that domain
	// is destroyed; never create commands/resources through the retained wrapper.
	HRESULT CreateOutputDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumLevel,
		ID3D12Device** device, ID3D12Device** addonLifetime);
	bool IsAvailable() noexcept;
	// Render-thread query; Reset never destroys the runtime from another thread.
	bool ShouldRenderHDR() noexcept;
	void Reset(); // Deferred: destruction occurs at the next render entry, never in a ReShade callback.

	// Pre-tonemap: supply the CURRENT ImageSpaceEffect input, not a hard-coded RT2.
	// A larger depth allocation defaults to a top-left crop; pass depthRect for other layouts.
	void RenderHDR(ID3D11Texture2D* color, ID3D11ShaderResourceView* depth, HWND window,
		UINT width, UINT height, const RECT* depthRect, std::uint64_t frame);

	// Native screen-space UI stage (after NR/SR): advance the runtime once, draw
	// its SDR menu, and premultiplied-alpha composite into the D3D11 UI layer.
	// The engine's swapchain RT0 keeps views, not a texture pointer. Resolve the
	// resource and full display extent from its RTV (also valid for ENB's alias).
	void RenderUI(ID3D11RenderTargetView* target, HWND window, std::uint64_t frame);
}
