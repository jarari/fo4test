#include "DX11Hooks.h"

#include <d3d11.h>
#include <magic_enum/magic_enum.hpp>

#include "DX12SwapChain.h"
#include "ENBRenderDomain.h"
#include "Streamline.h"
#include "Upscaling.h"

namespace
{
	using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

	struct SwapChainSize
	{
		UINT width = 0;
		UINT height = 0;
	};

	struct GameSwapChainDesc
	{
		HWND hwnd = nullptr;
		SwapChainSize size{};
		bool valid = false;
	};

	GameSwapChainDesc gameSwapChainDesc;

	bool GetClientSize(HWND a_hwnd, SwapChainSize& a_size)
	{
		if (!a_hwnd) {
			return false;
		}

		RECT clientRect{};
		if (!GetClientRect(a_hwnd, &clientRect)) {
			return false;
		}

		const auto width = clientRect.right - clientRect.left;
		const auto height = clientRect.bottom - clientRect.top;
		if (width <= 0 || height <= 0) {
			return false;
		}

		a_size.width = static_cast<UINT>(width);
		a_size.height = static_cast<UINT>(height);
		return true;
	}

	SwapChainSize ResolveSwapChainSize(const DXGI_SWAP_CHAIN_DESC& a_desc)
	{
		SwapChainSize size{};
		if (GetClientSize(a_desc.OutputWindow, size)) {
			return size;
		}

		size.width = a_desc.BufferDesc.Width;
		size.height = a_desc.BufferDesc.Height;
		return size;
	}

	bool IsPlausibleGameSwapChainSize(const SwapChainSize& a_size)
	{
		return a_size.width >= 640 && a_size.height >= 360;
	}

	void CaptureGameSwapChainDesc(const DXGI_SWAP_CHAIN_DESC& a_desc)
	{
		auto size = ResolveSwapChainSize(a_desc);
		if (!a_desc.OutputWindow || !IsPlausibleGameSwapChainSize(size)) {
			gameSwapChainDesc = {};
			logger::warn(
				"[DX12SwapChain] Game swapchain candidate rejected hwnd={} size={}x{}",
				static_cast<void*>(a_desc.OutputWindow),
				size.width,
				size.height);
			return;
		}

		gameSwapChainDesc.hwnd = a_desc.OutputWindow;
		gameSwapChainDesc.size = size;
		gameSwapChainDesc.valid = true;
		logger::info(
			"[DX12SwapChain] Game swapchain candidate hwnd={} size={}x{}",
			static_cast<void*>(gameSwapChainDesc.hwnd),
			gameSwapChainDesc.size.width,
			gameSwapChainDesc.size.height);
	}

	bool IsGameSwapChainRequest(const DXGI_SWAP_CHAIN_DESC& a_desc)
	{
		if (DX12SwapChain::GetSingleton()->IsReady()) {
			logger::info("[DX12SwapChain] Skipping factory swapchain hook because the D3D12 proxy swapchain is already active");
			return false;
		}

		auto size = ResolveSwapChainSize(a_desc);
		if (!gameSwapChainDesc.valid) {
			logger::warn(
				"[DX12SwapChain] Skipping factory swapchain hook because no game swapchain candidate is available; hwnd={} size={}x{}",
				static_cast<void*>(a_desc.OutputWindow),
				size.width,
				size.height);
			return false;
		}

		if (a_desc.OutputWindow != gameSwapChainDesc.hwnd ||
			size.width != gameSwapChainDesc.size.width ||
			size.height != gameSwapChainDesc.size.height ||
			!IsPlausibleGameSwapChainSize(size)) {
			logger::info(
				"[DX12SwapChain] Skipping non-game swapchain hwnd={} size={}x{} expectedHwnd={} expectedSize={}x{}",
				static_cast<void*>(a_desc.OutputWindow),
				size.width,
				size.height,
				static_cast<void*>(gameSwapChainDesc.hwnd),
				gameSwapChainDesc.size.width,
				gameSwapChainDesc.size.height);
			return false;
		}

		return true;
	}

	winrt::com_ptr<IDXGIAdapter> ResolveAdapter(IDXGIAdapter* a_adapter, ID3D11Device* a_device)
	{
		winrt::com_ptr<IDXGIAdapter> result;
		if (a_adapter) {
			result.copy_from(a_adapter);
			return result;
		}

		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) {
			IDXGIAdapter* adapter = nullptr;
			if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
				result.attach(adapter);
			}
		}

		return result;
	}

	Streamline* SelectStreamlineForD3D12Proxy(Streamline* a_streamline, IDXGIAdapter* a_adapter, bool a_wantsD3D12FrameGeneration)
	{
		if (!a_streamline || !a_streamline->interposer) {
			return nullptr;
		}

		a_streamline->Initialize(sl::RenderAPI::eD3D12);
		if (!a_streamline->UsesD3D12()) {
			return nullptr;
		}

		a_streamline->CheckFeatures(a_adapter);
		if constexpr (Upscaling::kForceFSRFrameGenerationForTesting) {
			if (a_wantsD3D12FrameGeneration) {
				logger::info("[DX12SwapChain] Streamline proxy disabled because FSR frame generation testing is forced");
				return nullptr;
			}
		}

		if (a_wantsD3D12FrameGeneration && !a_streamline->featureDLSSG) {
			logger::info("[DX12SwapChain] Streamline proxy disabled because FidelityFX frame generation will own the swapchain");
			return nullptr;
		}

		if (!a_streamline->featureDLSS && !a_streamline->featureDLSSG) {
			logger::info("[DX12SwapChain] Streamline proxy disabled because DLSS and DLSS-G are unavailable");
			return nullptr;
		}

		return a_streamline;
	}
}

struct hkIDXGIFactoryCreateSwapChain
{
	static HRESULT STDMETHODCALLTYPE thunk(
		IDXGIFactory* This,
		IUnknown* pDevice,
		DXGI_SWAP_CHAIN_DESC* pDesc,
		IDXGISwapChain** ppSwapChain)
	{
		if (!This || !pDevice || !pDesc || !ppSwapChain || !pDesc->Windowed) {
			return func(This, pDevice, pDesc, ppSwapChain);
		}

		if (!IsGameSwapChainRequest(*pDesc)) {
			return func(This, pDevice, pDesc, ppSwapChain);
		}

		winrt::com_ptr<ID3D11Device> d3d11Device;
		if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(d3d11Device.put())))) {
			return func(This, pDevice, pDesc, ppSwapChain);
		}

		try {
			auto streamline = Streamline::GetSingleton();
			logger::info("[DX12SwapChain] Factory swapchain hook requested");

			winrt::com_ptr<ID3D11DeviceContext> d3d11Context;
			d3d11Device->GetImmediateContext(d3d11Context.put());

			auto adapter = ResolveAdapter(nullptr, d3d11Device.get());
			if (!adapter) {
				throw DX::com_exception(E_FAIL);
			}

			Streamline* streamlineForProxy = SelectStreamlineForD3D12Proxy(streamline, adapter.get(), true);

			winrt::com_ptr<IDXGIFactory5> dxgiFactory;
			DX::ThrowIfFailed(This->QueryInterface(IID_PPV_ARGS(dxgiFactory.put())));

			auto dx12SwapChain = DX12SwapChain::GetSingleton();
			dx12SwapChain->SetD3D11Device(d3d11Device.get());
			dx12SwapChain->SetD3D11DeviceContext(d3d11Context.get());
			dx12SwapChain->CreateD3D12Device(adapter.get(), streamlineForProxy);
			if (!streamlineForProxy && streamline->UsesD3D12() && streamline->slSetD3DDevice) {
				if (SL_FAILED(result, streamline->slSetD3DDevice(dx12SwapChain->GetD3D12Device()))) {
					logger::warn("[DX12SwapChain] slSetD3DDevice(D3D12 native) failed: {}", magic_enum::enum_name(result));
				}
			}

			if (streamline->UsesD3D12()) {
				streamline->PostDevice();
			}

			const bool useFidelityFXFrameGeneration = (Upscaling::kForceFSRFrameGenerationForTesting || !streamline->featureDLSSG);
			if (useFidelityFXFrameGeneration) {
				logger::info("[DX12SwapChain] FidelityFX frame generation selected force={} dlssgAvailable={}", Upscaling::kForceFSRFrameGenerationForTesting, streamline->featureDLSSG);
			}
			dx12SwapChain->CreateSwapChain(dxgiFactory.get(), *pDesc, streamlineForProxy, useFidelityFXFrameGeneration);
			dx12SwapChain->CreateInterop();

			*ppSwapChain = dx12SwapChain->GetSwapChainProxy();
			if (streamlineForProxy) {
				streamline->SetSwapChain(*ppSwapChain);
			}
			return S_OK;
		} catch (const std::exception& e) {
			ENBRenderDomain::Get().CancelInitialization();
			logger::error("[DX12SwapChain] Failed to create ENB D3D12 proxy swapchain: {}", e.what());
			logger::warn("[DX12SwapChain] Falling back to the original DXGI factory swapchain");
			auto streamline = Streamline::GetSingleton();
			if (streamline->UsesD3D12()) {
				streamline->Shutdown();
			}
			return func(This, pDevice, pDesc, ppSwapChain);
		}
	}
	static inline CreateSwapChain_t func;
};

struct hkIDXGISwapChainPresent
{
	static HRESULT STDMETHODCALLTYPE thunk(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
	{
		auto streamline = Streamline::GetSingleton();
		streamline->OnPresentStart();
		const auto result = func(This, SyncInterval, Flags);
		streamline->OnPresentEnd(result);
		return result;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct hkD3D11CreateDeviceAndSwapChain
{
	static HRESULT WINAPI thunk(
		IDXGIAdapter* pAdapter,
		D3D_DRIVER_TYPE DriverType,
		HMODULE Software,
		UINT Flags,
		const D3D_FEATURE_LEVEL* pFeatureLevels,
		UINT FeatureLevels,
		UINT SDKVersion,
		const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
		IDXGISwapChain** ppSwapChain,
		ID3D11Device** ppDevice,
		D3D_FEATURE_LEVEL* pFeatureLevel,
		ID3D11DeviceContext** ppImmediateContext)
	{
		const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
		pFeatureLevels = &featureLevel;
		FeatureLevels = 1;

		auto streamline = Streamline::GetSingleton();
		if (pSwapChainDesc && pSwapChainDesc->Windowed && pAdapter) {
			CaptureGameSwapChainDesc(*pSwapChainDesc);

			static bool factoryHooked = false;
			if (!factoryHooked) {
				winrt::com_ptr<IDXGIFactory> dxgiFactory;
				if (SUCCEEDED(pAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.put())))) {
					*(uintptr_t*)&hkIDXGIFactoryCreateSwapChain::func =
						Detours::X64::DetourClassVTable(*(uintptr_t*)dxgiFactory.get(), &hkIDXGIFactoryCreateSwapChain::thunk, 10);
					factoryHooked = true;
					logger::info("[DX12SwapChain] Installed IDXGIFactory::CreateSwapChain hook");
				} else {
					logger::warn("[DX12SwapChain] IDXGI factory could not be resolved; D3D12 proxy will not be available");
				}
			}
		}

		DX::ThrowIfFailed(func(pAdapter,
			DriverType,
			Software,
			Flags,
			pFeatureLevels,
			FeatureLevels,
			SDKVersion,
			pSwapChainDesc,
			ppSwapChain,
			ppDevice,
			pFeatureLevel,
			ppImmediateContext));

		if (DX12SwapChain::GetSingleton()->IsReady()) {
			// Renderer::Init has set the native State dimensions before this call.
			// Replace them before InitD3D creates game scene/depth targets.
			ENBRenderDomain::Get().ApplySceneDimensions();
			logger::info("[DX12SwapChain] Factory hook created D3D12 proxy swapchain; skipping D3D11 Streamline initialization");
			return S_OK;
		}

		if (streamline->interposer && !streamline->initialized) {
			streamline->Initialize(sl::RenderAPI::eD3D11);
		}

		if (streamline->interposer && !streamline->UsesD3D12()){
			streamline->slUpgradeInterface((void**)&(*ppSwapChain));
			streamline->slSetD3DDevice(*ppDevice);

			streamline->SetSwapChain(*ppSwapChain);

			static bool presentHooked = false;
			if (!presentHooked && *ppSwapChain) {
				stl::detour_vfunc<8, hkIDXGISwapChainPresent>(*ppSwapChain);
				presentHooked = true;
			}

			IDXGIAdapter* featureAdapter = pAdapter;
			winrt::com_ptr<IDXGIAdapter> queriedAdapter;
			if (!featureAdapter && *ppDevice) {
				winrt::com_ptr<IDXGIDevice> dxgiDevice;
				if (SUCCEEDED((*ppDevice)->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) {
					IDXGIAdapter* adapter = nullptr;
					if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
						queriedAdapter.attach(adapter);
						featureAdapter = queriedAdapter.get();
					}
				}
			}

			streamline->CheckFeatures(featureAdapter);
			streamline->PostDevice();
		}

		return S_OK;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

namespace DX11Hooks
{
	void Install()
	{
		auto streamline = Streamline::GetSingleton();
		streamline->LoadInterposer();

		uintptr_t moduleBase = (uintptr_t)GetModuleHandle(nullptr);
		// Hook BSGraphics::CreateD3DAndSwapChain::D3D11CreateDeviceAndSwapChain to use D3D_FEATURE_LEVEL_11_1
		(uintptr_t&)hkD3D11CreateDeviceAndSwapChain::func = Detours::IATHook(moduleBase, "d3d11.dll", "D3D11CreateDeviceAndSwapChain", (uintptr_t)hkD3D11CreateDeviceAndSwapChain::thunk);
	}
}
