#include "SceneReShade.h"
#include "../extern/ReShade/include/reshade.hpp"
#include <atomic>
#include <cstring>
#include <filesystem>
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

	// A single D3D11 scene buffer, never the real D3D12 presentation buffer.
	// ReShade's manual runtime only needs IDXGISwapChain, not flip-model interfaces.
	class SceneSwapChain final : public IDXGISwapChain
	{
	public:
		winrt::com_ptr<ID3D11Texture2D> color;
		winrt::com_ptr<ID3D11Device> device;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		HWND window{};
		std::atomic<ULONG> refs{1};
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
		{
			if (!out) return E_POINTER;
			*out = nullptr;
			if (id != __uuidof(IUnknown) && id != __uuidof(IDXGIObject) &&
				id != __uuidof(IDXGIDeviceSubObject) && id != __uuidof(IDXGISwapChain)) return E_NOINTERFACE;
			*out = static_cast<IDXGISwapChain*>(this); AddRef(); return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
		ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs; if (!n) delete this; return n; }
		HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id, UINT n, const void* data) override { return color->SetPrivateData(id,n,data); }
		HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id, const IUnknown* data) override { return color->SetPrivateDataInterface(id,data); }
		HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id, UINT* n, void* data) override { return color->GetPrivateData(id,n,data); }
		HRESULT STDMETHODCALLTYPE GetParent(REFIID, void** out) override { if (out) *out=nullptr; return E_NOINTERFACE; }
		HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** out) override { return device->QueryInterface(id,out); }
		HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID id, void** out) override
		{ if (index) { if(out) *out=nullptr; return DXGI_ERROR_INVALID_CALL; } return color->QueryInterface(id,out); }
		HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* out) override
		{
			if (!out) return E_POINTER;
			D3D11_TEXTURE2D_DESC d{}; color->GetDesc(&d); *out = {};
			out->BufferDesc.Width=d.Width; out->BufferDesc.Height=d.Height; out->BufferDesc.Format=format;
			out->SampleDesc=d.SampleDesc; out->BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
			out->BufferCount=1; out->OutputWindow=window; out->Windowed=TRUE; out->SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* full, IDXGIOutput** output) override
		{ if(full) *full=FALSE; if(output) *output=nullptr; return S_OK; }
		HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT,UINT,UINT,DXGI_FORMAT,UINT) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** out) override { if(out) *out=nullptr; return DXGI_ERROR_NOT_FOUND; }
		HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return DXGI_ERROR_FRAME_STATISTICS_DISJOINT; }
		HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* n) override { if(!n) return E_POINTER; *n=0; return S_OK; }
	};
	std::recursive_mutex mutex;
	bool registered = false;
	std::optional<RuntimeDescription> failedRuntime;
	bool depthValid = false;
	std::optional<bool> reportedDepthValid;
	reshade::api::effect_runtime* g_sceneRuntime = nullptr;
	winrt::com_ptr<SceneSwapChain> scene;
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
	void InitRuntime(reshade::api::effect_runtime* r)
	{ const std::lock_guard lock(mutex); outputs.push_back(r); }
	void DestroyRuntime(reshade::api::effect_runtime* r)
	{ const std::lock_guard lock(mutex); std::erase(outputs,r); }
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
			// The manually created D3D11 runtime owns effects and overlay in this mode.
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
	reshade::register_event<reshade::addon_event::init_effect_runtime>(InitRuntime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(DestroyRuntime);
	reshade::register_event<reshade::addon_event::reshade_begin_effects>(BeginEffects);
	reshade::register_event<reshade::addon_event::present>(Present);
	reshade::register_event<reshade::addon_event::reshade_open_overlay>(OpenOverlay);
}

void SceneReShade::Reset()
{
	const std::lock_guard lock(mutex);
	if(g_sceneRuntime) { auto* old=g_sceneRuntime; g_sceneRuntime=nullptr; reshade::destroy_effect_runtime(old); }
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
		if(desc.Width!=width || desc.Height!=height || desc.Format!=storageFormat || scene->format!=inputDesc.Format ||
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
			scene.attach(new SceneSwapChain()); scene->device=device; scene->window=window; scene->format=inputDesc.Format;
			auto desc=inputDesc;
			desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
			desc.Usage=D3D11_USAGE_DEFAULT; desc.CPUAccessFlags=0; desc.MiscFlags=0;
			desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
			desc.Format=storageFormat;
			DX::ThrowIfFailed(device->CreateTexture2D(&desc,nullptr,scene->color.put()));
			// Preflight the exact view formats used by ReShade, retaining HRESULTs
			// that its bool-returning manual-runtime export would otherwise hide.
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
			// Use the existing ReShade configuration without creating a plugin-specific copy.
			wchar_t executable[32768]{};
			if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))))
				throw std::runtime_error("Cannot resolve game directory");
			const auto directory=std::filesystem::path(executable).parent_path();
			const auto config=directory/"ReShade.ini";
			if(!reshade::create_effect_runtime(reshade::api::device_api::d3d11,scene->device.get(),context.get(),
				static_cast<IDXGISwapChain*>(scene.get()),config.string().c_str(),&g_sceneRuntime)) {
				throw std::runtime_error("Manual D3D11 runtime creation failed; see ReShade.log");
			}
			// This provider copies raw, non-reversed engine depth. Configure the
			// existing ReShade settings through its API so shaders interpret it correctly.
			g_sceneRuntime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED","0");
			g_sceneRuntime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN","0");
			g_sceneRuntime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC","0");
			failedRuntime.reset();
			logger::info("[Scene ReShade] Created D3D11 scene runtime before SR: {}x{}; config=ReShade.ini",width,height);
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
	// This export updates shaders, input, frame uniforms and the per-frame guard,
	// then renders effects/overlay. It does not call IDXGISwapChain::Present.
	reshade::update_and_present_effect_runtime(g_sceneRuntime);
	context->CopySubresourceRegion(color,0,0,0,0,scene->color.get(),0,&box);
}
