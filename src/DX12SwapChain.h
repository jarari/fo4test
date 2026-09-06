#pragma once

#include <atomic>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "Buffer.h"
#include "FrameCount.h"

class Streamline;

class D3D11D3D12SharedTexture
{
public:
	D3D11D3D12SharedTexture(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11Device* a_d3d11Device, ID3D12Device* a_d3d12Device);

	winrt::com_ptr<ID3D11Texture2D> resource11;
	winrt::com_ptr<ID3D12Resource> resource12;
};

struct DXGISwapChainProxy final : IDXGISwapChain4
{
	explicit DXGISwapChainProxy(IDXGISwapChain4* a_swapChain);
	void SetSwapChain(IDXGISwapChain4* a_swapChain);

	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;

	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
	HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;
	HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

	HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
	HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
	HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
	HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override;
	HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
	HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
	HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override;
	HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
	HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
	HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

	HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
	HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
	HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override;
	HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override;
	HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
	BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
	HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override;
	HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override;
	HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override;
	HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override;
	HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override;

	HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
	HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override;
	HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
	HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
	HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
	HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
	HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override;

	UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
	HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) override;
	HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override;
	HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) override;

	HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override;

private:
	std::atomic<ULONG> refCount{ 1 };
	winrt::com_ptr<IDXGISwapChain4> swapChain;
};

class DX12SwapChain
{
public:
	static DX12SwapChain* GetSingleton()
	{
		static DX12SwapChain singleton;
		return &singleton;
	}

	void CreateD3D12Device(IDXGIAdapter* a_adapter, Streamline* a_streamline);
	void CreateSwapChain(IDXGIFactory5* a_dxgiFactory, const DXGI_SWAP_CHAIN_DESC& a_swapChainDesc, Streamline* a_streamline, bool a_useFidelityFXFrameGeneration);
	void CreateInterop();

	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);

	DXGISwapChainProxy* GetSwapChainProxy() const { return swapChainProxy; }
	bool IsReady() const { return swapChainProxy && swapChain && interopReady; }
	bool IsWindowMinimized() const { return windowMinimized.load(std::memory_order_acquire) || (hwnd && IsIconic(hwnd)); }
	bool IsWindowUnavailable() const { return IsWindowMinimized(); }
	bool AreTemporalFeaturesSuspended() const { return temporalFeaturesSuspended; }
	UINT GetFrameIndex() const { return frameIndex; }
	ID3D12Device* GetD3D12Device() const { return d3d12Device.get(); }
	bool WaitForFrameSlot(UINT a_frameIndex);
	bool WaitForGPUIdle();
	bool WaitForInteropIdle();
	HRESULT ResizeENBScene(uint32_t a_quality);
	bool BeginNativeUI();
	void PublishNativeUIForOverlays();
	void EndNativeUI();
	uint64_t NativeUIGeneration() const { return nativeUIGeneration; }
	uint64_t ENBSceneResizeGeneration() const { return enbSceneResizeGeneration; }
	HRESULT ResizeBuffers(UINT a_bufferCount, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags);
	HRESULT ResizeBuffers1(UINT a_bufferCount, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags, const UINT* a_creationNodeMask, IUnknown* const* a_presentQueue);
	HRESULT SetFullscreenState(BOOL a_fullscreen, IDXGIOutput* a_target);
	HRESULT ResizeTarget(const DXGI_MODE_DESC* a_newTargetParameters);
	void OnWindowMessage(UINT a_msg, WPARAM a_wParam, LPARAM a_lParam);

	HRESULT Present(UINT SyncInterval, UINT Flags);
	struct D3D12EvaluationResult
	{
		bool dlss = false;
		bool fsr = false;
		bool fsrFrameGeneration = false;

		bool Any() const { return dlss || fsr || fsrFrameGeneration; }
	};
	D3D12EvaluationResult EvaluateD3D12WorkForCurrentFrame(bool a_evaluateDLSS, bool a_evaluateFSR, bool a_evaluateFSRFrameGeneration, bool a_waitForD3D11Consumption = true);
	bool EvaluateD3D12DLSSForCurrentFrame();
	bool EvaluateD3D12FSRForCurrentFrame();
	bool EvaluateFSRFrameGenerationForCurrentFrame();
	void SetPresentOverride(ID3D12Resource* a_finalColor);
	bool EnsureFidelityFXFrameGenerationSwapChain();
	HRESULT GetBuffer(UINT a_buffer, REFIID a_riid, void** a_surface);
	HRESULT GetDevice(REFIID a_riid, void** a_device);
	void InstallWndProcHook(HWND a_hwnd);
	LRESULT CallOriginalWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam) const;

	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	winrt::com_ptr<IDXGISwapChain4> swapChain;
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};

private:
	struct CommandContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> list;
		std::unique_ptr<D3D11D3D12SharedTexture> presentStaging;
		winrt::com_ptr<ID3D12Resource> retainedPresentOverride;
		UINT index = 0;
		UINT64 fenceValue = 0;
	};

	static constexpr std::uint32_t kCommandContextCount = kDX12FrameCount * 4;

	CommandContext& AcquireCommandContext();
	void ExecuteCommandContext(CommandContext& a_context);
	bool FenceFrameSlotAfterPresent(UINT a_frameIndex, CommandContext* a_context = nullptr);
	D3D12EvaluationResult EvaluateD3D12WorkOnCommandList(ID3D12GraphicsCommandList* a_commandList, UINT a_frameIndex, bool a_evaluateDLSS, bool a_evaluateFSR, bool a_evaluateFSRFrameGeneration);
	bool WaitForCommandFence(UINT64 a_value);
	void RefreshBackBuffers();
	void RecreateInteropTextures();
	void ProcessWindowStateTransition();
	void SuspendTemporalFeatures(const char* a_reason);
	void ResumeTemporalFeatures();
	void ReleaseResizeDependentResources();
	HRESULT ResizeBuffersInternal(bool a_useResizeBuffers1, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags, const UINT* a_creationNodeMask);
	void RestoreResizeDependentResources(const char* a_context);

	winrt::com_ptr<ID3D12Device> proxyD3D12Device;
	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;
	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;
	winrt::com_ptr<ID3D12Fence> commandFence;
	winrt::com_ptr<ID3D12Resource> swapChainBuffers[kDX12FrameCount];
	std::unique_ptr<Texture2D> swapChainBufferProxy;
	std::unique_ptr<D3D11D3D12SharedTexture> swapChainBufferProxyENB;
	std::unique_ptr<D3D11D3D12SharedTexture> pendingSceneProxy;
	bool sceneResizeActive = false;
	bool sceneResizeObserved = false;
	uint32_t pendingSceneQuality = 0;
	uint64_t enbSceneResizeGeneration = 0;
	std::unique_ptr<Texture2D> nativeUITexture;
	winrt::com_ptr<ID3D11ShaderResourceView> sceneUISRV;
	winrt::com_ptr<ID3D11ComputeShader> nativeUIResolve;
	winrt::com_ptr<ID3D11SamplerState> nativeUISampler;
	RE::BSGraphics::RenderTarget savedSceneTarget{};
	RE::BSGraphics::RenderTarget savedSceneWindowTarget{};
	bool nativeUIActive = false;
	bool nativeUIWindowTargetActive = false;
	bool nativeUIIncludesScene = false;
	std::unique_ptr<D3D11D3D12SharedTexture> menuComposite;
	void ResolveNativeUIForMenu();
	uint64_t nativeUIGeneration = 0;
	CommandContext commandContexts[kCommandContextCount];
	winrt::handle commandFenceEvent;
	winrt::com_ptr<ID3D12Resource> presentOverrideFinalColor;
	DXGISwapChainProxy* swapChainProxy = nullptr;
	UINT frameIndex = 0;
	UINT nextCommandContext = 0;
	UINT64 fenceValue = 1;
	UINT64 commandFenceValue = 1;
	std::array<UINT64, kDX12FrameCount> frameSlotFenceValues{};
	bool fidelityFXFrameGenerationSwapChainAllowed = false;
	double desktopRefreshHz = 0.0;
	HWND hwnd = nullptr;
	WNDPROC originalWndProc = nullptr;
	std::atomic_bool windowMinimized{ false };
	std::atomic_bool windowStateDirty{ false };
	bool temporalFeaturesSuspended = false;
	bool deviceLost = false;
	bool interopReady = false;
};
