#pragma once
#include "Util.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <xess/xess_d3d12.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>
#include <atomic>
#include <chrono>

class XeSS
{
public:
    static XeSS* GetSingleton() { static XeSS instance; return &instance; }
    bool PrepareSR(ID3D12Device* device, UINT width, UINT height, uint quality, DXGI_FORMAT format);
    bool Upscale(ID3D12GraphicsCommandList* commands, ID3D12Resource* color, ID3D12Resource* output,
        ID3D12Resource* motion, ID3D12Resource* depth, float2 jitter, float2 renderSize);
    void DestroySR(); // Caller drains GPU first.
    void RequestReset() { resetSerial.fetch_add(1, std::memory_order_relaxed); }
    bool CreateSwapChain(ID3D12Device* device, IDXGIFactory2* factory, ID3D12CommandQueue* queue,
        HWND window, const DXGI_SWAP_CHAIN_DESC1& desc, IDXGISwapChain4** chain);
    bool DestroyFG(); // Release application proxy references first; XeFG drains its own pacer.
    bool TagFrame(ID3D12GraphicsCommandList* commands, ID3D12Resource* color, ID3D12Resource* motion,
        ID3D12Resource* depth, float2 jitter, float2 renderSize, float2 displaySize, uint generated);
    void DisableFG();
    void BeforePresent();
    void AfterPresent(HRESULT result);
    void Sleep(uint32_t engineFrame);
    void Marker(xell_latency_marker_type_t marker);
    void EndSimulation(uint32_t engineFrame);
    bool OwnsSwapChain() const { return fgInitialized; }
    const char* FailureReason() const { return failureReason; }
    bool FGEnabled() const { return fgEnabled; }
    uint MaxGeneratedFrames() const { return maxGenerated; }
    uint PacingMultiplier() const { return fgEnabled ? generatedFrames + 1 : 1; }
    uint64_t PresentedFrames() const { return presentedFrames.load(std::memory_order_relaxed); }
private:
    xess_context_handle_t sr = nullptr;
    xefg_swapchain_handle_t fg = nullptr;
    xell_context_handle_t ll = nullptr;
    ID3D12Device* srDevice = nullptr;
    UINT srWidth = 0, srHeight = 0, srQuality = UINT_MAX;
    DXGI_FORMAT srFormat = DXGI_FORMAT_UNKNOWN, fgFormat = DXGI_FORMAT_UNKNOWN;
    std::chrono::steady_clock::time_point lastTagTime = std::chrono::steady_clock::now();
    bool fgInitialized = false, fgEnabled = false;
    uint maxGenerated = 1, generatedFrames = 1;
    uint32_t taggedId = 0;
    std::atomic<uint32_t> simulationPresentId{0};
    std::atomic<uint32_t> presentId{1}, sleepFrame{UINT_MAX};
    const char* failureReason = nullptr;
    std::atomic<uint64_t> resetSerial{1}, presentedFrames{0};
    uint64_t srReset = 0, fgReset = 0;
};
