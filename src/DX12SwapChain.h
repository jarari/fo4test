#pragma once
#include "PresentPacing.h"

#include <atomic>
#include <mutex>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "Buffer.h"
#include "FrameCount.h"
#include "FrameTiming.h"
#include "OSD.h"

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
	friend class DX12SwapChain;
	class BackendUse
	{
	public:
		explicit BackendUse(DXGISwapChainProxy& a_owner);
		~BackendUse();
		BackendUse(const BackendUse&) = delete;
		BackendUse& operator=(const BackendUse&) = delete;
		IDXGISwapChain4* Get() const { return backend.get(); }

	private:
		DXGISwapChainProxy& owner;
		winrt::com_ptr<IDXGISwapChain4> backend;
	};

	template <class Result, class Callback>
	Result WithBackend(Result a_unavailable, Callback&& a_call)
	{
		BackendUse use(*this);
		return use.Get() ? a_call(use.Get()) : a_unavailable;
	}
	bool TryDetachBackend();

	std::atomic<ULONG> refCount{ 1 };
	std::mutex backendMutex;
	unsigned backendCalls = 0;
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
	void UpdatePresentationBackend();

	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);

	DXGISwapChainProxy* GetSwapChainProxy() const { return swapChainProxy; }
	DXGI_FORMAT GetBackBufferFormat() const
	{
		return frameIndex < kDX12FrameCount && swapChainBuffers[frameIndex] ?
			swapChainBuffers[frameIndex]->GetDesc().Format : (UsesNativePresentation() ? swapChainDesc.Format : DXGI_FORMAT_UNKNOWN);
	}
	bool IsReady() const { return interopReady.load(std::memory_order_acquire) && !changingPresentation.load(std::memory_order_acquire); }
	bool UsesNativePresentation() const { return nativePresentation.load(std::memory_order_acquire); }
	bool CanEvaluateD3D12() const { return IsReady() && !deviceLost; }
	bool CanUseD3D12FrameGeneration() const { return CanEvaluateD3D12() && !UsesNativePresentation(); }
	void PaceFrameStart(uint32_t frame);
	void NoteSimulationStart(uint32_t frame);
	bool GetSimulationToAppGPUCompletionMs(float& a_ms);
	enum class LoadingSource : uint64_t { Menu = 1, Save = 2 };
	void NotifyLoadingScreen(bool active, LoadingSource source = LoadingSource::Menu);
	void BeginLoadingFrame();
	bool IsLoadingScreen() const { return (loadingState.load(std::memory_order_acquire) & kLoadingSources) != 0; }
	bool IsLoadingRecoveryPending() const { return IsLoadingScreen() || loadingResumeFrames.load(std::memory_order_acquire) != 0 || (loadingState.load(std::memory_order_acquire) & ~kLoadingSources) != appliedLoadingEpoch.load(std::memory_order_acquire); }
	bool IsWindowMinimized() const { return windowMinimized.load(std::memory_order_acquire) || (hwnd && IsIconic(hwnd)); }
	bool IsWindowUnavailable() const { return IsWindowMinimized(); }
	bool AreTemporalFeaturesSuspended() const { return temporalFeaturesSuspended; }
	UINT GetFrameIndex() const { return frameIndex; }
	ID3D12Resource* GetPresentBufferForDiagnostics(UINT a_index) const { return a_index < kDX12FrameCount ? swapChainBuffers[a_index].get() : nullptr; }
	void WaitForFrameStart();
	void AdvanceLoadingRecovery();
	ID3D12Device* GetD3D12Device() const { return d3d12Device.get(); }
	ID3D12Fence* GetInteropReadyFence() const { return d3d12Fence.get(); }
	bool WaitForFrameSlot(UINT a_frameIndex, bool a_inputsOnly = false);
	bool IsFrameSlotComplete(UINT slot) const;
	// Retirement is stamped after this frame's recorded work is submitted.
	bool GetRetirementFences(uint64_t& a_d3d11, uint64_t& a_d3d12);
	bool AreRetirementFencesComplete(uint64_t a_d3d11, uint64_t a_d3d12) const;
	bool WaitForGPUIdle();
	bool WaitForInteropIdle();
	HRESULT ResizeENBScene(uint32_t a_quality);
	bool BeginNativeUI();
	// Preserve destination-dependent engine menu blending before its first draw.
	bool PrepareNativeUIForEngineComposition(const char* marker);
	void PublishNativeUIForOverlays();
	void EndNativeUI();
	// A same-frame, display-sized image for the engine's synchronous screenshot encoder.
	std::shared_ptr<D3D11D3D12SharedTexture> CaptureScreenshot();
	uint64_t NativeUIGeneration() const { return nativeUIGeneration; }
	bool IsNativeUIActive() const { return nativeUIActive; }
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
		bool externalFrameGeneration = false;

		bool Any() const { return dlss || fsr || externalFrameGeneration; }
	};
	D3D12EvaluationResult EvaluateD3D12WorkForCurrentFrame(bool a_evaluateDLSS, bool a_evaluateFSR, bool a_evaluateFSRFrameGeneration, bool a_waitForD3D11Consumption = true);
	bool EvaluateD3D12DLSSForCurrentFrame();
	bool EvaluateD3D12FSRForCurrentFrame();
	void SetPresentOverride(ID3D12Resource* a_finalColor, Texture2D* a_sharedColor = nullptr, bool a_evaluated = false);
	bool EnsureExternalFrameGenerationSwapChain();
	HRESULT GetBuffer(UINT a_buffer, REFIID a_riid, void** a_surface);
	HRESULT GetDevice(REFIID a_riid, void** a_device);
	void InstallWndProcHook(HWND a_hwnd);
	LRESULT CallOriginalWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam) const;

	// Preserve the automatic ReShade device/queue wrappers for final presentation.
	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	winrt::com_ptr<IDXGISwapChain4> swapChain;
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};

private:
	struct CommandContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> list;
		winrt::com_ptr<ID3D12Resource> retainedPresentOverride;
		// Reused only after this context's fence completes, unlike a global
		// upload buffer which CPU text updates could overwrite while in flight.
		OSD::Frame12 osd;
		winrt::com_ptr<ID3D12Resource> latencyReadback;
		FrameTiming::Gpu12Slot timing;
		uint32_t latencyFrame = 0;
		uint64_t latencyGeneration = 0;
		uint64_t latencyCalibrationGPU = 0;
		uint64_t latencyCalibrationCPU = 0;
		uint64_t latencySimulationQPC = 0;
		bool latencyPending = false;
		bool latencyPresentSucceeded = false;
		std::shared_ptr<D3D11D3D12SharedTexture> screenshotInput;
		std::shared_ptr<D3D11D3D12SharedTexture> screenshotOutput;
		UINT index = 0;
		UINT64 fenceValue = 0;
	};

	static constexpr std::uint32_t kCommandContextCount = kDX12FrameCount * 4;

	CommandContext& AcquireCommandContext();
	void ExecuteCommandContext(CommandContext& a_context, bool a_fenceAfterPresent = false);
	void RecordLatencyTimestamp(CommandContext& a_context, uint32_t a_frame);
	void ConsumeLatencyTimestamp(CommandContext& a_context);
	void ResetLatencyTelemetry();
	bool FenceFrameSlotAfterPresent(UINT a_frameIndex, CommandContext* a_context = nullptr);
	D3D12EvaluationResult EvaluateD3D12WorkOnCommandList(ID3D12GraphicsCommandList* a_commandList, UINT a_frameIndex, bool a_evaluateDLSS, bool a_evaluateFSR, bool a_evaluateFSRFrameGeneration);
	bool WaitForCommandFence(UINT64 a_value, bool a_publishD3D11 = true);
	bool QueueD3D11ReuseWait(UINT64 a_value, const char* gpuMarker);
	void RefreshBackBuffers();
	void RecreateInteropTextures();
	void ProcessWindowStateTransition();
	void SuspendTemporalFeatures(const char* a_reason);
	void ResumeTemporalFeatures();
	void ReleaseResizeDependentResources();
	HRESULT ResizeBuffersInternal(bool a_useResizeBuffers1, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags, const UINT* a_creationNodeMask);
	void RestoreResizeDependentResources(const char* a_context);
	HRESULT PresentNative(UINT a_syncInterval, UINT a_flags);
	HRESULT CreateNativeSwapChain(IDXGIFactory2* a_factory, const DXGI_SWAP_CHAIN_DESC1& a_desc);
	bool ReleasePresentationBackend();
	bool SetDLSSGPresentationHooks(bool a_enabled);

	winrt::com_ptr<ID3D12Device> proxyD3D12Device;
	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;
	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;
	winrt::com_ptr<ID3D12Fence> commandFence;
	winrt::com_ptr<ID3D11Fence> d3d11CommandFence;
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
	bool nativeUIPassthrough = false;
	bool nativeUIWindowTargetActive = false;
	bool nativeUIIncludesScene = false;
	std::unique_ptr<D3D11D3D12SharedTexture> menuComposite;
	std::unique_ptr<Texture2D> menuComposite11;
	winrt::com_ptr<ID3D11ComputeShader> menuCompositeShader;
	winrt::com_ptr<ID3D11UnorderedAccessView> menuOverlayUAV;
	winrt::com_ptr<ID3DDeviceContextState> menuComputeState;
	winrt::com_ptr<ID3D11Texture2D> menuOverlayTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> menuOverlayView;
	bool ResolveNativeUIOnD3D11(ID3D11Texture2D* a_uiTarget);
	void ResolveNativeUIForMenu(const char* marker);
	uint64_t nativeUIGeneration = 0;
	CommandContext commandContexts[kCommandContextCount];
	winrt::com_ptr<ID3D12QueryHeap> latencyQueryHeap;
	uint64_t gpuTimestampFrequency = 0;
	uint32_t lastProfileCalibrationFrame = UINT32_MAX;
	uint64_t qpcFrequency = 0;
	struct SimulationStart { uint32_t frame = 0; uint64_t qpc = 0; uint64_t generation = 0; };
	std::array<SimulationStart, 64> simulationStarts{};
	std::mutex simulationStartMutex;
	std::mutex latencySampleMutex;
	std::atomic<uint64_t> latencyGeneration{1};
	float latestAppGPULatencyMs = 0.0f;
	uint64_t latestAppGPULatencyGeneration = 0;
	uint64_t latestAppGPULatencySampleQPC = 0;
	// Presentation uses the swapchain's three fenced slots; menu resolves have
	// one independently fenced snapshot so evaluation contexts need no image.
	std::array<std::unique_ptr<D3D11D3D12SharedTexture>, kDX12FrameCount> presentStaging;
	std::unique_ptr<D3D11D3D12SharedTexture> menuStaging;
	UINT64 menuStagingFenceValue = 0;
	winrt::com_ptr<ID3D12Resource> presentOverrideFinalColor;
	winrt::com_ptr<ID3D11ShaderResourceView> presentOverrideSharedColor;
	UINT64 presentOverrideProducerFence = 0;
	DXGISwapChainProxy* swapChainProxy = nullptr;
	winrt::com_ptr<IDXGIFactory5> presentationFactory;
	DXGI_SWAP_CHAIN_DESC originalPresentationDesc{};
	std::atomic_bool nativePresentation{ false };
	std::atomic_bool changingPresentation{ false };
	ULONGLONG nextPresentationRetry = 0;
	UINT frameIndex = 0;
	UINT nextCommandContext = 0;
	UINT64 fenceValue = 1;
	UINT64 commandFenceValue = 1;
	std::array<UINT64, kDX12FrameCount> frameSlotFenceValues{};
	std::array<UINT64, kDX12FrameCount> inputReuseFenceValues{};
	std::array<UINT64, kDX12FrameCount> queuedReuseFenceValues{};
	std::array<std::atomic_uint64_t, kDX12FrameCount> presentSlotFenceValues{};
	std::array<bool, kDX12FrameCount> inputsUsedAtPresent{};
	bool fidelityFXFrameGenerationSwapChainAllowed = false;
	double desktopRefreshHz = 0.0;
	PresentPacing presentPacing;
	// Creation-time contract, independent of FG mode and the SL proxy's internal flags.
	bool applicationFrameLatencyWaitable = false;
	void ConfigureFrameLatency();
	void WaitForPresentationCapacity(uint32_t frame);
	std::mutex frameLatencyMutex;
	static constexpr uint64_t kLoadingSources = static_cast<uint64_t>(LoadingSource::Menu) | static_cast<uint64_t>(LoadingSource::Save);
	std::atomic<uint64_t> loadingState{0};
	std::atomic<uint64_t> appliedLoadingEpoch{0};
	std::atomic<unsigned> loadingResumeFrames{0};
	ULONGLONG frameLatencyRetryAfter = 0;
	winrt::handle frameLatencyEvent;
	uint32_t frameLatencyFrame = 0;
	bool frameLatencyFrameValid = false;
	HWND hwnd = nullptr;
	WNDPROC originalWndProc = nullptr;
	std::atomic_bool windowMinimized{ false };
	std::atomic_bool windowStateDirty{ false };
	bool temporalFeaturesSuspended = false;
	std::atomic_bool deviceLost{ false };
	std::atomic_bool interopReady{ false };
};
