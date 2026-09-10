#include "SceneReShade.h"
#include "../extern/ReShade/include/reshade.hpp"
#include <array>
#include <memory>
#include <dxgi1_6.h>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>
#include <winrt/base.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>

namespace
{
	DXGI_FORMAT SceneStorageFormat(DXGI_FORMAT format)
	{
		// Ordinary textures need typeless storage for ReShade's linear/sRGB RTV pair.
		// Keep float formats typed so ReShade does not infer a UNORM interpretation.
		switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		default:
			return format;
		}
	}

	struct RuntimeDescription
	{
		winrt::com_ptr<ID3D11Device> device;
		HWND window;
		UINT width, height;
		DXGI_FORMAT format;
		bool operator==(const RuntimeDescription& other) const
		{
			return device.get() == other.device.get() && window == other.window &&
				width == other.width && height == other.height && format == other.format;
		}
	};

	// A real, ReShade-wrapped D3D11 swapchain supplies normal add-on lifecycle.
	// Only this owned native object's Present slots are replaced: ReShade runs
	// its frame callbacks, but no image is submitted to DXGI/DWM.
	struct SceneSwapChain
	{
		winrt::com_ptr<ID3D11Texture2D> color;
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<IDXGISwapChain> proxy;
		winrt::com_ptr<IDXGISwapChain> native;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		HWND window{};
		void** originalTable = nullptr;
		std::array<void*, 41> table{};

		static HRESULT STDMETHODCALLTYPE NoPresent(IDXGISwapChain*, UINT, UINT) { return S_OK; }
		static HRESULT STDMETHODCALLTYPE NoPresent1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*) { return S_OK; }
		void SuppressNativePresent()
		{
			// Query the maximum standard interface before copying its vtable.
			size_t count = 18;
			const auto sameInterface = [this]<class T>() {
				const auto upgraded = native.try_as<T>();
				if (upgraded && static_cast<void*>(upgraded.get()) != static_cast<void*>(native.get()))
					throw std::runtime_error("Scene swapchain uses separate DXGI interface objects");
				return static_cast<bool>(upgraded);
			};
			if (sameInterface.operator()<IDXGISwapChain4>()) count = 41;
			else if (sameInterface.operator()<IDXGISwapChain3>()) count = 40;
			else if (sameInterface.operator()<IDXGISwapChain2>()) count = 36;
			else if (sameInterface.operator()<IDXGISwapChain1>()) count = 29;
			auto** object = reinterpret_cast<void***>(native.get());
			originalTable = *object;
			std::copy_n(originalTable, count, table.begin());
			table[8] = reinterpret_cast<void*>(&NoPresent);
			if (count >= 29) table[22] = reinterpret_cast<void*>(&NoPresent1);
			*object = table.data();
		}
		~SceneSwapChain()
		{
			if (originalTable) *reinterpret_cast<void***>(native.get()) = originalTable;
			// ReShade destroys the runtime through the owning proxy; never use the
			// manual destroy API on this automatically created runtime.
			color = nullptr;
			native = nullptr;
			proxy = nullptr;
		}
	};
	thread_local bool creatingScene = false;
	std::recursive_mutex mutex;
	bool registered = false;
	bool importedPreset = false;
	std::optional<RuntimeDescription> failedRuntime;
	bool depthValid = false;
	std::optional<bool> reportedDepthValid;
	reshade::api::effect_runtime* g_sceneRuntime = nullptr;
	std::unique_ptr<SceneSwapChain> scene;
	winrt::com_ptr<ID3D11ShaderResourceView> currentDepth;
	winrt::com_ptr<ID3D11Texture2D> depthCopy;
	winrt::com_ptr<ID3D11UnorderedAccessView> depthUAV;
	winrt::com_ptr<ID3D11ComputeShader> depthShader;
	std::vector<reshade::api::effect_runtime*> outputs;

	void BeginEffects(reshade::api::effect_runtime* r, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view)
	{
		const std::lock_guard lock(mutex);
		if (r != g_sceneRuntime) return;
		const reshade::api::resource_view depth{reinterpret_cast<uint64_t>(currentDepth.get())};
		// Other providers and effect reloads can change the semantic without changing
		// our SRV pointer. Publish it each frame on this runtime only.
		r->update_texture_bindings("DEPTH",depth,depth);
		r->enumerate_uniform_variables(nullptr,[&](auto* rt, auto u) {
			char source[32]{};
			if(rt->get_annotation_string_from_uniform_variable(u,"source",source) && std::strcmp(source,"bufready_depth")==0)
				rt->set_uniform_value_bool(u,depthValid && depth.handle!=0);
		});
	}
	void InitSwapChain(reshade::api::swapchain* swap, bool)
	{
		const std::lock_guard lock(mutex);
		if (creatingScene && scene && swap->get_device()->get_api() == reshade::api::device_api::d3d11)
			scene->native.copy_from(reinterpret_cast<IDXGISwapChain*>(swap->get_native()));
	}
	void InitRuntime(reshade::api::effect_runtime* r)
	{
		const std::lock_guard lock(mutex);
		outputs.push_back(r);
		if (creatingScene && scene && scene->native && r->get_device()->get_api() == reshade::api::device_api::d3d11 &&
			r->get_hwnd() == scene->window) g_sceneRuntime = r;
	}
	void DestroyRuntime(reshade::api::effect_runtime* r)
	{
		const std::lock_guard lock(mutex);
		std::erase(outputs,r);
		if (r == g_sceneRuntime) g_sceneRuntime = nullptr;
	}
	bool OpenOverlay(reshade::api::effect_runtime* r, bool open, reshade::api::input_source)
	{
		const std::lock_guard lock(mutex);
		return open && g_sceneRuntime && scene && r != g_sceneRuntime && r->get_hwnd() == scene->window;
	}
	void Present(reshade::api::command_queue*, reshade::api::swapchain* swap,
		const reshade::api::rect*, const reshade::api::rect*, uint32_t, const reshade::api::rect*)
	{
		const std::lock_guard lock(mutex);
		if (!g_sceneRuntime || !scene) return;
		for(auto* r:outputs) if(r != g_sceneRuntime && r->get_hwnd()==scene->window && r->get_back_buffer(0)==swap->get_back_buffer(0)) {
			// The scene D3D11 runtime owns effects and overlay in this mode.
			r->render_effects(r->get_command_queue()->get_immediate_command_list(),{},{});
			r->open_overlay(false, reshade::api::input_source::none);
		}
	}

	const char* ValidateDepth(ID3D11ShaderResourceView* depth, ID3D11Device* device, UINT width, UINT height)
	{
		if (!depth) return "missing SRV";
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		depth->GetDesc(&view);
		if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return "requires a Texture2D SRV";
		winrt::com_ptr<ID3D11Resource> resource;
		depth->GetResource(resource.put());
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put())))) return "requires a Texture2D resource";
		winrt::com_ptr<ID3D11Device> owner;
		texture->GetDevice(owner.put());
		if (owner.get() != device) return "depth and color belong to different devices";
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1) return "multisampled depth is unsupported";
		const UINT mip = view.Texture2D.MostDetailedMip;
		if (mip >= desc.MipLevels || mip >= 32 || (desc.Width >> mip) < width || (desc.Height >> mip) < height)
			return "depth view is smaller than the scene rectangle";
		return nullptr;
	}
}

void SceneReShade::Initialize()
{
	if (registered) return;
	HMODULE module{};
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&Initialize),&module);
	registered=reshade::register_addon(module);
	if(!registered) return;
	reshade::register_event<reshade::addon_event::init_swapchain>(InitSwapChain);
	reshade::register_event<reshade::addon_event::init_effect_runtime>(InitRuntime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(DestroyRuntime);
	reshade::register_event<reshade::addon_event::reshade_begin_effects>(BeginEffects);
	reshade::register_event<reshade::addon_event::present>(Present);
	reshade::register_event<reshade::addon_event::reshade_open_overlay>(OpenOverlay);
}

void SceneReShade::Reset()
{
	const std::lock_guard lock(mutex);
	g_sceneRuntime=nullptr;
	scene=nullptr; currentDepth=nullptr;
	depthCopy=nullptr; depthUAV=nullptr; depthShader=nullptr;
	failedRuntime.reset(); reportedDepthValid.reset(); depthValid=false;
}

void SceneReShade::Render(ID3D11Texture2D* color, ID3D11ShaderResourceView* depth, HWND window, UINT width, UINT height)
{
	const std::lock_guard lock(mutex);
	if(!registered || !color || !window || !width || !height) return;
	D3D11_TEXTURE2D_DESC inputDesc{};
	color->GetDesc(&inputDesc);
	if(width > inputDesc.Width || height > inputDesc.Height || inputDesc.SampleDesc.Count != 1) return;
	winrt::com_ptr<ID3D11Device> device;
	color->GetDevice(device.put());
	const RuntimeDescription requested{device, window, width, height, inputDesc.Format};
	if (failedRuntime && *failedRuntime == requested) return;
	const auto storageFormat = SceneStorageFormat(inputDesc.Format);
	if(scene) {
		D3D11_TEXTURE2D_DESC desc{}; scene->color->GetDesc(&desc);
		if(desc.Width!=width || desc.Height!=height || SceneStorageFormat(desc.Format)!=storageFormat || scene->format!=inputDesc.Format ||
			scene->device.get()!=device.get() || scene->window!=window) Reset();
	}
	winrt::com_ptr<ID3D11DeviceContext> context;
	device->GetImmediateContext(context.put());
	if(!g_sceneRuntime) {
		try {
			// ReShade 6.8 copies BGRX into a BGRA resolve texture, which is not a
			// legal D3D11 copy and can remove the device. The game uses RGBA.
			if (inputDesc.Format==DXGI_FORMAT_B8G8R8X8_UNORM || inputDesc.Format==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
				inputDesc.Format==DXGI_FORMAT_B8G8R8X8_TYPELESS)
				throw std::runtime_error("BGRX scene color requires conversion to RGBA/BGRA before ReShade");
			scene=std::make_unique<SceneSwapChain>(); scene->device=device; scene->window=window; scene->format=inputDesc.Format;
			auto desc=inputDesc;
			desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
			desc.Usage=D3D11_USAGE_DEFAULT; desc.CPUAccessFlags=0; desc.MiscFlags=0;
			desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
			desc.Format=storageFormat;
			// Enter ReShade through its normal factory export, not the manual
			// runtime API. This reuses the initialized D3D11 device proxy.
			using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
			const auto createFactory = reinterpret_cast<CreateFactory>(GetProcAddress(
				reshade::internal::get_reshade_module_handle(), "CreateDXGIFactory1"));
			if (!createFactory) throw std::runtime_error("ReShade DXGI factory export unavailable");
			winrt::com_ptr<IDXGIFactory1> factory;
			DX::ThrowIfFailed(createFactory(IID_PPV_ARGS(factory.put())));
			DXGI_SWAP_CHAIN_DESC swapDesc{};
			swapDesc.BufferDesc.Width=width; swapDesc.BufferDesc.Height=height;
			swapDesc.BufferDesc.Format=static_cast<DXGI_FORMAT>(reshade::api::format_to_default_typed(
				static_cast<reshade::api::format>(storageFormat), 0));
			swapDesc.SampleDesc.Count=1;
			swapDesc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT|DXGI_USAGE_SHADER_INPUT;
			swapDesc.BufferCount=1; swapDesc.OutputWindow=window;
			swapDesc.Windowed=TRUE; swapDesc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
			struct CreatingScope {
				CreatingScope() { creatingScene=true; }
				~CreatingScope() { creatingScene=false; }
			};
			{
				const CreatingScope scope;
				DX::ThrowIfFailed(factory->CreateSwapChain(device.get(),&swapDesc,scene->proxy.put()));
			}
			if (!g_sceneRuntime || !scene->native)
				throw std::runtime_error("ReShade did not initialize a scene runtime on the game D3D11 device");
			scene->SuppressNativePresent();
			DX::ThrowIfFailed(scene->proxy->GetBuffer(0,IID_PPV_ARGS(scene->color.put())));
			D3D11_TEXTURE2D_DESC actual{}; scene->color->GetDesc(&actual);
			if (actual.Width!=width || actual.Height!=height || SceneStorageFormat(actual.Format)!=storageFormat)
				throw std::runtime_error("Scene swapchain format or size was overridden");
			// Check the linear/sRGB views on the actual swapchain backbuffer.
			HRESULT viewResults[2]{};
			DXGI_FORMAT viewFormats[2]{};
			winrt::com_ptr<ID3D11Device3> device3;
			device->QueryInterface(IID_PPV_ARGS(device3.put()));
			for (int i=0; i<2; ++i) {
				viewFormats[i]=static_cast<DXGI_FORMAT>(reshade::api::format_to_default_typed(
					static_cast<reshade::api::format>(storageFormat),i));
				if (device3) {
					D3D11_RENDER_TARGET_VIEW_DESC1 view{};
					view.Format=viewFormats[i]; view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
					winrt::com_ptr<ID3D11RenderTargetView1> rtv;
					viewResults[i]=device3->CreateRenderTargetView1(scene->color.get(),&view,rtv.put());
				} else {
					D3D11_RENDER_TARGET_VIEW_DESC view{};
					view.Format=viewFormats[i]; view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
					winrt::com_ptr<ID3D11RenderTargetView> rtv;
					viewResults[i]=device->CreateRenderTargetView(scene->color.get(),&view,rtv.put());
				}
			}
			logger::info("[Scene ReShade] Backbuffer {}x{} inputFormat={} storageFormat={} linearRTV={} hr=0x{:08X} sRGBRTV={} hr=0x{:08X}",
				width,height,static_cast<UINT>(inputDesc.Format),static_cast<UINT>(storageFormat),
				static_cast<UINT>(viewFormats[0]),static_cast<UINT>(viewResults[0]),
				static_cast<UINT>(viewFormats[1]),static_cast<UINT>(viewResults[1]));
			DX::ThrowIfFailed(viewResults[0]);
			DX::ThrowIfFailed(viewResults[1]);
			desc.Format=DXGI_FORMAT_R32_FLOAT;
			desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
			DX::ThrowIfFailed(device->CreateTexture2D(&desc,nullptr,depthCopy.put()));
			DX::ThrowIfFailed(device->CreateShaderResourceView(depthCopy.get(),nullptr,currentDepth.put()));
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(depthCopy.get(),nullptr,depthUAV.put()));
			constexpr char source[] = "Texture2D<float> src:register(t0); RWTexture2D<float> dst:register(u0);"
				"[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){uint w,h;dst.GetDimensions(w,h);"
				"if(p.x<w&&p.y<h)dst[p.xy]=src.Load(int3(p.xy,0));}";
			winrt::com_ptr<ID3DBlob> code, errors;
			DX::ThrowIfFailed(D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,code.put(),errors.put()));
			DX::ThrowIfFailed(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,depthShader.put()));
			// Automatic runtimes use ReShade's own numbered configuration files.
			// Import the user's active preset once; later recreations retain the
			// scene runtime's settings, including changes made in its overlay.
			if (!importedPreset) {
				for (auto* output : outputs) {
					if (output == g_sceneRuntime || output->get_hwnd() != window) continue;
					char preset[32768]{};
					output->get_current_preset_path(preset);
					if (preset[0]) g_sceneRuntime->set_current_preset_path(preset);
					g_sceneRuntime->set_effects_state(output->get_effects_state());
					importedPreset = true;
					break;
				}
			}
			// This provider copies raw, non-reversed engine depth. Configure the
			// existing ReShade settings through its API so shaders interpret it correctly.
			g_sceneRuntime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED","0");
			g_sceneRuntime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN","0");
			g_sceneRuntime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC","0");
			failedRuntime.reset();
			logger::info("[Scene ReShade] Created automatic D3D11 scene runtime before SR: {}x{}; native Present suppressed",width,height);
		} catch(const std::exception& e) {
			logger::error("[Scene ReShade] Initialization failed: {}; retry after reset or scene/device change",e.what());
			Reset(); failedRuntime=requested; return;
		}
	}
	const char* depthError=ValidateDepth(depth,device.get(),width,height);
	depthValid=depthError==nullptr;
	if (!reportedDepthValid || *reportedDepthValid!=depthValid) {
		if (depthValid) logger::info("[Scene ReShade] DEPTH ready: {}x{}, R32_FLOAT, non-reversed",width,height);
		else logger::warn("[Scene ReShade] DEPTH unavailable: {}",depthError);
		reportedDepthValid=depthValid;
	}
	if (!depthValid) depth=nullptr;
	const D3D11_BOX box{0,0,0,width,height,1};
	context->CopySubresourceRegion(scene->color.get(),0,0,0,0,color,0,&box);
	if(depth) {
		winrt::com_ptr<ID3D11ComputeShader> oldShader;
		winrt::com_ptr<ID3D11ShaderResourceView> oldSRV;
		winrt::com_ptr<ID3D11UnorderedAccessView> oldUAV;
		ID3D11ClassInstance* instances[256]{};
		UINT count=static_cast<UINT>(std::size(instances));
		context->CSGetShader(oldShader.put(),instances,&count);
		context->CSGetShaderResources(0,1,oldSRV.put());
		context->CSGetUnorderedAccessViews(0,1,oldUAV.put());
		auto* uav=depthUAV.get();
		context->CSSetShader(depthShader.get(),nullptr,0);
		context->CSSetShaderResources(0,1,&depth);
		context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
		context->Dispatch((width+7)/8,(height+7)/8,1);
		auto* srv=oldSRV.get(); uav=oldUAV.get();
		context->CSSetShaderResources(0,1,&srv);
		context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
		context->CSSetShader(oldShader.get(),instances,count);
		for(UINT i=0;i<count;++i) if(instances[i]) instances[i]->Release();
	} else {
		const float farDepth[4]{1,1,1,1}; context->ClearUnorderedAccessViewFloat(depthUAV.get(),farDepth);
	}
	// The proxy updates effects/input/frame state through normal ReShade events.
	// Its underlying native Present is an instance-local no-op (no display wait).
	const auto presentResult=scene->proxy->Present(0,0);
	if (FAILED(presentResult)) {
		logger::error("[Scene ReShade] Scene effect update failed: 0x{:08X}",static_cast<UINT>(presentResult));
		Reset(); failedRuntime=requested; return;
	}
	context->CopySubresourceRegion(color,0,0,0,0,scene->color.get(),0,&box);
}
