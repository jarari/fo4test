#include "DX12SwapChain.h"
#include "ENBRenderDomain.h"
#include "ENBEffectDiagnostics.h"
#include "ENBTiledLighting.h"
#include "NativeInterfaceUI.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <d3dcompiler.h>
#include <limits>
#include <string_view>

#include "D3D12UIComposite.h"
#include "FidelityFX.h"
#include "OSD.h"
#include "RenderProfiling.h"
#include "Streamline.h"
#include "TaggedTextureDebug.h"
#include "Upscaling.h"
#include "UpscalingMenu.h"
#include "third_party/RTX40MFGUnlock/integration.h"

extern bool enbLoaded;

namespace
{
	struct LoadedImage
	{
		std::uintptr_t base = 0;
		std::size_t size = 0;
		const IMAGE_NT_HEADERS64* ntHeaders = nullptr;
	};

	bool GetLoadedImage(HMODULE a_module, LoadedImage& a_image)
	{
		if (!a_module) {
			return false;
		}

		const auto base = reinterpret_cast<std::uintptr_t>(a_module);
		const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
			return false;
		}

		const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
			ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
			ntHeaders->OptionalHeader.SizeOfImage == 0) {
			return false;
		}

		a_image.base = base;
		a_image.size = ntHeaders->OptionalHeader.SizeOfImage;
		a_image.ntHeaders = ntHeaders;
		return true;
	}

	template <std::size_t N, std::size_t M>
	std::uintptr_t FindUniqueExecutablePattern(
		const LoadedImage& a_image,
		const std::array<std::uint8_t, N>& a_pattern,
		const char (&a_mask)[M])
	{
		static_assert(N + 1 == M);
		std::uintptr_t match = 0;
		const auto* section = IMAGE_FIRST_SECTION(a_image.ntHeaders);
		for (WORD sectionIndex = 0; sectionIndex < a_image.ntHeaders->FileHeader.NumberOfSections; ++sectionIndex, ++section) {
			if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section->VirtualAddress >= a_image.size) {
				continue;
			}

			const auto sectionSize = std::min<std::size_t>(
				section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData,
				a_image.size - section->VirtualAddress);
			if (sectionSize < N) {
				continue;
			}

			const auto* begin = reinterpret_cast<const std::uint8_t*>(a_image.base + section->VirtualAddress);
			for (std::size_t offset = 0; offset <= sectionSize - N; ++offset) {
				bool matches = true;
				for (std::size_t byteIndex = 0; byteIndex < N; ++byteIndex) {
					if (a_mask[byteIndex] != '?' && begin[offset + byteIndex] != a_pattern[byteIndex]) {
						matches = false;
						break;
					}
				}
				if (!matches) {
					continue;
				}

				const auto candidate = reinterpret_cast<std::uintptr_t>(begin + offset);
				if (match != 0) {
					return 0;
				}
				match = candidate;
			}
		}
		return match;
	}

	std::uintptr_t ResolveRipRelativeAddress(
		std::uintptr_t a_instruction,
		std::size_t a_displacementOffset,
		std::size_t a_instructionLength)
	{
		std::int32_t displacement = 0;
		std::memcpy(
			&displacement,
			reinterpret_cast<const void*>(a_instruction + a_displacementOffset),
			sizeof(displacement));
		return a_instruction + a_instructionLength + displacement;
	}

	std::uintptr_t* FindImportSlot(const LoadedImage& a_image, std::string_view a_dllName, std::string_view a_functionName)
	{
		const auto& importDirectory =
			a_image.ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!importDirectory.VirtualAddress || !importDirectory.Size || importDirectory.VirtualAddress >= a_image.size) {
			return nullptr;
		}

		const auto isRvaInImage = [&a_image](std::uintptr_t a_rva, std::size_t a_size = 1) {
			return a_rva < a_image.size && a_size <= a_image.size - a_rva;
		};
		const auto importEnd = std::min<std::uintptr_t>(
			a_image.size,
			static_cast<std::uintptr_t>(importDirectory.VirtualAddress) + importDirectory.Size);
		std::uintptr_t* match = nullptr;
		for (auto descriptorRva = static_cast<std::uintptr_t>(importDirectory.VirtualAddress);
			descriptorRva < importEnd && isRvaInImage(descriptorRva, sizeof(IMAGE_IMPORT_DESCRIPTOR));
			descriptorRva += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
			const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(a_image.base + descriptorRva);
			if (!descriptor->Name) {
				break;
			}
			if (!descriptor->OriginalFirstThunk ||
				!isRvaInImage(descriptor->Name) ||
				!isRvaInImage(descriptor->OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA64)) ||
				!isRvaInImage(descriptor->FirstThunk, sizeof(IMAGE_THUNK_DATA64))) {
				continue;
			}

			const auto* importedDll = reinterpret_cast<const char*>(a_image.base + descriptor->Name);
			if (_stricmp(importedDll, a_dllName.data()) != 0) {
				continue;
			}

			const auto* nameThunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(a_image.base + descriptor->OriginalFirstThunk);
			auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(a_image.base + descriptor->FirstThunk);
			for (std::size_t index = 0;; ++index) {
				const auto nameThunkRva = static_cast<std::uintptr_t>(descriptor->OriginalFirstThunk) + index * sizeof(IMAGE_THUNK_DATA64);
				const auto addressThunkRva = static_cast<std::uintptr_t>(descriptor->FirstThunk) + index * sizeof(IMAGE_THUNK_DATA64);
				if (!isRvaInImage(nameThunkRva, sizeof(IMAGE_THUNK_DATA64)) ||
					!isRvaInImage(addressThunkRva, sizeof(IMAGE_THUNK_DATA64)) ||
					!nameThunk[index].u1.AddressOfData) {
					break;
				}
				if (IMAGE_SNAP_BY_ORDINAL64(nameThunk[index].u1.Ordinal) ||
					!isRvaInImage(nameThunk[index].u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME))) {
					continue;
				}

				const auto* importByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
					a_image.base + nameThunk[index].u1.AddressOfData);
				if (std::string_view(reinterpret_cast<const char*>(importByName->Name)) != a_functionName) {
					continue;
				}

				auto* candidate = reinterpret_cast<std::uintptr_t*>(&addressThunk[index].u1.Function);
				if (match) {
					return nullptr;
				}
				match = candidate;
			}
		}
		return match;
	}

	bool PatchImportPair(
		std::uintptr_t* a_firstSlot,
		std::uintptr_t a_firstValue,
		std::uintptr_t* a_secondSlot,
		std::uintptr_t a_secondValue)
	{
		DWORD firstProtection = 0;
		if (!VirtualProtect(a_firstSlot, sizeof(*a_firstSlot), PAGE_READWRITE, &firstProtection)) {
			return false;
		}

		DWORD secondProtection = 0;
		if (!VirtualProtect(a_secondSlot, sizeof(*a_secondSlot), PAGE_READWRITE, &secondProtection)) {
			DWORD ignoredProtection = 0;
			VirtualProtect(a_firstSlot, sizeof(*a_firstSlot), firstProtection, &ignoredProtection);
			return false;
		}

		InterlockedExchangePointer(
			reinterpret_cast<void* volatile*>(a_firstSlot),
			reinterpret_cast<void*>(a_firstValue));
		InterlockedExchangePointer(
			reinterpret_cast<void* volatile*>(a_secondSlot),
			reinterpret_cast<void*>(a_secondValue));

		DWORD ignoredProtection = 0;
		const auto secondRestored = VirtualProtect(
			a_secondSlot, sizeof(*a_secondSlot), secondProtection, &ignoredProtection);
		const auto firstRestored = VirtualProtect(
			a_firstSlot, sizeof(*a_firstSlot), firstProtection, &ignoredProtection);
		FlushInstructionCache(GetCurrentProcess(), a_firstSlot, sizeof(*a_firstSlot));
		FlushInstructionCache(GetCurrentProcess(), a_secondSlot, sizeof(*a_secondSlot));
		return firstRestored && secondRestored;
	}

	void InstallSideAimUnicodeWndProcCompatibility()
	{
		static bool attempted = false;
		if (attempted) {
			return;
		}
		attempted = true;

		const auto sideAim = GetModuleHandleW(L"SideAimAni.dll");
		if (!sideAim) {
			return;
		}

		LoadedImage image{};
		if (!GetLoadedImage(sideAim, image)) {
			logger::warn("[DX12SwapChain] SideAimAni.dll has invalid PE headers; Unicode WndProc compatibility was not installed");
			return;
		}

		constexpr std::array<std::uint8_t, 20> directWndProcCallPattern{
			0x48, 0x8B, 0x05, 0, 0, 0, 0,
			0x4C, 0x8B, 0xCF,
			0x4C, 0x8B, 0xC6,
			0x8B, 0xD5,
			0x49, 0x8B, 0xCE,
			0xFF, 0xD0
		};
		const auto directWndProcCall = FindUniqueExecutablePattern(
			image,
			directWndProcCallPattern,
			"xxx????xxxxxxxxxxxxx");

		constexpr std::array<std::uint8_t, 45> wndProcInstallPattern{
			0x48, 0x8B, 0xF0,
			0xBA, 0xFC, 0xFF, 0xFF, 0xFF,
			0x48, 0x8B, 0xC8,
			0xFF, 0x15, 0, 0, 0, 0,
			0x48, 0x89, 0x05, 0, 0, 0, 0,
			0x4C, 0x8D, 0x05, 0, 0, 0, 0,
			0xBA, 0xFC, 0xFF, 0xFF, 0xFF,
			0x48, 0x8B, 0xCE,
			0xFF, 0x15, 0, 0, 0, 0
		};
		const auto wndProcInstall = FindUniqueExecutablePattern(
			image,
			wndProcInstallPattern,
			"xxxxxxxxxxxxx????xxx????xxx????xxxxxxxxxx????");
		if (!directWndProcCall || !wndProcInstall) {
			logger::warn("[DX12SwapChain] SideAimAni.dll WndProc signature is unsupported; Unicode WndProc compatibility was not installed");
			return;
		}

		auto* getWindowLongPtrSlot = FindImportSlot(image, "user32.dll", "GetWindowLongPtrA");
		auto* setWindowLongPtrSlot = FindImportSlot(image, "user32.dll", "SetWindowLongPtrA");
		if (!getWindowLongPtrSlot || !setWindowLongPtrSlot) {
			logger::warn("[DX12SwapChain] SideAimAni.dll ANSI WndProc imports could not be resolved");
			return;
		}

		const auto savedWndProcFromCall = ResolveRipRelativeAddress(directWndProcCall, 3, 7);
		const auto getImportFromInstall = ResolveRipRelativeAddress(wndProcInstall + 11, 2, 6);
		const auto savedWndProcFromInstall = ResolveRipRelativeAddress(wndProcInstall + 17, 3, 7);
		const auto setImportFromInstall = ResolveRipRelativeAddress(wndProcInstall + 39, 2, 6);
		if (savedWndProcFromCall != savedWndProcFromInstall ||
			getImportFromInstall != reinterpret_cast<std::uintptr_t>(getWindowLongPtrSlot) ||
			setImportFromInstall != reinterpret_cast<std::uintptr_t>(setWindowLongPtrSlot)) {
			logger::warn("[DX12SwapChain] SideAimAni.dll WndProc signature references do not match its imports");
			return;
		}

		const auto user32 = GetModuleHandleW(L"user32.dll");
		const auto getWindowLongPtrA = reinterpret_cast<std::uintptr_t>(GetProcAddress(user32, "GetWindowLongPtrA"));
		const auto setWindowLongPtrA = reinterpret_cast<std::uintptr_t>(GetProcAddress(user32, "SetWindowLongPtrA"));
		const auto getWindowLongPtrW = reinterpret_cast<std::uintptr_t>(GetProcAddress(user32, "GetWindowLongPtrW"));
		const auto setWindowLongPtrW = reinterpret_cast<std::uintptr_t>(GetProcAddress(user32, "SetWindowLongPtrW"));
		if (!getWindowLongPtrA || !setWindowLongPtrA || !getWindowLongPtrW || !setWindowLongPtrW ||
			*getWindowLongPtrSlot != getWindowLongPtrA || *setWindowLongPtrSlot != setWindowLongPtrA) {
			logger::warn("[DX12SwapChain] SideAimAni.dll WndProc imports were already modified; Unicode compatibility was not installed");
			return;
		}

		if (!PatchImportPair(
				getWindowLongPtrSlot,
				getWindowLongPtrW,
				setWindowLongPtrSlot,
				setWindowLongPtrW)) {
			logger::warn("[DX12SwapChain] Could not patch SideAimAni.dll WndProc imports");
			return;
		}

		logger::info("[DX12SwapChain] Installed SideAimAni.dll Unicode WndProc compatibility");
	}

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

D3D11D3D12SharedTexture::D3D11D3D12SharedTexture(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11Device* a_d3d11Device, ID3D12Device* a_d3d12Device)
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
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
	const auto result = swapChain->GetDesc1(pDesc);
	const auto& domain = ENBRenderDomain::Get();
	if (SUCCEEDED(result) && pDesc && domain.Active()) {
		pDesc->Width = domain.Width();
		pDesc->Height = domain.Height();
		// The game-facing proxy has all three views owned by RendererWindow.
		// WindowSizeChanged releases RTV/SRV/UAV without null checks.
		pDesc->BufferUsage |= DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_UNORDERED_ACCESS;
	}
	return result;
}
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
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetSourceSize(UINT Width, UINT Height)
{
	const auto& domain = ENBRenderDomain::Get();
	if (domain.Active()) {
		return Width == domain.Width() && Height == domain.Height() ? S_OK : E_INVALIDARG;
	}
	return swapChain->SetSourceSize(Width, Height);
}
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
	const auto& domain = ENBRenderDomain::Get();
	if (domain.Active()) {
		if (!pWidth || !pHeight) {
			return E_POINTER;
		}
		*pWidth = domain.Width();
		*pHeight = domain.Height();
		return S_OK;
	}
	return swapChain->GetSourceSize(pWidth, pHeight);
}
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
	RTX40MFGUnlock::ObserveD3D12Device(d3d12Device.get());

	if (a_streamline && a_streamline->slSetD3DDevice) {
		if (SL_FAILED(result, a_streamline->slSetD3DDevice(d3d12Device.get()))) {
			logger::warn("[DX12SwapChain] slSetD3DDevice(D3D12) failed: {}", magic_enum::enum_name(result));
		}
	}
	RTX40MFGUnlock::PatchLoadedModules();

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
	InstallSideAimUnicodeWndProcCompatibility();
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
	EndNativeUI();
	NativeInterfaceUI::ReleaseResources();
	nativeUITexture = nullptr;
	menuComposite = nullptr;
	sceneUISRV = nullptr;
	interopReady = false;
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

HRESULT DX12SwapChain::ResizeENBScene(uint32_t a_quality)
{
	auto& domain = ENBRenderDomain::Get();
	auto* data = RE::BSGraphics::GetRendererData();
	if (!data) { return E_UNEXPECTED; }
	auto* device = reinterpret_cast<ID3D11Device*>(data->device);
	auto* outerSwapChain = reinterpret_cast<IDXGISwapChain*>(data->renderWindow[0].swapChain);
	using CreateTargets = void (*)();
	// Verified OG/AE target-manager creation callback, not WindowSizeChanged.
	static REL::Relocation<CreateTargets*> createTargets{ REL::ID{ 1096254, 2666763 } };
	using DestroyTargets = void (*)(RE::BSGraphics::RenderTargetManager*);
	static REL::Relocation<DestroyTargets> destroyTargets{ REL::ID{ 456166, 2277229 } };
	using SetProperties = void (*)(RE::BSGraphics::RenderTargetManager*, const RE::BSGraphics::RenderTargetProperties&);
	static REL::Relocation<SetProperties> setProperties{ REL::ID{ 1453219, 2721521 } };
	if (!domain.Active() || !IsReady() || !swapChainBufferProxyENB || !device || !outerSwapChain || !*createTargets.get() || sceneResizeActive) {
		return E_UNEXPECTED;
	}
	if (Streamline::GetSingleton()->NeedsDLSSGPresentSafety() || FidelityFX::GetSingleton()->IsFrameGenerationEnabled()) {
		return DXGI_ERROR_WAS_STILL_DRAWING;
	}
	if (!WaitForInteropIdle()) { return DXGI_ERROR_DEVICE_REMOVED; }
	EndNativeUI();
	const auto oldQuality = domain.Quality();
	const auto oldWidth = domain.Width();
	const auto oldHeight = domain.Height();
	const auto extent = ENBRenderDomain::CalculateExtent(swapChainDesc.Width, swapChainDesc.Height, a_quality);
	ENBTiledLightingResize tiledLighting;
	// Reserve the full display extent on the first live transition, so later
	// quality changes need no further tiled-lighting allocation.
	const auto lightingResult = tiledLighting.Prepare(device, swapChainDesc.Width, swapChainDesc.Height);
	if (FAILED(lightingResult)) { return lightingResult; }
	try {
		D3D11_TEXTURE2D_DESC desc{};
		swapChainBufferProxyENB->resource11->GetDesc(&desc);
		desc.Width = extent.width;
		desc.Height = extent.height;
		// Allocate before modifying the active domain. Allocation failure leaves
		// the old scene, engine views, UI and presentation resources untouched.
		// Use the game-facing device so ENB registers the new texture's private
		// metadata, which its RTV/SRV creation then propagates to the views.
		// The raw device bypasses that registration; ENB's GetBuffer only forwards.
		pendingSceneProxy = std::make_unique<D3D11D3D12SharedTexture>(desc, device, d3d12Device.get());
	} catch (const std::exception& e) {
		logger::error("[ENB scene resize] Allocation failed: {}", e.what());
		return E_OUTOFMEMORY;
	}
	struct EndTransaction
	{
		bool& active;
		~EndTransaction() { active = false; }
	} transaction{ sceneResizeActive };
	sceneResizeActive = true;
	sceneResizeObserved = false;
	pendingSceneQuality = a_quality;
	ENBEffectDiagnostics::BeforeResize();
	auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
	context->ClearState();
	// This notifies the ENB wrapper only. The nested proxy call is handled by
	// the sceneResizeActive branch and does not resize the real swapchain.
	// ENB can reset AntTweakBar to the scene extent even though our native UI
	// texture survives. Invalidate its coordinate cache independently, including
	// failed resize / rollback attempts that may still touch ENB's GUI state.
	++enbSceneResizeGeneration;
	HRESULT result = outerSwapChain->ResizeBuffers(0, extent.width, extent.height, DXGI_FORMAT_UNKNOWN, swapChainDesc.Flags);
	if (SUCCEEDED(result) && !sceneResizeObserved) { result = E_UNEXPECTED; }
	winrt::com_ptr<ID3D11Texture2D> buffer;
	winrt::com_ptr<ID3D11RenderTargetView> rtv;
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	winrt::com_ptr<ID3D11UnorderedAccessView> uav;
	if (SUCCEEDED(result)) { result = outerSwapChain->GetBuffer(0, IID_PPV_ARGS(buffer.put())); }
	if (SUCCEEDED(result)) {
		D3D11_TEXTURE2D_DESC desc{};
		buffer->GetDesc(&desc);
		if (desc.Width != extent.width || desc.Height != extent.height) { result = E_UNEXPECTED; }
	}
	if (SUCCEEDED(result)) { result = device->CreateRenderTargetView(buffer.get(), nullptr, rtv.put()); }
	if (SUCCEEDED(result)) { result = device->CreateShaderResourceView(buffer.get(), nullptr, srv.put()); }
	if (SUCCEEDED(result)) { result = device->CreateUnorderedAccessView(buffer.get(), nullptr, uav.put()); }
	if (FAILED(result)) {
		// Restore ENB's descriptor/cache as well as the proxy if notification or
		// view creation failed. Do not destroy the game's still-valid scene RTs.
		if (sceneResizeObserved) {
			pendingSceneQuality = oldQuality;
			sceneResizeObserved = false;
			const auto rollback = outerSwapChain->ResizeBuffers(0, oldWidth, oldHeight, DXGI_FORMAT_UNKNOWN, swapChainDesc.Flags);
			if (!sceneResizeObserved) {
				pendingSceneProxy.swap(swapChainBufferProxyENB);
				domain.SetQuality(oldQuality, swapChainDesc.Width, swapChainDesc.Height);
			}
			logger::error("[ENB scene resize] Failed hr=0x{:08X}; rollback hr=0x{:08X}", static_cast<uint32_t>(result), static_cast<uint32_t>(rollback));
		}
		return result;
	}

	Upscaling::GetSingleton()->DestroyUpscalingResources();
	Streamline::GetSingleton()->DestroyDLSSResources();
	presentOverrideFinalColor = nullptr;
	for (auto& command : commandContexts) { command.retainedPresentOverride = nullptr; }
	NativeInterfaceUI::ReleaseResources();
	sceneUISRV = nullptr;
	auto& windowTarget = data->renderWindow[0].swapChainRenderTarget;
	// RendererWindow owns the views; RT0 aliases them without another AddRef.
	// The backing texture is owned by the proxy (as in the native UI path).
	const auto oldTarget = windowTarget;
	windowTarget = {};
	windowTarget.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(swapChainBufferProxyENB->resource11.get());
	windowTarget.rtView = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(rtv.detach());
	windowTarget.srView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(srv.detach());
	windowTarget.uaView = reinterpret_cast<REX::W32::ID3D11UnorderedAccessView*>(uav.detach());
	data->renderTargets[0] = windowTarget;
	auto* manager = Util::RenderTargetManager_GetSingleton();
	auto properties = manager->renderTargetData[0];
	properties.width = extent.width;
	properties.height = extent.height;
	setProperties(manager, properties);
	destroyTargets(manager);
	(*createTargets.get())();
	data->renderTargets[0] = windowTarget;
	domain.ApplySceneDimensions();
	tiledLighting.Commit();
	if (oldTarget.rtView) { oldTarget.rtView->Release(); }
	if (oldTarget.srView) { oldTarget.srView->Release(); }
	if (oldTarget.uaView) { oldTarget.uaView->Release(); }
	// Keep the former proxy alive until both APIs finish the rebuild work too.
	if (!WaitForInteropIdle()) {
		logger::error("[ENB scene resize] Post-rebuild GPU drain failed; retaining former proxy");
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	pendingSceneProxy.reset();
	Streamline::GetSingleton()->RequestTemporalReset();
	logger::info("[ENB scene resize] quality {} -> {} scene={}x{} display={}x{}; engine-window-resize=false real-swapchain-resize=false native-UI-preserved=true",
		oldQuality, a_quality, domain.Width(), domain.Height(), swapChainDesc.Width, swapChainDesc.Height);
	return S_OK;
}

HRESULT DX12SwapChain::ResizeBuffersInternal(bool a_useResizeBuffers1, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags, const UINT* a_creationNodeMask)
{
	// ENB's outer wrapper still needs its ResizeBuffers notification, but a
	// quality-only transaction must never reach the real display swapchain.
	if (sceneResizeActive) {
		if (!sceneResizeObserved) {
			if (!pendingSceneProxy) { return E_UNEXPECTED; }
			pendingSceneProxy.swap(swapChainBufferProxyENB);
			// Commit the virtual descriptor with the buffer, not before the
			// outer ENB wrapper enters ResizeBuffers and inspects its old state.
			ENBRenderDomain::Get().SetQuality(pendingSceneQuality, swapChainDesc.Width, swapChainDesc.Height);
			sceneResizeObserved = true;
		}
		return S_OK;
	}
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
	if (ENBRenderDomain::Get().Active()) {
		// Game / ENB resize arguments describe the virtual buffer. The real
		// swapchain follows the HWND, never the low-resolution descriptor.
		RECT client{};
		if (GetClientRect(hwnd, &client) && client.right > client.left && client.bottom > client.top) {
			a_width = static_cast<UINT>(client.right - client.left);
			a_height = static_cast<UINT>(client.bottom - client.top);
		} else {
			a_width = swapChainDesc.Width;
			a_height = swapChainDesc.Height;
		}
	}
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
	auto targetMode = *a_newTargetParameters;
	if (ENBRenderDomain::Get().Active()) {
		// The virtual mode must not resize the HWND / output to scene resolution.
		targetMode.Width = swapChainDesc.Width;
		targetMode.Height = swapChainDesc.Height;
	}
	const auto result = swapChain->ResizeTarget(&targetMode);
	windowStateDirty.store(true, std::memory_order_release);
	return result;
}

void DX12SwapChain::CreateInterop()
{
	ENBRenderDomain::Get().Initialize(swapChainDesc.Width, swapChainDesc.Height);
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
	interopReady = false;
	D3D11_TEXTURE2D_DESC textureDesc{};
	textureDesc.Width = swapChainDesc.Width;
	textureDesc.Height = swapChainDesc.Height;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = swapChainDesc.Format;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	const auto& domain = ENBRenderDomain::Get();
	if (domain.Active()) {
		textureDesc.Width = domain.Width();
		textureDesc.Height = domain.Height();
	}

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
		textureDesc.Width = swapChainDesc.Width;
		textureDesc.Height = swapChainDesc.Height;
		textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		context.presentStaging = std::make_unique<D3D11D3D12SharedTexture>(textureDesc, d3d11Device.get(), d3d12Device.get());
	}
	interopReady = true;
}

DX12SwapChain::CommandContext& DX12SwapChain::AcquireCommandContext()
{
	const ScopedRenderCPUProfile cpuTiming("command-context-acquire (inclusive)");
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
	const auto waitResult = [&]() {
		const ScopedRenderCPUProfile cpuTiming("command-fence-wait");
		return WaitForSingleObjectEx(commandFenceEvent.get(), 5000, FALSE);
	}();
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
	if (nativeUIActive && nativeUITexture) {
		return nativeUITexture->resource->QueryInterface(a_riid, a_surface);
	}

	if (swapChainBufferProxyENB) {
		return swapChainBufferProxyENB->resource11->QueryInterface(a_riid, a_surface);
	}

	if (!swapChainBufferProxy) {
		return E_POINTER;
	}

	return swapChainBufferProxy->resource->QueryInterface(a_riid, a_surface);
}

bool DX12SwapChain::WaitForInteropIdle()
{
	if (!d3d11Context || !d3d11Fence || !commandQueue || !d3d12Fence) {
		return false;
	}
	const auto value = fenceValue++;
	if (FAILED(d3d11Context->Signal(d3d11Fence.get(), value))) {
		return false;
	}
	d3d11Context->Flush();
	return SUCCEEDED(commandQueue->Wait(d3d12Fence.get(), value)) && WaitForGPUIdle();
}

void DX12SwapChain::EndNativeUI()
{
	if (!nativeUIActive) {
		return;
	}
	d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);
	auto* renderer = RE::BSGraphics::GetRendererData();
	renderer->renderTargets[0] = savedSceneTarget;
	if (nativeUIWindowTargetActive) {
		auto& windowTarget = renderer->renderWindow[0].swapChainRenderTarget;
		// RendererWindow owns its view references; RT0 only borrows them.
		if (windowTarget.rtView) { windowTarget.rtView->Release(); }
		if (windowTarget.srView) { windowTarget.srView->Release(); }
		if (windowTarget.uaView) { windowTarget.uaView->Release(); }
		windowTarget = savedSceneWindowTarget;
		nativeUIWindowTargetActive = false;
	}
	nativeUIActive = false;
	nativeUIIncludesScene = false;
	ENBRenderDomain::Get().ApplySceneDimensions();
}

void DX12SwapChain::PublishNativeUIForOverlays()
{
	if (!nativeUIActive || nativeUIWindowTargetActive) { return; }
	if (UpscalingMenu::IsOpen()) {
		ResolveNativeUIForMenu();
	}
	// Only after engine UI/model rendering has finished. RendererWindow is
	// also an engine input; aliasing it during Interface3D can feed UI back
	// into model composition. Late Present overlays still need this alias.
	auto* renderer = RE::BSGraphics::GetRendererData();
	auto& windowTarget = renderer->renderWindow[0].swapChainRenderTarget;
	savedSceneWindowTarget = windowTarget;
	windowTarget = renderer->renderTargets[0];
	windowTarget.rtView->AddRef();
	windowTarget.srView->AddRef();
	windowTarget.uaView->AddRef();
	nativeUIWindowTargetActive = true;
}

void DX12SwapChain::ResolveNativeUIForMenu()
{
	if (nativeUIIncludesScene || !presentOverrideFinalColor) { return; }
	const ScopedRenderCPUProfile timing("menu-background-resolve (inclusive)");
	// Present-time background filters expect a complete frame. Only while the
	// framework menu is open, resolve the D3D12 scene + engine UI before its
	// D3D11 blur runs. Normal gameplay keeps the asynchronous split UI path.
	if (!menuComposite) {
		D3D11_TEXTURE2D_DESC desc{};
		nativeUITexture->resource->GetDesc(&desc);
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		menuComposite = std::make_unique<D3D11D3D12SharedTexture>(desc, d3d11Device.get(), d3d12Device.get());
	}
	auto& command = AcquireCommandContext();
	auto* staging = command.presentStaging.get();
	d3d11Context->CopyResource(staging->resource11.get(), nativeUITexture->resource.get());
	const auto inputFence = fenceValue++;
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), inputFence));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), inputFence));
	command.retainedPresentOverride = presentOverrideFinalColor;
	auto* scene = command.retainedPresentOverride.get();
	auto* output = menuComposite->resource12.get();
	auto* input = staging->resource12.get();
	D3D12_RESOURCE_BARRIER before[]{
		CD3DX12_RESOURCE_BARRIER::Transition(scene, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)
	};
	command.list->ResourceBarrier(static_cast<UINT>(std::size(before)), before);
	D3D12UIComposite::GetSingleton()->Render(d3d12Device.get(), command.list.get(), output, scene, input,
		swapChainDesc.Format, swapChainDesc.Width, swapChainDesc.Height,
		command.index, static_cast<uint32_t>(std::size(commandContexts)));
	for (auto& barrier : before) {
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	}
	command.list->ResourceBarrier(static_cast<UINT>(std::size(before)), before);
	ExecuteCommandContext(command);
	const auto outputFence = fenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), outputFence));
	DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), outputFence));
	d3d11Context->CopyResource(nativeUITexture->resource.get(), menuComposite->resource11.get());
	nativeUIIncludesScene = true;
}

bool DX12SwapChain::BeginNativeUI()
{
	if (!ENBRenderDomain::Get().Active() || nativeUIActive) {
		return true;
	}
	if (!IsReady() || !swapChainBufferProxyENB) {
		return false;
	}
	const ScopedRenderCPUProfile uiPreparationTiming("native-UI-prepare (inclusive)");
	try {
		if (!nativeUITexture) {
			D3D11_TEXTURE2D_DESC desc{};
			swapChainBufferProxyENB->resource11->GetDesc(&desc);
			desc.Width = swapChainDesc.Width;
			desc.Height = swapChainDesc.Height;
			desc.MiscFlags = 0;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			winrt::com_ptr<ID3D11Texture2D> texture;
			DX::ThrowIfFailed(d3d11Device->CreateTexture2D(&desc, nullptr, texture.put()));
			auto ui = std::make_unique<Texture2D>(texture.detach());
			DX::ThrowIfFailed(d3d11Device->CreateRenderTargetView(ui->resource.get(), nullptr, ui->rtv.put()));
			DX::ThrowIfFailed(d3d11Device->CreateShaderResourceView(ui->resource.get(), nullptr, ui->srv.put()));
			DX::ThrowIfFailed(d3d11Device->CreateUnorderedAccessView(ui->resource.get(), nullptr, ui->uav.put()));
			nativeUITexture = std::move(ui);
			++nativeUIGeneration;
			logger::info("[ENB UI] Native screen-space target {}x{}; scene {}x{}, generation={}",
				desc.Width, desc.Height, ENBRenderDomain::Get().Width(), ENBRenderDomain::Get().Height(), nativeUIGeneration);
		}
		if (!sceneUISRV) {
			DX::ThrowIfFailed(d3d11Device->CreateShaderResourceView(swapChainBufferProxyENB->resource11.get(), nullptr, sceneUISRV.put()));
		}
		if (!nativeUIResolve) {
			constexpr char source[] = R"(
Texture2D<float4> Input : register(t0);
RWTexture2D<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);
[numthreads(8, 8, 1)]
void main(uint3 p : SV_DispatchThreadID)
{
    uint width, height;
    Output.GetDimensions(width, height);
    if (p.x >= width || p.y >= height) return;
    Output[p.xy] = Input.SampleLevel(LinearClamp, (float2(p.xy) + 0.5) / float2(width, height), 0);
})";
			winrt::com_ptr<ID3DBlob> shader, errors;
			DX::ThrowIfFailed(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.put(), errors.put()));
			DX::ThrowIfFailed(d3d11Device->CreateComputeShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, nativeUIResolve.put()));
		}
		if (!nativeUISampler) {
			D3D11_SAMPLER_DESC sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			DX::ThrowIfFailed(d3d11Device->CreateSamplerState(&sampler, nativeUISampler.put()));
		}
		// A separate D3D12 scene requires a transparent overlay, including when
		// menu rendering leaves an old world image in the D3D11 backbuffer.
		if (presentOverrideFinalColor) {
			const float transparent[4]{};
			d3d11Context->ClearRenderTargetView(nativeUITexture->rtv.get(), transparent);
		} else {
			// No separate scene exists in the fallback path: retain its background.
			winrt::com_ptr<ID3D11ComputeShader> oldShader;
			std::array<ID3D11ClassInstance*, 256> instances{};
			UINT instanceCount = static_cast<UINT>(instances.size());
			d3d11Context->CSGetShader(oldShader.put(), instances.data(), &instanceCount);
			winrt::com_ptr<ID3D11ShaderResourceView> oldSRV;
			winrt::com_ptr<ID3D11UnorderedAccessView> oldUAV;
			winrt::com_ptr<ID3D11SamplerState> oldSampler;
			d3d11Context->CSGetShaderResources(0, 1, oldSRV.put());
			d3d11Context->CSGetUnorderedAccessViews(0, 1, oldUAV.put());
			d3d11Context->CSGetSamplers(0, 1, oldSampler.put());
			d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);
			auto* input = sceneUISRV.get();
			auto* output = nativeUITexture->uav.get();
			auto* sampler = nativeUISampler.get();
			d3d11Context->CSSetShader(nativeUIResolve.get(), nullptr, 0);
			d3d11Context->CSSetShaderResources(0, 1, &input);
			d3d11Context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			d3d11Context->CSSetSamplers(0, 1, &sampler);
			d3d11Context->Dispatch((swapChainDesc.Width + 7) / 8, (swapChainDesc.Height + 7) / 8, 1);
			input = nullptr;
			output = nullptr;
			d3d11Context->CSSetShaderResources(0, 1, &input);
			d3d11Context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			input = oldSRV.get();
			output = oldUAV.get();
			sampler = oldSampler.get();
			d3d11Context->CSSetShaderResources(0, 1, &input);
			d3d11Context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			d3d11Context->CSSetSamplers(0, 1, &sampler);
			d3d11Context->CSSetShader(oldShader.get(), instances.data(), instanceCount);
			for (UINT i = 0; i < instanceCount; ++i) {
				instances[i]->Release();
			}
		}
		auto* renderer = RE::BSGraphics::GetRendererData();
		savedSceneTarget = renderer->renderTargets[0];
		auto& target = renderer->renderTargets[0];
		target = {};
		target.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(nativeUITexture->resource.get());
		target.rtView = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(nativeUITexture->rtv.get());
		target.srView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(nativeUITexture->srv.get());
		target.uaView = reinterpret_cast<REX::W32::ID3D11UnorderedAccessView*>(nativeUITexture->uav.get());
		auto* state = Util::State_GetSingleton();
		state->screenWidth = state->backBufferWidth = swapChainDesc.Width;
		state->screenHeight = state->backBufferHeight = swapChainDesc.Height;
		auto* manager = Util::RenderTargetManager_GetSingleton();
		manager->renderTargetData[0].width = swapChainDesc.Width;
		manager->renderTargetData[0].height = swapChainDesc.Height;
		auto* rtv = nativeUITexture->rtv.get();
		d3d11Context->OMSetRenderTargets(1, &rtv, nullptr);
		using SetViewport = void (*)(RE::BSGraphics::RenderTargetManager*);
		static REL::Relocation<SetViewport> setViewport{ REL::ID{ 158420, 2277192 } };
		setViewport(manager);
		D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(swapChainDesc.Width), static_cast<float>(swapChainDesc.Height), 0, 1 };
		d3d11Context->RSSetViewports(1, &viewport);
		nativeUIActive = true;
		return true;
	} catch (const std::exception& e) {
		logger::error("[ENB UI] Native UI preparation failed: {}", e.what());
		return false;
	}
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	// TEST does not consume the staged scene or advance resource retirement.
	if (Flags & DXGI_PRESENT_TEST) {
		return swapChain ? swapChain->Present(SyncInterval, Flags) : DXGI_ERROR_INVALID_CALL;
	}
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
		presentOverrideFinalColor = nullptr;
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
			upscaling->AdvanceDeferredResourceReleases();
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

	if (ENBRenderDomain::Get().Active()) {
		if (!BeginNativeUI()) {
			return E_FAIL;
		}
		d3d11Context->CopyResource(presentStaging->resource11.get(), nativeUITexture->resource.get());
	} else if (swapChainBufferProxyENB) {
		d3d11Context->CopyResource(presentStaging->resource11.get(), swapChainBufferProxyENB->resource11.get());
	} else {
		d3d11Context->CopyResource(presentStaging->resource11.get(), swapChainBufferProxy->resource.get());
	}
	const auto& enbDomain = ENBRenderDomain::Get();
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	++fenceValue;

	const auto presentedFrameIndex = frameIndex;
	auto destination = swapChainBuffers[presentedFrameIndex].get();
	auto copySource = presentStaging->resource12.get();
	commandContext.retainedPresentOverride = std::move(presentOverrideFinalColor);
	// The menu's D3D11 background filter has already consumed the resolved scene.
	// Retain its resource through submission, but do not composite it a second time.
	auto overrideFinalColor = nativeUIIncludesScene ? nullptr : commandContext.retainedPresentOverride.get();
	const bool usePresentOverride = overrideFinalColor != nullptr;
	const bool useComposite = usePresentOverride || enbDomain.Active();
	if (enbDomain.Active() && upscaling->settings.enbGPUTiming) {
		const auto* state = Util::State_GetSingleton();
		if (state && state->frameCount % 120 == 0) {
			const auto sourceDesc = copySource->GetDesc();
			const auto destinationDesc = destination->GetDesc();
			const auto baseDesc = overrideFinalColor ? overrideFinalColor->GetDesc() : sourceDesc;
			UINT sourceWidth = 0;
			UINT sourceHeight = 0;
			const auto sourceSizeResult = swapChain->GetSourceSize(&sourceWidth, &sourceHeight);
			logger::info("[ENB present] frame={} override={} staging={}x{} base={}x{} destination={}x{} viewport={}x{} real-source={}x{} source-query-ok={}",
				state->frameCount, usePresentOverride, sourceDesc.Width, sourceDesc.Height,
				baseDesc.Width, baseDesc.Height, destinationDesc.Width, destinationDesc.Height,
				swapChainDesc.Width, swapChainDesc.Height, sourceWidth, sourceHeight, SUCCEEDED(sourceSizeResult));
		}
	}

	D3D12_RESOURCE_BARRIER beforeCopy[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(
			copySource,
			D3D12_RESOURCE_STATE_COMMON,
			useComposite ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
	};
	commandList->ResourceBarrier(static_cast<UINT>(std::size(beforeCopy)), beforeCopy);
	D3D12_RESOURCE_STATES destinationState = D3D12_RESOURCE_STATE_COPY_DEST;
	if (useComposite) {
		auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		commandList->ResourceBarrier(1, &toRenderTarget);
		if (overrideFinalColor) {
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(overrideFinalColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			commandList->ResourceBarrier(1, &barrier);
		}
		D3D12UIComposite::GetSingleton()->Render(
			d3d12Device.get(),
			commandList,
			destination,
			overrideFinalColor ? overrideFinalColor : copySource,
			usePresentOverride ? copySource : nullptr,
			swapChainDesc.Format,
			swapChainDesc.Width,
			swapChainDesc.Height,
			commandContext.index,
			static_cast<uint32_t>(std::size(commandContexts)));
		if (overrideFinalColor) {
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(overrideFinalColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			commandList->ResourceBarrier(1, &barrier);
		}
		destinationState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	} else {
		commandList->CopyResource(destination, copySource);
	}
	auto afterSourceCopy = CD3DX12_RESOURCE_BARRIER::Transition(
		copySource,
		useComposite ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE,
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
	const auto result = [&]() {
		const ScopedRenderCPUProfile cpuTiming("real-present");
		return swapChain->Present(presentSyncInterval, presentFlags);
	}();
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
		upscaling->AdvanceDeferredResourceReleases();
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
	const ScopedRenderCPUProfile cpuTiming("D3D12-evaluate-submit (inclusive)");
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
		const ScopedRenderCPUProfile cpuTiming("FSR-record");
		result.fsr = upscaling->EvaluateD3D12FSR(a_commandList, a_frameIndex);
	}
	if (a_evaluateDLSS) {
		const ScopedRenderCPUProfile cpuTiming("DLSS-NR-SR-record");
		result.dlss = upscaling->EvaluateD3D12DLSS(a_commandList, a_frameIndex);
	}

	const bool upscalerRequested = a_evaluateFSR || a_evaluateDLSS;
	const bool upscalerSucceeded = (!a_evaluateFSR || result.fsr) && (!a_evaluateDLSS || result.dlss);
	if (a_evaluateFSRFrameGeneration && (!upscalerRequested || upscalerSucceeded)) {
		const ScopedRenderCPUProfile cpuTiming("FSR-FG-record");
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
