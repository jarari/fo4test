#include "SwapChainCreationObserver.h"
#include "../extern/ReShade/include/reshade.hpp"
#include <winrt/base.h>
#include <mutex>

namespace SwapChainCreationObserver
{
	namespace
	{
		std::mutex mutex;
		HWND pendingWindow{};
		std::optional<DXGI_SWAP_CHAIN_DESC1> observed;
		void Initialized(reshade::api::swapchain* chain, bool)
		{
			std::lock_guard lock(mutex);
			if (!pendingWindow || chain->get_hwnd() != pendingWindow ||
				chain->get_device()->get_api() != reshade::api::device_api::d3d12) return;
			winrt::com_ptr<IDXGISwapChain1> native;
			auto* object = reinterpret_cast<IUnknown*>(chain->get_native());
			DXGI_SWAP_CHAIN_DESC1 desc{};
			if (object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(native.put()))) && SUCCEEDED(native->GetDesc1(&desc))) {
				// Resource description is authoritative even if a wrapper caches GetDesc1.
				const auto resource = chain->get_device()->get_resource_desc(chain->get_current_back_buffer());
				desc.Format = static_cast<DXGI_FORMAT>(resource.texture.format);
				desc.Width = static_cast<UINT>(resource.texture.width);
				desc.Height = resource.texture.height;
				observed = desc;
			}
		}
	}
	void Initialize()
	{
		reshade::register_event<reshade::addon_event::init_swapchain>(Initialized);
	}
	Scope::Scope(HWND window)
	{
		std::lock_guard lock(mutex);
		if (pendingWindow) throw std::runtime_error("Nested swapchain creation observation");
		pendingWindow = window;
		observed.reset();
	}
	Scope::~Scope()
	{
		std::lock_guard lock(mutex);
		pendingWindow = nullptr;
		observed.reset();
	}
	std::optional<DXGI_SWAP_CHAIN_DESC1> Scope::Result() const
	{
		std::lock_guard lock(mutex);
		return observed;
	}
}
