#include "DX12SwapChain.h"

#include <algorithm>
#include <limits>

#include "D3D12UIComposite.h"
#include "FidelityFX.h"
#include "OSD.h"
#include "Streamline.h"
#include "TaggedTextureDebug.h"
#include "Upscaling.h"

extern bool enbLoaded;

namespace
{
	LRESULT CALLBACK DX12SwapChainWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
	{
		auto* streamline = Streamline::GetSingleton();
		if (streamline->GetPCLStatsWindowMessage() != 0 && a_msg == streamline->GetPCLStatsWindowMessage()) {
			streamline->OnPCLStatsPing();
		}

		auto* swapChain = DX12SwapChain::GetSingleton();
		swapChain->OnWindowMessage(a_msg, a_wParam, a_lParam);
		return swapChain->CallOriginalWndProc(a_hwnd, a_msg, a_wParam, a_lParam);
	}

	double QueryDesktopRefreshHz(IDXGISwapChain* a_swapChain)
	{
		if (!a_swapChain) {
			return 0.0;
		}

		winrt::com_ptr<IDXGIOutput> output;
		if (FAILED(a_swapChain->GetContainingOutput(output.put())) || !output) {
			return 0.0;
		}

		DXGI_OUTPUT_DESC outputDesc{};
		if (FAILED(output->GetDesc(&outputDesc))) {
			return 0.0;
		}

		DEVMODEW displayMode{};
		displayMode.dmSize = sizeof(displayMode);
		if (!EnumDisplaySettingsW(outputDesc.DeviceName, ENUM_CURRENT_SETTINGS, &displayMode) || displayMode.dmDisplayFrequency == 0) {
			return 0.0;
		}

		return static_cast<double>(displayMode.dmDisplayFrequency);
	}

	const char* HResultName(HRESULT a_result)
	{
		switch (a_result) {
		case S_OK:
			return "S_OK";
		case DXGI_ERROR_DEVICE_HUNG:
			return "DXGI_ERROR_DEVICE_HUNG";
		case DXGI_ERROR_DEVICE_REMOVED:
			return "DXGI_ERROR_DEVICE_REMOVED";
		case DXGI_ERROR_DEVICE_RESET:
			return "DXGI_ERROR_DEVICE_RESET";
		case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
			return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
		case DXGI_ERROR_INVALID_CALL:
			return "DXGI_ERROR_INVALID_CALL";
		case DXGI_ERROR_ACCESS_DENIED:
			return "DXGI_ERROR_ACCESS_DENIED";
		case DXGI_ERROR_WAS_STILL_DRAWING:
			return "DXGI_ERROR_WAS_STILL_DRAWING";
		default:
			return "UNKNOWN";
		}
	}

	bool LogStreamlineProxy(Streamline* a_streamline, const char* a_name, IUnknown* a_interface)
	{
		if (!a_streamline || !a_streamline->slGetNativeInterface || !a_interface) {
			return false;
		}

		void* nativeInterface = nullptr;
		if (SL_FAILED(result, a_streamline->slGetNativeInterface(a_interface, &nativeInterface))) {
			logger::warn("[DX12SwapChain] slGetNativeInterface({}) failed: {}", a_name, magic_enum::enum_name(result));
			return false;
		}

		const auto isProxy = nativeInterface && nativeInterface != a_interface;
		logger::info(
			"[DX12SwapChain] Streamline proxy check {} proxy={} interface={} native={}",
			a_name,
			isProxy,
			static_cast<void*>(a_interface),
			nativeInterface);

		if (nativeInterface) {
			static_cast<IUnknown*>(nativeInterface)->Release();
		}

		return isProxy;
	}

	DXGI_SWAP_CHAIN_DESC1 MakeSwapChainDescFromWindow(const DXGI_SWAP_CHAIN_DESC& a_swapChainDesc, BOOL a_allowTearing)
	{
		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.BufferCount = kDX12FrameCount;
		desc.Width = a_swapChainDesc.BufferDesc.Width;
		desc.Height = a_swapChainDesc.BufferDesc.Height;
		desc.Format = a_swapChainDesc.BufferDesc.Format;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.SampleDesc.Count = 1;

		RECT clientRect{};
		if (a_swapChainDesc.OutputWindow && GetClientRect(a_swapChainDesc.OutputWindow, &clientRect)) {
			const auto clientWidth = static_cast<UINT>(std::max<LONG>(0, clientRect.right - clientRect.left));
			const auto clientHeight = static_cast<UINT>(std::max<LONG>(0, clientRect.bottom - clientRect.top));
			if (clientWidth > 0 && clientHeight > 0) {
				desc.Width = clientWidth;
				desc.Height = clientHeight;
			}
		}

		if (a_allowTearing) {
			desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		}

		return desc;
	}

}

D3D11D3D12SharedTexture::D3D11D3D12SharedTexture(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device)
{
	auto desc = a_desc;
	desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	DX::ThrowIfFailed(a_d3d11Device->CreateTexture2D(&desc, nullptr, resource11.put()));

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	DX::ThrowIfFailed(resource11->QueryInterface(IID_PPV_ARGS(dxgiResource.put())));

	HANDLE sharedHandle = nullptr;
	DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle));
	DX::ThrowIfFailed(a_d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(resource12.put())));
	CloseHandle(sharedHandle);
}

DXGISwapChainProxy::DXGISwapChainProxy(IDXGISwapChain4* a_swapChain)
{
	swapChain.copy_from(a_swapChain);
}

void DXGISwapChainProxy::SetSwapChain(IDXGISwapChain4* a_swapChain)
{
	swapChain.copy_from(a_swapChain);
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::AddRef()
{
	return ++refCount;
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::Release()
{
	const auto refs = --refCount;
	if (refs == 0) {
		delete this;
	}
	return refs;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
	if (!ppvObj) {
		return E_POINTER;
	}

	*ppvObj = nullptr;
	if (riid == __uuidof(IUnknown) ||
		riid == __uuidof(IDXGIObject) ||
		riid == __uuidof(IDXGIDeviceSubObject) ||
		riid == __uuidof(IDXGISwapChain) ||
		riid == __uuidof(IDXGISwapChain1) ||
		riid == __uuidof(IDXGISwapChain2) ||
		riid == __uuidof(IDXGISwapChain3) ||
		riid == __uuidof(IDXGISwapChain4)) {
		*ppvObj = static_cast<IDXGISwapChain4*>(this);
		AddRef();
		return S_OK;
	}

	return swapChain->QueryInterface(riid, ppvObj);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) { return swapChain->SetPrivateData(Name, DataSize, pData); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) { return swapChain->SetPrivateDataInterface(Name, pUnknown); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) { return swapChain->GetPrivateData(Name, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetParent(REFIID riid, void** ppParent) { return swapChain->GetParent(riid, ppParent); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDevice(REFIID riid, void** ppDevice) { return DX12SwapChain::GetSingleton()->GetDevice(riid, ppDevice); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present(UINT SyncInterval, UINT Flags) { return DX12SwapChain::GetSingleton()->Present(SyncInterval, Flags); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) { return DX12SwapChain::GetSingleton()->GetBuffer(Buffer, riid, ppSurface); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) { return DX12SwapChain::GetSingleton()->SetFullscreenState(Fullscreen, pTarget); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) { return swapChain->GetFullscreenState(pFullscreen, ppTarget); }

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
	if (!pDesc) {
		return E_POINTER;
	}

	DXGI_SWAP_CHAIN_DESC1 desc1{};
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
	HWND hwnd = nullptr;
	DX::ThrowIfFailed(GetDesc1(&desc1));
	std::ignore = GetFullscreenDesc(&fullscreenDesc);
	std::ignore = GetHwnd(&hwnd);

	pDesc->BufferDesc.Width = desc1.Width;
	pDesc->BufferDesc.Height = desc1.Height;
	pDesc->BufferDesc.RefreshRate = fullscreenDesc.RefreshRate;
	pDesc->BufferDesc.Format = desc1.Format;
	pDesc->BufferDesc.ScanlineOrdering = fullscreenDesc.ScanlineOrdering;
	pDesc->BufferDesc.Scaling = fullscreenDesc.Scaling;
	pDesc->SampleDesc = desc1.SampleDesc;
	pDesc->BufferUsage = desc1.BufferUsage;
	pDesc->BufferCount = desc1.BufferCount;
	pDesc->OutputWindow = hwnd;
	pDesc->Windowed = TRUE;
	pDesc->SwapEffect = desc1.SwapEffect;
	pDesc->Flags = desc1.Flags;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	return DX12SwapChain::GetSingleton()->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) { return DX12SwapChain::GetSingleton()->ResizeTarget(pNewTargetParameters); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetContainingOutput(IDXGIOutput** ppOutput) { return swapChain->GetContainingOutput(ppOutput); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) { return swapChain->GetFrameStatistics(pStats); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetLastPresentCount(UINT* pLastPresentCount) { return swapChain->GetLastPresentCount(pLastPresentCount); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) { return swapChain->GetDesc1(pDesc); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) { return swapChain->GetFullscreenDesc(pDesc); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetHwnd(HWND* pHwnd) { return swapChain->GetHwnd(pHwnd); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetCoreWindow(REFIID refiid, void** ppUnk) { return swapChain->GetCoreWindow(refiid, ppUnk); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS*) { return Present(SyncInterval, PresentFlags); }
BOOL STDMETHODCALLTYPE DXGISwapChainProxy::IsTemporaryMonoSupported() { return swapChain->IsTemporaryMonoSupported(); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) { return swapChain->GetRestrictToOutput(ppRestrictToOutput); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetBackgroundColor(const DXGI_RGBA* pColor) { return swapChain->SetBackgroundColor(pColor); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBackgroundColor(DXGI_RGBA* pColor) { return swapChain->GetBackgroundColor(pColor); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetRotation(DXGI_MODE_ROTATION Rotation) { return swapChain->SetRotation(Rotation); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetRotation(DXGI_MODE_ROTATION* pRotation) { return swapChain->GetRotation(pRotation); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetSourceSize(UINT Width, UINT Height) { return swapChain->SetSourceSize(Width, Height); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetSourceSize(UINT* pWidth, UINT* pHeight) { return swapChain->GetSourceSize(pWidth, pHeight); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetMaximumFrameLatency(UINT MaxLatency) { return swapChain->SetMaximumFrameLatency(MaxLatency); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetMaximumFrameLatency(UINT* pMaxLatency) { return swapChain->GetMaximumFrameLatency(pMaxLatency); }
HANDLE STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameLatencyWaitableObject() { return swapChain->GetFrameLatencyWaitableObject(); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) { return swapChain->SetMatrixTransform(pMatrix); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) { return swapChain->GetMatrixTransform(pMatrix); }
UINT STDMETHODCALLTYPE DXGISwapChainProxy::GetCurrentBackBufferIndex() { return swapChain->GetCurrentBackBufferIndex(); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) { return swapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport); }
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) { return swapChain->SetColorSpace1(ColorSpace); }

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
	return DX12SwapChain::GetSingleton()->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) { return swapChain->SetHDRMetaData(Type, Size, pMetaData); }

void DX12SwapChain::CreateD3D12Device(IDXGIAdapter* a_adapter, Streamline* a_streamline)
{
	DX::ThrowIfFailed(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(d3d12Device.put())));

	if (a_streamline && a_streamline->slSetD3DDevice) {
		if (SL_FAILED(result, a_streamline->slSetD3DDevice(d3d12Device.get()))) {
			logger::warn("[DX12SwapChain] slSetD3DDevice(D3D12) failed: {}", magic_enum::enum_name(result));
		}
	}

	ID3D12Device* deviceForQueue = d3d12Device.get();
	if (a_streamline && a_streamline->slUpgradeInterface) {
		if (SL_FAILED(result, a_streamline->slUpgradeInterface(reinterpret_cast<void**>(&deviceForQueue)))) {
			logger::warn("[DX12SwapChain] Could not upgrade D3D12 device for Streamline: {}", magic_enum::enum_name(result));
			deviceForQueue = d3d12Device.get();
		}
	}

	if (deviceForQueue == d3d12Device.get()) {
		proxyD3D12Device.copy_from(d3d12Device.get());
	} else {
		proxyD3D12Device.attach(deviceForQueue);
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

	DX::ThrowIfFailed(proxyD3D12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.put())));
	logger::info("[DX12SwapChain] D3D12 command queue created via {} device: queue={}", proxyD3D12Device.get() == d3d12Device.get() ? "native" : "Streamline proxy", static_cast<void*>(commandQueue.get()));
	LogStreamlineProxy(a_streamline, "d3d12Device", d3d12Device.get());
	LogStreamlineProxy(a_streamline, "deviceForQueue", proxyD3D12Device.get());
	LogStreamlineProxy(a_streamline, "commandQueue", commandQueue.get());

	for (auto& context : commandContexts) {
		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(context.allocator.put())));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator.get(), nullptr, IID_PPV_ARGS(context.list.put())));
		DX::ThrowIfFailed(context.list->Close());
	}
}

void DX12SwapChain::CreateSwapChain(IDXGIFactory5* a_dxgiFactory, const DXGI_SWAP_CHAIN_DESC& a_swapChainDesc, Streamline* a_streamline, bool a_useFidelityFXFrameGeneration)
{
	hwnd = a_swapChainDesc.OutputWindow;
	BOOL allowTearing = FALSE;
	std::ignore = a_dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));

	swapChainDesc = MakeSwapChainDescFromWindow(a_swapChainDesc, allowTearing);

	IDXGIFactory5* factoryForSwapChain = a_dxgiFactory;
	winrt::com_ptr<IDXGIFactory5> upgradedFactory;
	if (a_streamline && a_streamline->slUpgradeInterface) {
		if (SL_FAILED(result, a_streamline->slUpgradeInterface(reinterpret_cast<void**>(&factoryForSwapChain)))) {
			logger::warn("[DX12SwapChain] Could not upgrade DXGI factory for Streamline: {}", magic_enum::enum_name(result));
			factoryForSwapChain = a_dxgiFactory;
		}
	}

	if (factoryForSwapChain == a_dxgiFactory) {
		upgradedFactory.copy_from(a_dxgiFactory);
	} else {
		upgradedFactory.attach(factoryForSwapChain);
	}

	fidelityFXFrameGenerationSwapChainAllowed = a_useFidelityFXFrameGeneration;
	if (fidelityFXFrameGenerationSwapChainAllowed) {
		logger::info("[DX12SwapChain] FidelityFX frame generation swapchain will be created during swapchain initialization");
	}

	if (!swapChain) {
		if (fidelityFXFrameGenerationSwapChainAllowed) {
			DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
			fullscreenDesc.RefreshRate = a_swapChainDesc.BufferDesc.RefreshRate;
			fullscreenDesc.ScanlineOrdering = a_swapChainDesc.BufferDesc.ScanlineOrdering;
			fullscreenDesc.Scaling = a_swapChainDesc.BufferDesc.Scaling;
			fullscreenDesc.Windowed = a_swapChainDesc.Windowed;

			IDXGISwapChain4* fidelityFXSwapChain = nullptr;
			if (FidelityFX::GetSingleton()->CreateFrameGenerationSwapChainForHwnd(
					upgradedFactory.get(),
					a_swapChainDesc.OutputWindow,
					&swapChainDesc,
					&fullscreenDesc,
					commandQueue.get(),
					&fidelityFXSwapChain) &&
				fidelityFXSwapChain) {
				swapChain.attach(fidelityFXSwapChain);
				logger::info("[DX12SwapChain] FidelityFX frame generation swapchain created for hwnd: swapchain={}", static_cast<void*>(swapChain.get()));
			} else {
				if (fidelityFXSwapChain) {
					fidelityFXSwapChain->Release();
				}
				fidelityFXFrameGenerationSwapChainAllowed = false;
				logger::warn("[DX12SwapChain] FidelityFX frame generation swapchain creation failed; falling back to a regular D3D12 swapchain");
			}
		}

		if (!swapChain) {
			winrt::com_ptr<IDXGISwapChain1> swapChain1;
			DX::ThrowIfFailed(upgradedFactory->CreateSwapChainForHwnd(commandQueue.get(), a_swapChainDesc.OutputWindow, &swapChainDesc, nullptr, nullptr, swapChain1.put()));
			DX::ThrowIfFailed(swapChain1->QueryInterface(IID_PPV_ARGS(swapChain.put())));
		}
	}

	logger::info("[DX12SwapChain] Swapchain created via {} factory: swapchain={}", upgradedFactory.get() == a_dxgiFactory ? "native" : "Streamline proxy", static_cast<void*>(swapChain.get()));
	LogStreamlineProxy(a_streamline, "dxgiFactory", a_dxgiFactory);
	LogStreamlineProxy(a_streamline, "factoryForSwapChain", upgradedFactory.get());
	auto swapChainIsStreamlineProxy = LogStreamlineProxy(a_streamline, "swapChain", swapChain.get());
	if (!swapChainIsStreamlineProxy && a_streamline && a_streamline->slUpgradeInterface) {
		IDXGISwapChain* upgradedSwapChain = swapChain.get();
		if (SL_FAILED(result, a_streamline->slUpgradeInterface(reinterpret_cast<void**>(&upgradedSwapChain)))) {
			logger::warn("[DX12SwapChain] Could not upgrade swapchain for Streamline: {}", magic_enum::enum_name(result));
		} else if (upgradedSwapChain && upgradedSwapChain != swapChain.get()) {
			winrt::com_ptr<IDXGISwapChain> upgradedSwapChainOwner;
			upgradedSwapChainOwner.attach(upgradedSwapChain);
			winrt::com_ptr<IDXGISwapChain4> upgradedSwapChain4;
			DX::ThrowIfFailed(upgradedSwapChainOwner->QueryInterface(IID_PPV_ARGS(upgradedSwapChain4.put())));
			swapChain = upgradedSwapChain4;
			logger::info("[DX12SwapChain] Swapchain explicitly upgraded for Streamline: swapchain={}", static_cast<void*>(swapChain.get()));
			swapChainIsStreamlineProxy = LogStreamlineProxy(a_streamline, "swapChain.afterUpgrade", swapChain.get());
		}
	}
	if (!swapChainIsStreamlineProxy) {
		logger::warn("[DX12SwapChain] D3D12 swapchain is not a Streamline proxy; DLSS-G Present interception may not run on this swapchain");
	}
	RefreshBackBuffers();
	frameIndex = swapChain->GetCurrentBackBufferIndex();
	swapChainProxy = new DXGISwapChainProxy(swapChain.get());
	desktopRefreshHz = QueryDesktopRefreshHz(swapChain.get());
	if (desktopRefreshHz <= 0.0 &&
		a_swapChainDesc.BufferDesc.RefreshRate.Numerator != 0 &&
		a_swapChainDesc.BufferDesc.RefreshRate.Denominator != 0) {
		desktopRefreshHz =
			static_cast<double>(a_swapChainDesc.BufferDesc.RefreshRate.Numerator) /
			static_cast<double>(a_swapChainDesc.BufferDesc.RefreshRate.Denominator);
	}

	logger::info(
		"[DX12SwapChain] Created D3D12 Streamline swapchain {}x{} format={} buffers={} flags={} desktopRefreshHz={:.3f}",
		swapChainDesc.Width,
		swapChainDesc.Height,
		static_cast<uint32_t>(swapChainDesc.Format),
		swapChainDesc.BufferCount,
		swapChainDesc.Flags,
		desktopRefreshHz);

	InstallWndProcHook(hwnd);
}

void DX12SwapChain::InstallWndProcHook(HWND a_hwnd)
{
	if (!a_hwnd || originalWndProc) {
		return;
	}

	SetLastError(0);
	const auto previous = SetWindowLongPtrW(a_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DX12SwapChainWndProc));
	if (previous == 0 && GetLastError() != 0) {
		logger::warn("[DX12SwapChain] Could not install PCL stats window proc hook: {}", GetLastError());
		return;
	}

	originalWndProc = reinterpret_cast<WNDPROC>(previous);
	logger::info("[DX12SwapChain] Installed PCL stats window proc hook");
}

LRESULT DX12SwapChain::CallOriginalWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam) const
{
	if (originalWndProc) {
		return CallWindowProcW(originalWndProc, a_hwnd, a_msg, a_wParam, a_lParam);
	}
	return DefWindowProcW(a_hwnd, a_msg, a_wParam, a_lParam);
}

void DX12SwapChain::OnWindowMessage(UINT a_msg, WPARAM a_wParam, LPARAM)
{
	switch (a_msg) {
	case WM_SIZE:
		windowMinimized.store(a_wParam == SIZE_MINIMIZED, std::memory_order_release);
		windowStateDirty.store(true, std::memory_order_release);
		break;
	case WM_DISPLAYCHANGE:
	case WM_DPICHANGED:
		windowStateDirty.store(true, std::memory_order_release);
		break;
	default:
		break;
	}
}

void DX12SwapChain::SuspendTemporalFeatures(const char* a_reason)
{
	if (temporalFeaturesSuspended || deviceLost) {
		return;
	}

	auto* streamline = Streamline::GetSingleton();
	if (streamline->NeedsDLSSGPresentSafety()) {
		streamline->RequestDLSSGDisable();
	}
	if (!WaitForGPUIdle()) {
		return;
	}
	streamline->ApplyPendingDLSSGDisable();
	streamline->SuspendDLSSNR();
	Upscaling::GetSingleton()->OnD3D12TemporalSuspend();
	presentOverrideFinalColor = nullptr;
	for (auto& context : commandContexts) {
		context.retainedPresentOverride = nullptr;
	}
	temporalFeaturesSuspended = true;
	logger::info("[DX12SwapChain] Suspended temporal features reason={}", a_reason ? a_reason : "unknown");
}

void DX12SwapChain::ResumeTemporalFeatures()
{
	if (!temporalFeaturesSuspended || deviceLost) {
		return;
	}

	Streamline::GetSingleton()->ResumeDLSSNR();
	temporalFeaturesSuspended = false;
	logger::info("[DX12SwapChain] Resumed temporal features; next evaluation will reset history");
}

void DX12SwapChain::ProcessWindowStateTransition()
{
	const auto stateChanged = windowStateDirty.exchange(false, std::memory_order_acq_rel);
	const auto unavailable = IsWindowUnavailable();
	if (unavailable) {
		SuspendTemporalFeatures("minimized");
		return;
	}
	if (temporalFeaturesSuspended && stateChanged) {
		ResumeTemporalFeatures();
	}
}

void DX12SwapChain::ReleaseResizeDependentResources()
{
	presentOverrideFinalColor = nullptr;
	for (auto& context : commandContexts) {
		context.presentStaging = nullptr;
		context.retainedPresentOverride = nullptr;
		context.fenceValue = 0;
	}
	for (auto& backBuffer : swapChainBuffers) {
		backBuffer = nullptr;
	}
	swapChainBufferProxy = nullptr;
	swapChainBufferProxyENB = nullptr;
	frameSlotFenceValues.fill(0);
}

void DX12SwapChain::RestoreResizeDependentResources(const char* a_context)
{
	try {
		std::ignore = swapChain->GetDesc1(&swapChainDesc);
		RefreshBackBuffers();
		RecreateInteropTextures();
		frameIndex = swapChain->GetCurrentBackBufferIndex();
	} catch (const std::exception& e) {
		logger::error("[DX12SwapChain] {} resource restoration failed: {}", a_context, e.what());
	}
}

HRESULT DX12SwapChain::ResizeBuffersInternal(bool a_useResizeBuffers1, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags, const UINT* a_creationNodeMask)
{
	const auto operation = a_useResizeBuffers1 ? "ResizeBuffers1" : "ResizeBuffers";
	if (!swapChain || deviceLost) {
		return DXGI_ERROR_DEVICE_REMOVED;
	}

	SuspendTemporalFeatures(operation);
	if (deviceLost || !WaitForGPUIdle()) {
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	Streamline::GetSingleton()->DestroyDLSSResources();
	ReleaseResizeDependentResources();

	const auto format = a_format == DXGI_FORMAT_UNKNOWN ? swapChainDesc.Format : a_format;
	const auto resizeFlags = a_flags | swapChainDesc.Flags;
	HRESULT result = S_OK;
	if (a_useResizeBuffers1) {
		std::array<UINT, kDX12FrameCount> nodeMasks{};
		std::array<IUnknown*, kDX12FrameCount> presentQueues{};
		for (std::size_t i = 0; i < presentQueues.size(); ++i) {
			nodeMasks[i] = a_creationNodeMask ? a_creationNodeMask[0] : 0;
			presentQueues[i] = commandQueue.get();
		}
		result = swapChain->ResizeBuffers1(kDX12FrameCount, a_width, a_height, format, resizeFlags, nodeMasks.data(), presentQueues.data());
	} else {
		result = swapChain->ResizeBuffers(kDX12FrameCount, a_width, a_height, format, resizeFlags);
	}

	if (FAILED(result)) {
		logger::error("[DX12SwapChain] {} failed result=0x{:08X} width={} height={} format={} flags=0x{:X}", operation, static_cast<uint32_t>(result), a_width, a_height, static_cast<uint32_t>(format), resizeFlags);
		RestoreResizeDependentResources(operation);
		return result;
	}

	RestoreResizeDependentResources(operation);
	if (!swapChainBufferProxy && !swapChainBufferProxyENB) {
		return DXGI_ERROR_DEVICE_RESET;
	}
	Streamline::GetSingleton()->RequestTemporalReset();
	windowStateDirty.store(true, std::memory_order_release);
	logger::info("[DX12SwapChain] {} completed {}x{} format={} buffers={}", operation, swapChainDesc.Width, swapChainDesc.Height, static_cast<uint32_t>(swapChainDesc.Format), swapChainDesc.BufferCount);
	return S_OK;
}

HRESULT DX12SwapChain::ResizeBuffers(UINT, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags)
{
	return ResizeBuffersInternal(false, a_width, a_height, a_format, a_flags, nullptr);
}

HRESULT DX12SwapChain::ResizeBuffers1(UINT, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags, const UINT* a_creationNodeMask, IUnknown* const*)
{
	return ResizeBuffersInternal(true, a_width, a_height, a_format, a_flags, a_creationNodeMask);
}

HRESULT DX12SwapChain::SetFullscreenState(BOOL a_fullscreen, IDXGIOutput* a_target)
{
	if (!swapChain || deviceLost) {
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	SuspendTemporalFeatures("SetFullscreenState");
	if (deviceLost) {
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	const auto result = swapChain->SetFullscreenState(a_fullscreen, a_target);
	windowStateDirty.store(true, std::memory_order_release);
	return result;
}

HRESULT DX12SwapChain::ResizeTarget(const DXGI_MODE_DESC* a_newTargetParameters)
{
	if (!a_newTargetParameters) {
		return E_INVALIDARG;
	}
	if (!swapChain || deviceLost) {
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	SuspendTemporalFeatures("ResizeTarget");
	if (deviceLost) {
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	const auto result = swapChain->ResizeTarget(a_newTargetParameters);
	windowStateDirty.store(true, std::memory_order_release);
	return result;
}

void DX12SwapChain::CreateInterop()
{
	HANDLE sharedFenceHandle = nullptr;
	DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d3d12Fence.put())));
	DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(commandFence.put())));
	DX::ThrowIfFailed(d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
	DX::ThrowIfFailed(d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(d3d11Fence.put())));
	CloseHandle(sharedFenceHandle);
	RecreateInteropTextures();
}

void DX12SwapChain::RecreateInteropTextures()
{
	D3D11_TEXTURE2D_DESC textureDesc{};
	textureDesc.Width = swapChainDesc.Width;
	textureDesc.Height = swapChainDesc.Height;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = swapChainDesc.Format;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	if (enbLoaded) {
		swapChainBufferProxyENB = std::make_unique<D3D11D3D12SharedTexture>(textureDesc, d3d11Device.get(), d3d12Device.get());
		swapChainBufferProxy = nullptr;
	} else {
		winrt::com_ptr<ID3D11Texture2D> proxyTexture;
		DX::ThrowIfFailed(d3d11Device->CreateTexture2D(&textureDesc, nullptr, proxyTexture.put()));
		swapChainBufferProxy = std::make_unique<Texture2D>(proxyTexture.detach());
		swapChainBufferProxyENB = nullptr;
	}
	for (auto& context : commandContexts) {
		context.retainedPresentOverride = nullptr;
		context.presentStaging = std::make_unique<D3D11D3D12SharedTexture>(textureDesc, d3d11Device.get(), d3d12Device.get());
	}
}

DX12SwapChain::CommandContext& DX12SwapChain::AcquireCommandContext()
{
	const auto completedValue = commandFence ? commandFence->GetCompletedValue() : 0;
	if (completedValue == std::numeric_limits<UINT64>::max()) {
		deviceLost = true;
		DX::ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
	}
	for (UINT i = 0; i < std::size(commandContexts); ++i) {
		const auto contextIndex = (nextCommandContext + i) % std::size(commandContexts);
		auto& context = commandContexts[contextIndex];
		if (context.fenceValue == 0 || completedValue >= context.fenceValue) {
			nextCommandContext = static_cast<UINT>((contextIndex + 1) % std::size(commandContexts));
			context.index = static_cast<UINT>(contextIndex);
			context.fenceValue = 0;
			context.retainedPresentOverride = nullptr;
			DX::ThrowIfFailed(context.allocator->Reset());
			DX::ThrowIfFailed(context.list->Reset(context.allocator.get(), nullptr));
			return context;
		}
	}

	UINT waitContextIndex = nextCommandContext;
	auto waitValue = std::numeric_limits<UINT64>::max();
	for (UINT i = 0; i < std::size(commandContexts); ++i) {
		if (const auto value = commandContexts[i].fenceValue; value != 0 && value < waitValue) {
			waitContextIndex = i;
			waitValue = value;
		}
	}

	if (!WaitForCommandFence(waitValue)) {
		DX::ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
	}
	nextCommandContext = static_cast<UINT>((waitContextIndex + 1) % std::size(commandContexts));
	auto& context = commandContexts[waitContextIndex];
	context.index = waitContextIndex;
	context.fenceValue = 0;
	context.retainedPresentOverride = nullptr;
	DX::ThrowIfFailed(context.allocator->Reset());
	DX::ThrowIfFailed(context.list->Reset(context.allocator.get(), nullptr));
	return context;
}

void DX12SwapChain::ExecuteCommandContext(CommandContext& a_context)
{
	DX::ThrowIfFailed(a_context.list->Close());
	ID3D12CommandList* lists[] = { a_context.list.get() };
	commandQueue->ExecuteCommandLists(static_cast<UINT>(std::size(lists)), lists);

	const auto signalValue = commandFenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(commandFence.get(), signalValue));
	a_context.fenceValue = signalValue;
}

bool DX12SwapChain::FenceFrameSlotAfterPresent(UINT a_frameIndex, CommandContext* a_context)
{
	if (a_frameIndex >= std::size(frameSlotFenceValues) || !commandQueue || !commandFence) {
		return false;
	}
	const auto signalValue = commandFenceValue++;
	const auto result = commandQueue->Signal(commandFence.get(), signalValue);
	if (FAILED(result)) {
		deviceLost = true;
		logger::error("[DX12SwapChain] Post-Present signal failed result=0x{:08X}", static_cast<uint32_t>(result));
		return false;
	}
	frameSlotFenceValues[a_frameIndex] = signalValue;
	if (a_context) {
		a_context->fenceValue = signalValue;
	}
	return true;
}

bool DX12SwapChain::WaitForCommandFence(UINT64 a_value)
{
	if (!commandFence || a_value == 0) {
		return true;
	}
	const auto completedValue = commandFence->GetCompletedValue();
	if (completedValue == std::numeric_limits<UINT64>::max()) {
		deviceLost = true;
		logger::error("[DX12SwapChain] Command fence reports device removal while waiting for {}", a_value);
		return false;
	}
	if (completedValue >= a_value) {
		return true;
	}

	if (!commandFenceEvent) {
		commandFenceEvent.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		if (!commandFenceEvent) {
			DX::ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}
	}

	const auto eventResult = commandFence->SetEventOnCompletion(a_value, commandFenceEvent.get());
	if (FAILED(eventResult)) {
		deviceLost = true;
		logger::error("[DX12SwapChain] SetEventOnCompletion failed result=0x{:08X} value={}", static_cast<uint32_t>(eventResult), a_value);
		return false;
	}
	const auto waitResult = WaitForSingleObjectEx(commandFenceEvent.get(), 5000, FALSE);
	if (waitResult != WAIT_OBJECT_0) {
		const auto removedReason = d3d12Device ? d3d12Device->GetDeviceRemovedReason() : E_FAIL;
		deviceLost = FAILED(removedReason);
		logger::error(
			"[DX12SwapChain] Command fence wait failed waitResult=0x{:08X} value={} completed={} removed=0x{:08X}",
			waitResult,
			a_value,
			commandFence->GetCompletedValue(),
			static_cast<uint32_t>(removedReason));
		return false;
	}
	return true;
}

bool DX12SwapChain::WaitForFrameSlot(UINT a_frameIndex)
{
	if (a_frameIndex >= std::size(frameSlotFenceValues)) {
		return false;
	}

	const auto waitValue = frameSlotFenceValues[a_frameIndex];
	if (waitValue == 0) {
		return true;
	}

	if (!WaitForCommandFence(waitValue)) {
		return false;
	}
	frameSlotFenceValues[a_frameIndex] = 0;
	return true;
}

bool DX12SwapChain::WaitForGPUIdle()
{
	if (!commandQueue || !commandFence || deviceLost) {
		return false;
	}

	const auto signalValue = commandFenceValue++;
	const auto signalResult = commandQueue->Signal(commandFence.get(), signalValue);
	if (FAILED(signalResult)) {
		deviceLost = true;
		logger::error("[DX12SwapChain] GPU-idle signal failed result=0x{:08X}", static_cast<uint32_t>(signalResult));
		return false;
	}
	if (!WaitForCommandFence(signalValue)) {
		return false;
	}
	frameSlotFenceValues.fill(0);
	return true;
}

void DX12SwapChain::SetD3D11Device(ID3D11Device* a_d3d11Device)
{
	DX::ThrowIfFailed(a_d3d11Device->QueryInterface(IID_PPV_ARGS(d3d11Device.put())));
}

void DX12SwapChain::SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context)
{
	DX::ThrowIfFailed(a_d3d11Context->QueryInterface(IID_PPV_ARGS(d3d11Context.put())));
}

HRESULT DX12SwapChain::GetBuffer(UINT, REFIID a_riid, void** a_surface)
{
	if (!a_surface) {
		return E_POINTER;
	}

	if (swapChainBufferProxyENB) {
		return swapChainBufferProxyENB->resource11->QueryInterface(a_riid, a_surface);
	}

	if (!swapChainBufferProxy) {
		return E_POINTER;
	}

	return swapChainBufferProxy->resource->QueryInterface(a_riid, a_surface);
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	if (deviceLost) {
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	if (!IsReady()) {
		return DXGI_ERROR_INVALID_CALL;
	}

	auto streamline = Streamline::GetSingleton();
	auto upscaling = Upscaling::GetSingleton();
	const auto osdEnabled = upscaling->settings.osdMode != 0;
	ProcessWindowStateTransition();
	if (IsWindowUnavailable()) {
		const auto presentedFrameIndex = frameIndex;
		const auto result = swapChain->Present(0, Flags & ~DXGI_PRESENT_ALLOW_TEARING);
		const auto removedReason = d3d12Device ? d3d12Device->GetDeviceRemovedReason() : S_OK;
		if (FAILED(removedReason)) {
			deviceLost = true;
		} else if (!FenceFrameSlotAfterPresent(presentedFrameIndex)) {
			return DXGI_ERROR_DEVICE_REMOVED;
		}
		if (SUCCEEDED(result) && !deviceLost) {
			frameIndex = swapChain->GetCurrentBackBufferIndex();
		}
		return result;
	}
	const auto dlssgPresentSafety = streamline->NeedsDLSSGPresentSafety();

	auto& commandContext = AcquireCommandContext();
	auto* commandList = commandContext.list.get();
	auto* presentStaging = commandContext.presentStaging.get();
	if (!presentStaging) {
		return DXGI_ERROR_INVALID_CALL;
	}

	if (swapChainBufferProxyENB) {
		d3d11Context->CopyResource(presentStaging->resource11.get(), swapChainBufferProxyENB->resource11.get());
	} else {
		d3d11Context->CopyResource(presentStaging->resource11.get(), swapChainBufferProxy->resource.get());
	}
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	++fenceValue;

	const auto presentedFrameIndex = frameIndex;
	auto destination = swapChainBuffers[presentedFrameIndex].get();
	auto copySource = presentStaging->resource12.get();
	commandContext.retainedPresentOverride = std::move(presentOverrideFinalColor);
	auto overrideFinalColor = commandContext.retainedPresentOverride.get();
	const bool usePresentOverride = overrideFinalColor != nullptr;

	D3D12_RESOURCE_BARRIER beforeCopy[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(
			copySource,
			D3D12_RESOURCE_STATE_COMMON,
			usePresentOverride ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
	};
	commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeCopy)), beforeCopy);
	D3D12_RESOURCE_STATES destinationState = D3D12_RESOURCE_STATE_COPY_DEST;
	if (usePresentOverride) {
		D3D12_RESOURCE_BARRIER beforeComposite[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
			CD3DX12_RESOURCE_BARRIER::Transition(overrideFinalColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		};
		commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeComposite)), beforeComposite);
		D3D12UIComposite::GetSingleton()->Render(
			d3d12Device.get(),
			commandList,
			destination,
			overrideFinalColor,
			copySource,
			swapChainDesc.Format,
			swapChainDesc.Width,
			swapChainDesc.Height,
			commandContext.index,
			static_cast<uint32_t>(std::size(commandContexts)));
		auto afterComposite = CD3DX12_RESOURCE_BARRIER::Transition(overrideFinalColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		commandList->ResourceBarrier(1, &afterComposite);
		destinationState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	} else {
		commandList->CopyResource(destination, copySource);
	}
	auto afterSourceCopy = CD3DX12_RESOURCE_BARRIER::Transition(
		copySource,
		usePresentOverride ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_COMMON);
	commandList->ResourceBarrier(1, &afterSourceCopy);

	if (upscaling->IsFrameGenerationActive() || dlssgPresentSafety) {
		upscaling->TagDLSSGInputs(commandList, presentedFrameIndex);
	}
	auto fidelityFX = FidelityFX::GetSingleton();
	if (fidelityFX->IsFrameGenerationEnabled() &&
		!upscaling->IsFSRFrameGenerationActive()) {
		const auto desc = destination->GetDesc();
		const auto displaySize = float2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
		fidelityFX->DisableFrameGeneration(
			d3d12Device.get(),
			commandList,
			swapChain.get(),
			displaySize,
			desc.Format);
	}

	const auto taggedTextureDebug = upscaling->settings.taggedTextureDebug != 0;
	static bool wasOSDEnabled = false;
	if (!osdEnabled && wasOSDEnabled) {
		OSD::GetSingleton()->Reset();
	}
	wasOSDEnabled = osdEnabled;
	if (osdEnabled || taggedTextureDebug) {
		if (destinationState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
			auto beforeOSD = CD3DX12_RESOURCE_BARRIER::Transition(destination, destinationState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			commandList->ResourceBarrier(1, &beforeOSD);
			destinationState = D3D12_RESOURCE_STATE_RENDER_TARGET;
		}
		if (taggedTextureDebug) {
			ID3D12Resource* color = nullptr;
			ID3D12Resource* depth = nullptr;
			ID3D12Resource* motionVectors = nullptr;
			upscaling->GetTaggedTextureDebugResources(presentedFrameIndex, color, depth, motionVectors);
			if (color && depth && motionVectors) {
				D3D12_RESOURCE_BARRIER beforeDebug[] = {
					CD3DX12_RESOURCE_BARRIER::Transition(color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(copySource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
				};
				commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeDebug)), beforeDebug);
				TaggedTextureDebug::GetSingleton()->Render(
					d3d12Device.get(),
					commandList,
					destination,
					color,
					depth,
					motionVectors,
					copySource,
					swapChainDesc.Format,
					swapChainDesc.Width,
					swapChainDesc.Height,
					commandContext.index,
					static_cast<uint32_t>(std::size(commandContexts)));
				D3D12_RESOURCE_BARRIER afterDebug[] = {
					CD3DX12_RESOURCE_BARRIER::Transition(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(copySource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
				};
				commandList->ResourceBarrier(static_cast<UINT>(std::size(afterDebug)), afterDebug);
			}
		}
		if (osdEnabled) {
			OSD::GetSingleton()->Render(
				d3d12Device.get(),
				commandList,
				destination,
				presentedFrameIndex,
				swapChainDesc.Format,
				swapChainDesc.Width,
				swapChainDesc.Height);
		}
		auto afterOSD = CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		commandList->ResourceBarrier(1, &afterOSD);
	} else {
		auto afterCopy = CD3DX12_RESOURCE_BARRIER::Transition(destination, destinationState, D3D12_RESOURCE_STATE_PRESENT);
		commandList->ResourceBarrier(1, &afterCopy);
	}

	ExecuteCommandContext(commandContext);

	const auto fidelityFXFrameGenerationActive = upscaling->IsFSRFrameGenerationActive();
	const auto presentSyncInterval = (dlssgPresentSafety || fidelityFXFrameGenerationActive) ? 0u : SyncInterval;
	const auto presentFlags = dlssgPresentSafety ? (Flags & ~DXGI_PRESENT_ALLOW_TEARING) : Flags;
	const auto emitPresentMarkers = streamline->NeedsPresentMarkers();
	if (emitPresentMarkers) {
		streamline->OnPresentStart();
	}
	const auto result = swapChain->Present(presentSyncInterval, presentFlags);
	if (emitPresentMarkers) {
		streamline->OnPresentEnd(result, false);
	}
	if (SUCCEEDED(result)) {
		// Streamline may enqueue work while intercepting Present. Signal after it
		// returns so per-frame shared resources stay alive until that work finishes.
		if (!FenceFrameSlotAfterPresent(presentedFrameIndex, &commandContext)) {
			return DXGI_ERROR_DEVICE_REMOVED;
		}

		streamline->ApplyPendingDLSSGDisable();
		streamline->OnDLSSGPresentComplete();
	}
	if (FAILED(result)) {
		const auto d3d12RemovedReason = d3d12Device ? d3d12Device->GetDeviceRemovedReason() : S_OK;
		HRESULT d3d11RemovedReason = S_OK;
		if (d3d11Device) {
			winrt::com_ptr<ID3D11Device> d3d11Base;
			if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(d3d11Base.put())))) {
				d3d11RemovedReason = d3d11Base->GetDeviceRemovedReason();
			}
		}

		logger::error(
			"[DX12SwapChain] Present failed result=0x{:08X}({}) d3d12Removed=0x{:08X}({}) d3d11Removed=0x{:08X}({}) sync={} flags=0x{:X} frameIndex={} queue={} swapchain={} thread={}",
			static_cast<uint32_t>(result),
			HResultName(result),
			static_cast<uint32_t>(d3d12RemovedReason),
			HResultName(d3d12RemovedReason),
			static_cast<uint32_t>(d3d11RemovedReason),
			HResultName(d3d11RemovedReason),
			presentSyncInterval,
			presentFlags,
			presentedFrameIndex,
			static_cast<void*>(commandQueue.get()),
			static_cast<void*>(swapChain.get()),
			GetCurrentThreadId());

		if (FAILED(d3d12RemovedReason)) {
			deviceLost = true;
		} else {
			// Present interception can submit work even when DXGI reports a transient
			// mode-change failure. Fence that work before any frame-slot reuse.
			if (FenceFrameSlotAfterPresent(presentedFrameIndex, &commandContext)) {
				SuspendTemporalFeatures("Present failure");
				windowStateDirty.store(true, std::memory_order_release);
			}
		}
	}
	if (FAILED(result)) {
		return result;
	}

	const auto nextFrameIndex = swapChain->GetCurrentBackBufferIndex();

	if (dlssgPresentSafety || upscaling->IsFrameGenerationActive()) {
		streamline->QueryDLSSGState("post-wait");
	}

	static bool loggedPresentAdjustment = false;
	if (dlssgPresentSafety && !loggedPresentAdjustment && (presentSyncInterval != SyncInterval || presentFlags != Flags)) {
		logger::info(
			"[DX12SwapChain] DLSS-G adjusted present sync {}->{} flags 0x{:X}->0x{:X}",
			SyncInterval,
			presentSyncInterval,
			Flags,
			presentFlags);
		loggedPresentAdjustment = true;
	}

	frameIndex = nextFrameIndex;
	return S_OK;
}

DX12SwapChain::D3D12EvaluationResult DX12SwapChain::EvaluateD3D12WorkForCurrentFrame(bool a_evaluateDLSS, bool a_evaluateFSR, bool a_evaluateFSRFrameGeneration, bool a_waitForD3D11Consumption)
{
	D3D12EvaluationResult result{};
	if (!IsReady() || deviceLost || temporalFeaturesSuspended || IsWindowUnavailable()) {
		return result;
	}

	if (a_evaluateFSRFrameGeneration && !EnsureFidelityFXFrameGenerationSwapChain()) {
		a_evaluateFSRFrameGeneration = false;
	}

	if (!a_evaluateDLSS && !a_evaluateFSR && !a_evaluateFSRFrameGeneration) {
		return result;
	}

	const auto evaluationFrameIndex = frameIndex;
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	++fenceValue;

	auto& commandContext = AcquireCommandContext();
	auto* commandList = commandContext.list.get();

	result = EvaluateD3D12WorkOnCommandList(commandList, evaluationFrameIndex, a_evaluateDLSS, a_evaluateFSR, a_evaluateFSRFrameGeneration);

	DX::ThrowIfFailed(commandList->Close());

	if (!result.Any()) {
		return result;
	}

	ID3D12CommandList* lists[] = { commandList };
	commandQueue->ExecuteCommandLists(static_cast<UINT>(std::size(lists)), lists);
	const auto signalValue = commandFenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(commandFence.get(), signalValue));
	commandContext.fenceValue = signalValue;
	frameSlotFenceValues[evaluationFrameIndex] = signalValue;

	if (a_waitForD3D11Consumption) {
		DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));
		DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));
		++fenceValue;
	}
	return result;
}

DX12SwapChain::D3D12EvaluationResult DX12SwapChain::EvaluateD3D12WorkOnCommandList(ID3D12GraphicsCommandList* a_commandList, UINT a_frameIndex, bool a_evaluateDLSS, bool a_evaluateFSR, bool a_evaluateFSRFrameGeneration)
{
	D3D12EvaluationResult result{};
	if (!a_commandList) {
		return result;
	}

	auto* upscaling = Upscaling::GetSingleton();
	if (a_evaluateFSR) {
		result.fsr = upscaling->EvaluateD3D12FSR(a_commandList, a_frameIndex);
	}
	if (a_evaluateDLSS) {
		result.dlss = upscaling->EvaluateD3D12DLSS(a_commandList, a_frameIndex);
	}

	const bool upscalerRequested = a_evaluateFSR || a_evaluateDLSS;
	const bool upscalerSucceeded = (!a_evaluateFSR || result.fsr) && (!a_evaluateDLSS || result.dlss);
	if (a_evaluateFSRFrameGeneration && (!upscalerRequested || upscalerSucceeded)) {
		result.fsrFrameGeneration = upscaling->EvaluateFSRFrameGeneration(a_commandList, a_frameIndex);
	}

	return result;
}

bool DX12SwapChain::EvaluateD3D12DLSSForCurrentFrame()
{
	return EvaluateD3D12WorkForCurrentFrame(true, false, false).dlss;
}

bool DX12SwapChain::EvaluateD3D12FSRForCurrentFrame()
{
	return EvaluateD3D12WorkForCurrentFrame(false, true, false).fsr;
}

bool DX12SwapChain::EvaluateFSRFrameGenerationForCurrentFrame()
{
	return EvaluateD3D12WorkForCurrentFrame(false, false, true).fsrFrameGeneration;
}

void DX12SwapChain::SetPresentOverride(ID3D12Resource* a_finalColor)
{
	presentOverrideFinalColor.copy_from(a_finalColor);
}

bool DX12SwapChain::EnsureFidelityFXFrameGenerationSwapChain()
{
	if (!IsReady() || !fidelityFXFrameGenerationSwapChainAllowed) {
		return false;
	}

	auto* fidelityFX = FidelityFX::GetSingleton();
	if (fidelityFX->IsFrameGenerationSwapChainActive()) {
		return true;
	}

	auto* originalSwapChain = swapChain.get();
	if (!originalSwapChain) {
		return false;
	}

	WaitForGPUIdle();

	originalSwapChain->AddRef();
	IDXGISwapChain4* wrappedSwapChain = originalSwapChain;
	const auto created = fidelityFX->CreateFrameGenerationSwapChain(&wrappedSwapChain, commandQueue.get());
	if (created && wrappedSwapChain) {
		if (wrappedSwapChain != originalSwapChain) {
			winrt::com_ptr<IDXGISwapChain4> wrappedOwner;
			wrappedOwner.attach(wrappedSwapChain);
			swapChain = wrappedOwner;
			if (swapChainProxy) {
				swapChainProxy->SetSwapChain(swapChain.get());
			}
			RefreshBackBuffers();
			frameIndex = swapChain->GetCurrentBackBufferIndex();
		}

		originalSwapChain->Release();
		logger::info("[DX12SwapChain] FidelityFX frame generation swapchain enabled at runtime");
		return true;
	}

	if (wrappedSwapChain && wrappedSwapChain != originalSwapChain) {
		wrappedSwapChain->Release();
	}
	originalSwapChain->Release();
	logger::warn("[DX12SwapChain] FidelityFX frame generation swapchain could not be enabled");
	return false;
}

HRESULT DX12SwapChain::GetDevice(REFIID a_riid, void** a_device)
{
	if (!a_device) {
		return E_POINTER;
	}

	if (a_riid == __uuidof(ID3D11Device) ||
		a_riid == __uuidof(ID3D11Device1) ||
		a_riid == __uuidof(ID3D11Device2) ||
		a_riid == __uuidof(ID3D11Device3) ||
		a_riid == __uuidof(ID3D11Device4) ||
		a_riid == __uuidof(ID3D11Device5)) {
		return d3d11Device->QueryInterface(a_riid, a_device);
	}

	return swapChain->GetDevice(a_riid, a_device);
}

void DX12SwapChain::RefreshBackBuffers()
{
	if (commandFence && !WaitForGPUIdle()) {
		DX::ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
	}
	for (auto& backBuffer : swapChainBuffers) {
		backBuffer = nullptr;
	}

	for (auto i = 0; i < std::size(swapChainBuffers); ++i) {
		DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(swapChainBuffers[i].put())));
	}
}
