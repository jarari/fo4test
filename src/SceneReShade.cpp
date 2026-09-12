#include "SceneReShade.h"
#include "../extern/ReShade/include/reshade.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#pragma comment(lib, "d3dcompiler.lib")
static_assert(RESHADE_API_VERSION >= 20, "ReShade add-on SDK API 20 or newer is required.");

namespace
{
	using Runtime = reshade::api::effect_runtime;
	using winrt::com_ptr;
	constexpr auto kSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	constexpr auto kDepthFormat = DXGI_FORMAT_R32_FLOAT;
	// No absolute luminance calibration is inferred from a floating-point texture format.
	constexpr float kSceneToScRGBScale = 1.0f;
	static_assert(kSceneToScRGBScale > 0.0f);

	std::atomic_bool initialized{ false };
	std::atomic_bool resetRequested{ false };
	std::atomic<Runtime*> sceneRuntime{ nullptr };
	std::atomic_flag busy = ATOMIC_FLAG_INIT;
	thread_local bool insideScene = false;
	thread_local bool creatingScene = false;
	std::atomic_bool depthCallbacksRegistered{ false };
	ULONGLONG retryAfter = 0;
	std::atomic_bool overlapLogged{ false };
	std::string resumePreset;
	std::optional<bool> resumeEffects;
	// Only intercept the device we create for the output/FG domain.
	thread_local com_ptr<ID3D12Device>* creatingOutputDevice = nullptr;

	void InitDevice(reshade::api::device* device)
	{
		if (!creatingOutputDevice || *creatingOutputDevice || device->get_api() != reshade::api::device_api::d3d12) return;
		auto* native = reinterpret_cast<ID3D12Device*>(device->get_native());
		if (native) creatingOutputDevice->copy_from(native);
	}

	void Check(HRESULT hr, const char* operation)
	{
		if (FAILED(hr)) {
			logger::error("[Scene ReShade] {} failed: 0x{:08X}", operation, static_cast<UINT>(hr));
			throw std::runtime_error(operation);
		}
	}

	bool SameObject(IUnknown* a, IUnknown* b)
	{
		if (!a || !b) return false;
		com_ptr<IUnknown> ia, ib;
		return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(ia.put()))) &&
			SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(ib.put()))) && ia.get() == ib.get();
	}

	struct Entry
	{
		bool entered = !busy.test_and_set(std::memory_order_acquire);
		Entry() { if (entered) insideScene = true; }
		~Entry() { if (entered) { insideScene = false; busy.clear(std::memory_order_release); } }
	};

	struct Surface
	{
		com_ptr<ID3D11Texture2D> texture;
		com_ptr<ID3D11ShaderResourceView> srv;
		com_ptr<ID3D11RenderTargetView> rtv;
	};

	struct alignas(16) BlitConstants
	{
		float uvScale[2];
		float uvBias[2];
		float rgbScale;
		float clampNegative;
		float depthOnly;
		float opaque;
	};
	static_assert(sizeof(BlitConstants) == 32);

	// Numeric format conversion only. No gamma curve, tonemap or saturation in the HDR path.
	constexpr char kBlitShader[] = R"hlsl(
cbuffer Parameters : register(b0) {
	float2 UVScale; float2 UVBias;
	float RGBScale; float ClampNegative; float DepthOnly; float Opaque;
};
Texture2D<float4> InputTexture : register(t0);
SamplerState InputSampler : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
	Vertex o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float4 PS(Vertex i) : SV_Target {
	float4 c = InputTexture.SampleLevel(InputSampler, i.uv * UVScale + UVBias, 0);
	if (DepthOnly > 0.5) return float4(c.r, 0, 0, 1);
	c.rgb *= RGBScale;
	if (ClampNegative > 0.5) c.rgb = max(c.rgb, 0.0);
	return float4(c.rgb, Opaque > 0.5 ? 1.0 : c.a);
}
)hlsl";

	struct Scene
	{
		com_ptr<ID3D11Device> device;
		com_ptr<ID3D11DeviceContext> context;
		com_ptr<ID3D11DeviceContext1> context1;
		com_ptr<ID3DDeviceContextState> isolatedState;
		com_ptr<IDXGISwapChain> proxy;
		com_ptr<IDXGISwapChain> native;
		void** originalTable = nullptr;
		std::array<void*, 41> presentTable{};
		Surface work, depth;
		com_ptr<ID3D11ShaderResourceView> mainDepth;
		Runtime* runtime = nullptr;
		HWND window = nullptr;
		UINT width = 0, height = 0;
		com_ptr<ID3D11VertexShader> vertexShader;
		com_ptr<ID3D11PixelShader> pixelShader;
		com_ptr<ID3D11Buffer> constants;
		com_ptr<ID3D11SamplerState> pointSampler, linearSampler;
		com_ptr<ID3D11RasterizerState> rasterizer;
		com_ptr<ID3D11DepthStencilState> noDepth;
		com_ptr<ID3D11BlendState> overlayBlend;
		com_ptr<ID3D11Texture2D> inputColor, outputColor;
		com_ptr<ID3D11ShaderResourceView> inputSRV;
		com_ptr<ID3D11RenderTargetView> inputRTV, outputRTV;
		com_ptr<ID3D11Texture2D> depthSource;
		com_ptr<ID3D11ShaderResourceView> depthInput;
		D3D11_SHADER_RESOURCE_VIEW_DESC lastDepthView{};
		bool depthReady = false, effectsIssued = false, missingLateLogged = false;
		bool depthFailureLogged = false;
		std::optional<std::uint64_t> hdrFrame, uiFrame;

		static HRESULT STDMETHODCALLTYPE NoPresent(IDXGISwapChain*, UINT, UINT) { return S_OK; }
		static HRESULT STDMETHODCALLTYPE NoPresent1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*) { return S_OK; }

		void SuppressNativePresent()
		{
			std::size_t count = 18;
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
			std::copy_n(originalTable, count, presentTable.begin());
			presentTable[8] = reinterpret_cast<void*>(&NoPresent);
			if (count >= 29) presentTable[22] = reinterpret_cast<void*>(&NoPresent1);
			*object = presentTable.data();
		}

		~Scene()
		{
			// Destruction is driven by the owning automatic proxy, not the manual-runtime API.
			if (originalTable && native) *reinterpret_cast<void***>(native.get()) = originalTable;
			work = {};
			native = nullptr;
			proxy = nullptr;
		}
	};
	std::unique_ptr<Scene> scene;

	struct ContextScope
	{
		com_ptr<ID3D11DeviceContext1> context;
		com_ptr<ID3DDeviceContextState> previous;
		explicit ContextScope(Scene& s) : context(s.context1)
		{
			context->SwapDeviceContextState(s.isolatedState.get(), previous.put());
			if (!previous) throw std::runtime_error("SwapDeviceContextState returned no previous state");
			context->ClearState();
		}
		~ContextScope() { if (previous) context->SwapDeviceContextState(previous.get(), nullptr); }
	};

	std::string ConfigValue(Runtime* r, const char* section, const char* key)
	{
		std::size_t size = 0;
		if (!reshade::get_config_value(r, section, key, nullptr, &size) || !size) return {};
		std::string value(size, '\0');
		if (!reshade::get_config_value(r, section, key, value.data(), &size)) return {};
		while (!value.empty() && value.back() == '\0') value.pop_back();
		return value;
	}

	bool LegacyDefinition(std::string_view item)
	{
		const auto equal = item.find('=');
		auto name = item.substr(0, equal);
		while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
		while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
		return name == "MANUAL_OVERRIDE_MODE_ENABLE" || name == "SURFACE_FORMAT_OVERRIDE" ||
			name == "SURFACE_CSP_OVERRIDE" || name == "CSP_OVERRIDE" || name == "SCENE_RESHADER_HDR_BRIDGE";
	}

	std::string FilterDefinitions(std::string_view list, char separator)
	{
		std::string result;
		for (std::size_t begin = 0; begin < list.size();) {
			const auto next = list.find(separator, begin);
			const auto item = list.substr(begin, next == std::string_view::npos ? list.size() - begin : next - begin);
			if (!item.empty() && !LegacyDefinition(item)) {
				if (!result.empty()) result += separator;
				result.append(item);
			}
			if (next == std::string_view::npos) break;
			begin = next + 1;
		}
		return result;
	}

	void DestroyScene()
	{
		if (scene && scene->runtime && sceneRuntime.load(std::memory_order_acquire) == scene->runtime) {
			char path[32768]{};
			scene->runtime->get_current_preset_path(path);
			if (path[0]) resumePreset = path;
			resumeEffects = scene->runtime->get_effects_state();
		}
		sceneRuntime.store(nullptr, std::memory_order_release);
		scene.reset();
	}

	com_ptr<ID3DBlob> Compile(const char* entry, const char* target)
	{
		com_ptr<ID3DBlob> code, errors;
		const auto hr = D3DCompile(kBlitShader, sizeof(kBlitShader) - 1, "SceneReShade_Blit", nullptr, nullptr,
			entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
		if (FAILED(hr) && errors)
			logger::error("[Scene ReShade] HLSL: {}", static_cast<const char*>(errors->GetBufferPointer()));
		Check(hr, "D3DCompile");
		return code;
	}

	void CreateSurface(Scene& s, Surface& surface, DXGI_FORMAT format)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = s.width; desc.Height = s.height; desc.MipLevels = 1; desc.ArraySize = 1;
		desc.Format = format; desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		Check(s.device->CreateTexture2D(&desc, nullptr, surface.texture.put()), "Create surface");
		Check(s.device->CreateShaderResourceView(surface.texture.get(), nullptr, surface.srv.put()), "Create surface SRV");
		Check(s.device->CreateRenderTargetView(surface.texture.get(), nullptr, surface.rtv.put()), "Create surface RTV");
	}

	void CreateBlitter(Scene& s)
	{
		const auto vs = Compile("VS", "vs_5_0"), ps = Compile("PS", "ps_5_0");
		Check(s.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, s.vertexShader.put()), "Create blit VS");
		Check(s.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, s.pixelShader.put()), "Create blit PS");
		D3D11_BUFFER_DESC cb{};
		cb.ByteWidth = sizeof(BlitConstants); cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Check(s.device->CreateBuffer(&cb, nullptr, s.constants.put()), "Create blit constants");
		D3D11_SAMPLER_DESC sampler{};
		sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.MaxAnisotropy = 1; sampler.MaxLOD = D3D11_FLOAT32_MAX; sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
		Check(s.device->CreateSamplerState(&sampler, s.pointSampler.put()), "Create point sampler");
		sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		Check(s.device->CreateSamplerState(&sampler, s.linearSampler.put()), "Create UI sampler");
		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
		Check(s.device->CreateRasterizerState(&raster, s.rasterizer.put()), "Create rasterizer");
		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
		depth.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK; depth.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
		depth.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
		depth.BackFace = depth.FrontFace;
		Check(s.device->CreateDepthStencilState(&depth, s.noDepth.put()), "Create depth-disabled state");
		D3D11_BLEND_DESC blend{};
		auto& rt = blend.RenderTarget[0];
		rt.BlendEnable = TRUE; rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOp = D3D11_BLEND_OP_ADD; rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		Check(s.device->CreateBlendState(&blend, s.overlayBlend.put()), "Create premultiplied UI blend");
	}

	void Blit(Scene& s, ID3D11ShaderResourceView* input, ID3D11RenderTargetView* target, UINT width, UINT height,
		const BlitConstants& data, bool overlay = false)
	{
		// render_effects may leave the work RTV bound. Unbind it BEFORE binding that same texture as an SRV.
		s.context->OMSetRenderTargets(0, nullptr, nullptr);
		s.context->UpdateSubresource(s.constants.get(), 0, nullptr, &data, 0, 0);
		s.context->SetPredication(nullptr, FALSE);
		s.context->SOSetTargets(0, nullptr, nullptr);
		s.context->IASetInputLayout(nullptr);
		s.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		s.context->VSSetShader(s.vertexShader.get(), nullptr, 0);
		s.context->GSSetShader(nullptr, nullptr, 0);
		s.context->HSSetShader(nullptr, nullptr, 0);
		s.context->DSSetShader(nullptr, nullptr, 0);
		s.context->PSSetShader(s.pixelShader.get(), nullptr, 0);
		auto* cb = s.constants.get();
		s.context->PSSetConstantBuffers(0, 1, &cb);
		auto* sampler = overlay ? s.linearSampler.get() : s.pointSampler.get();
		s.context->PSSetSamplers(0, 1, &sampler);
		s.context->PSSetShaderResources(0, 1, &input);
		s.context->RSSetState(s.rasterizer.get());
		const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
		s.context->RSSetViewports(1, &viewport);
		s.context->OMSetDepthStencilState(s.noDepth.get(), 0);
		s.context->OMSetBlendState(overlay ? s.overlayBlend.get() : nullptr, nullptr, 0xFFFFFFFF);
		s.context->OMSetRenderTargets(1, &target, nullptr);
		s.context->Draw(3, 0);
		ID3D11ShaderResourceView* nullSRV = nullptr;
		s.context->PSSetShaderResources(0, 1, &nullSRV);
		s.context->OMSetRenderTargets(0, nullptr, nullptr);
	}

	bool IsSceneCallback(Runtime* r)
	{
		return insideScene && scene && r == scene->runtime;
	}

	void PublishDepth(Scene& s)
	{
		auto* r = s.runtime;
		const auto* depth = s.mainDepth.get();
		const reshade::api::resource_view view{ reinterpret_cast<std::uint64_t>(depth) };
		r->update_texture_bindings("DEPTH", view, view);
		r->enumerate_uniform_variables(nullptr, [ready = s.depthReady](Runtime* runtime, reshade::api::effect_uniform_variable variable) {
			char source[32]{};
			if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "bufready_depth") == 0)
				runtime->set_uniform_value_bool(variable, ready);
		});
	}

	void InvalidateDepth(Scene& s)
	{
		if (!s.depthReady && !s.mainDepth) return;
		s.depthReady = false;
		s.mainDepth = nullptr;
		PublishDepth(s); // Withdraw a prior frame's binding, including while effects are off.
	}

	void BindMainDepth(Runtime* r, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view)
	{
		if (!IsSceneCallback(r) || !r->get_effects_state()) return;
		PublishDepth(*scene);
	}

	void ReloadedEffects(Runtime* r)
	{
		if (IsSceneCallback(r)) BindMainDepth(r, nullptr, {}, {});
	}

	void InitSwapChain(reshade::api::swapchain* swap, bool)
	{
		if (!creatingScene || !scene || swap->get_device()->get_api() != reshade::api::device_api::d3d11) return;
		scene->native.copy_from(reinterpret_cast<IDXGISwapChain*>(swap->get_native()));
	}

	void InitRuntime(Runtime* r)
	{
		if (!initialized.load(std::memory_order_acquire)) return;
		// Startup-only registration, after built-in add-ons and before the first runtime Present.
		// Register different event lists, never modify the currently-dispatched init_runtime list.
		if (!depthCallbacksRegistered.exchange(true, std::memory_order_acq_rel)) {
			reshade::register_event<reshade::addon_event::reshade_begin_effects>(BindMainDepth);
			reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(ReloadedEffects);
		}
		if (creatingScene && scene && r->get_device()->get_api() == reshade::api::device_api::d3d11 && r->get_hwnd() == scene->window) {
			scene->runtime = r;
			sceneRuntime.store(r, std::memory_order_release);
			return;
		}
	}

	void DestroyRuntime(Runtime* r)
	{
		if (r == sceneRuntime.load(std::memory_order_acquire)) {
			sceneRuntime.store(nullptr, std::memory_order_release);
			if (insideScene && scene) scene->runtime = nullptr;
		}
	}

	void ConfigureScene(Scene& s)
	{
		auto definitions = FilterDefinitions(ConfigValue(s.runtime, "GENERAL", "PreprocessorDefinitions"), '\0');
		if (!definitions.empty()) definitions += '\0';
		definitions += "SCENE_RESHADER_HDR_BRIDGE=3"; // Prevent fallback to legacy global format overrides.
		reshade::set_config_value(s.runtime, "GENERAL", "PreprocessorDefinitions", definitions.c_str(), definitions.size());
		reshade::set_config_value(s.runtime, "GENERAL", "SkipLoadingDisabledEffects", false);
		reshade::set_config_value(s.runtime, "GENERAL", "PerformanceMode", false);
		// ReShade loads this runtime's INPUT settings itself, with its normal
		// global fallback. Preserve custom modifiers and deliberately unbound keys.
		// ReShade 6.8 runtime_gui.cpp passes this to imgui_ps_4_0 independently
		// of the effect color space. GUI pixels stay SDR-encoded, including alpha.
		reshade::set_config_value(s.runtime, "STYLE", "HdrOverlayOverwriteColorSpaceTo", 1);
		if (!resumePreset.empty()) s.runtime->set_current_preset_path(resumePreset.c_str());
		s.runtime->set_color_space(reshade::api::color_space::scrgb);
		// The actual surface/permutation is FP16, not a shader-specific HDR spoof.
		s.runtime->set_preprocessor_definition("MANUAL_OVERRIDE_MODE_ENABLE", "0");
		s.runtime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", "0");
		s.runtime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0");
		s.runtime->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "0");
		s.runtime->set_effects_state(resumeEffects.value_or(s.runtime->get_effects_state()));
		s.runtime->reload_effect_next_frame(nullptr);
	}

	void CreateScene(ID3D11Device* device, HWND window, UINT width, UINT height)
	{
		scene = std::make_unique<Scene>();
		auto& s = *scene;
		s.device.copy_from(device); s.window = window; s.width = width; s.height = height;
		s.device->GetImmediateContext(s.context.put());
		s.context1 = s.context.try_as<ID3D11DeviceContext1>();
		const auto device1 = s.device.try_as<ID3D11Device1>();
		if (!device1 || !s.context1) throw std::runtime_error("D3D11.1 context-state isolation is required");
		const auto level = s.device->GetFeatureLevel();
		D3D_FEATURE_LEVEL chosen{};
		Check(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, s.isolatedState.put()),
			"CreateDeviceContextState");
		const ContextScope initializationState(s);
		using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
		const auto createFactory = reinterpret_cast<CreateFactory>(GetProcAddress(reshade::internal::get_reshade_module_handle(), "CreateDXGIFactory1"));
		if (!createFactory) throw std::runtime_error("ReShade DXGI factory export is missing");
		com_ptr<IDXGIFactory1> factory;
		Check(createFactory(IID_PPV_ARGS(factory.put())), "Create ReShade factory");
		DXGI_SWAP_CHAIN_DESC desc{};
		desc.BufferDesc.Width = width; desc.BufferDesc.Height = height; desc.BufferDesc.Format = kSceneFormat;
		desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
		desc.BufferCount = 1; desc.OutputWindow = window; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		struct CreationScope { CreationScope() { creatingScene = true; } ~CreationScope() { creatingScene = false; } };
		{ const CreationScope scope; Check(factory->CreateSwapChain(device, &desc, s.proxy.put()), "Create FP16 scene swapchain"); }
		if (!s.runtime || !s.native) throw std::runtime_error("ReShade did not wrap the scene D3D11 swapchain");
		s.SuppressNativePresent();
		Check(s.proxy->GetBuffer(0, IID_PPV_ARGS(s.work.texture.put())), "Get scene backbuffer");
		D3D11_TEXTURE2D_DESC actual{};
		s.work.texture->GetDesc(&actual);
		if (actual.Width != width || actual.Height != height || actual.Format != kSceneFormat || actual.SampleDesc.Count != 1)
			throw std::runtime_error("An add-on overrode the required typed FP16 scene backbuffer");
		Check(s.device->CreateShaderResourceView(s.work.texture.get(), nullptr, s.work.srv.put()), "Create FP16 SRV");
		Check(s.device->CreateRenderTargetView(s.work.texture.get(), nullptr, s.work.rtv.put()), "Create FP16 RTV");
		CreateSurface(s, s.depth, kDepthFormat);
		CreateBlitter(s);
		ConfigureScene(s);
		logger::info("[Scene ReShade] FP16 runtime ready {}x{}; native Present suppressed; UI/effects share D3D11 runtime", width, height);
	}

	void PrepareInput(Scene& s, ID3D11Texture2D* color, DXGI_FORMAT format)
	{
		if (s.inputColor.get() == color && s.inputSRV && s.inputRTV) return;
		s.inputSRV = nullptr; s.inputRTV = nullptr; s.inputColor.copy_from(color);
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = format; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
		D3D11_RENDER_TARGET_VIEW_DESC rtv{};
		rtv.Format = format; rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		Check(s.device->CreateShaderResourceView(color, &srv, s.inputSRV.put()), "Create HDR source SRV");
		Check(s.device->CreateRenderTargetView(color, &rtv, s.inputRTV.put()), "Create HDR writeback RTV");
	}

	void SnapshotDepth(Scene& s, ID3D11ShaderResourceView* source, const RECT* requestedRect)
	{
		s.depthReady = false;
		const float farPlane[4]{ 1, 0, 0, 1 };
		s.context->ClearRenderTargetView(s.depth.rtv.get(), farPlane);
		const auto reject = [&](const char* reason) {
			if (!s.depthFailureLogged) logger::warn("[Scene ReShade] DEPTH unavailable: {}. bufready_depth=false; no stale depth reuse", reason);
			s.depthFailureLogged = true;
		};
		if (!source) { reject("null caller SRV"); return; }
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		source->GetDesc(&view);
		if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || view.Texture2D.MostDetailedMip != 0) {
			reject("expected a non-MSAA Texture2D depth view at mip zero"); return;
		}
		switch (view.Format) {
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
			break;
		default: reject("view is not a supported raw-depth format"); return;
		}
		com_ptr<ID3D11Resource> resource;
		source->GetResource(resource.put());
		const auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture) { reject("not a Texture2D resource"); return; }
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		const RECT rect = requestedRect ? *requestedRect : RECT{ 0, 0, static_cast<LONG>(s.width), static_cast<LONG>(s.height) };
		if (desc.SampleDesc.Count != 1 || rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
			static_cast<UINT>(rect.right) > desc.Width || static_cast<UINT>(rect.bottom) > desc.Height) {
			reject("depth sample rectangle exceeds the actual allocation"); return;
		}
		const bool changed = s.depthSource.get() != texture.get() || std::memcmp(&view, &s.lastDepthView, sizeof(view)) != 0;
		if (changed || !s.depthInput) {
			// ReShade's proxy and native device have different IUnknown identities.
			// Let D3D11 validate resource ownership, instead of rejecting that pair.
			s.depthInput = nullptr;
			if (FAILED(s.device->CreateShaderResourceView(texture.get(), &view, s.depthInput.put()))) {
				reject("cannot create depth view on scene device"); return;
			}
		}
		const BlitConstants cb{ { float(rect.right - rect.left) / desc.Width, float(rect.bottom - rect.top) / desc.Height },
			{ float(rect.left) / desc.Width, float(rect.top) / desc.Height }, 1, 0, 1, 0 };
		Blit(s, s.depthInput.get(), s.depth.rtv.get(), s.width, s.height, cb);
		s.depthReady = true;
		s.depthSource = texture; s.lastDepthView = view;
		s.depthFailureLogged = false;
	}

	void AdvanceRuntime(Scene& s)
	{
		// Even without a valid HDR pass, prevent this late Present from applying effects to the UI surface.
		if (!s.effectsIssued) {
			InvalidateDepth(s);
			s.runtime->render_effects(s.runtime->get_command_queue()->get_immediate_command_list(), {}, {});
		}
		const float transparent[4]{};
		s.context->ClearRenderTargetView(s.work.rtv.get(), transparent);
		Check(s.proxy->Present(0, 0), "Scene UI Present"); // Underlying native Present is still a no-op.
		s.effectsIssued = false;
	}
}

void SceneReShade::Initialize()
{
	// ReShade is optional; absence must not enable any bridge work in the engine hooks.
	if (!reshade::internal::get_reshade_module_handle()) return;
	if (initialized.exchange(true, std::memory_order_acq_rel)) return;
	HMODULE module{};
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&Initialize), &module);
	if (!reshade::register_addon(module)) {
		initialized.store(false, std::memory_order_release);
		logger::error("[Scene ReShade] Add-on registration failed; A compatible ReShade with full add-on support is required");
		return;
	}
	reshade::register_event<reshade::addon_event::init_device>(InitDevice);
	reshade::register_event<reshade::addon_event::init_swapchain>(InitSwapChain);
	reshade::register_event<reshade::addon_event::init_effect_runtime>(InitRuntime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(DestroyRuntime);
}

HRESULT SceneReShade::CreateOutputDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumLevel,
	ID3D12Device** device, ID3D12Device** addonLifetime)
{
	if (!device || !addonLifetime || device == addonLifetime) return E_POINTER;
	*device = nullptr; *addonLifetime = nullptr;
	com_ptr<ID3D12Device> native, created;
	struct CaptureScope {
		com_ptr<ID3D12Device>* previous = creatingOutputDevice;
		explicit CaptureScope(com_ptr<ID3D12Device>& target) { creatingOutputDevice = &target; }
		~CaptureScope() { creatingOutputDevice = previous; }
	} scope(native);
	const auto hr = D3D12CreateDevice(adapter, minimumLevel, IID_PPV_ARGS(created.put()));
	if (FAILED(hr)) return hr;
	if (native && native.get() != created.get()) {
		// Keep normal add-on device lifetime, but do not create queues/resources
		// through the ReShade device. SL/FG creates its own queues too, so merely
		// unwrapping our presentation queue would leave a second runtime in FG.
		*addonLifetime = created.detach();
		*device = native.detach();
	} else {
		*device = created.detach(); // ReShade absent, disabled or not wrapping this device.
	}
	return hr;
}

bool SceneReShade::IsAvailable() noexcept { return initialized.load(std::memory_order_acquire); }

bool SceneReShade::ShouldRenderHDR() noexcept
{
	if (!IsAvailable()) return false;
	const Entry entry;
	if (!entry.entered) return false;
	const auto* runtime = sceneRuntime.load(std::memory_order_acquire);
	return !runtime || runtime->get_effects_state();
}

void SceneReShade::Reset()
{
	if (IsAvailable()) resetRequested.store(true, std::memory_order_release);
}

void SceneReShade::RenderHDR(ID3D11Texture2D* color, ID3D11ShaderResourceView* depth, HWND window,
	UINT width, UINT height, const RECT* depthRect, std::uint64_t frame)
{
	if (!IsAvailable() || !color || !window) return;
	const Entry entry;
	if (!entry.entered) {
		if (!overlapLogged.exchange(true, std::memory_order_relaxed))
			logger::warn("[Scene ReShade] Overlapping bridge calls: skipped without waiting. Check hook scheduling");
		return;
	}
	try {
		if (resetRequested.exchange(false, std::memory_order_acq_rel)) { DestroyScene(); retryAfter = 0; }
		if (GetTickCount64() < retryAfter) return;
		// Off means no HDR/depth sampling, conversion, effect pass or writeback.
		// RenderUI advances input/menu so the user's shortcuts can re-enable it.
		if (auto* runtime = sceneRuntime.load(std::memory_order_acquire); runtime && !runtime->get_effects_state()) return;
		D3D11_TEXTURE2D_DESC desc{};
		color->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || !width || !height || width > desc.Width || height > desc.Height)
			throw std::runtime_error("Invalid HDR color extent or MSAA/array resource");
		if (desc.Format != DXGI_FORMAT_R11G11B10_FLOAT && desc.Format != kSceneFormat)
			throw std::runtime_error("RenderHDR requires an actual float HDR texture (26 or 10)");
		com_ptr<ID3D11Device> device;
		color->GetDevice(device.put());
		if (scene && (scene->width != width || scene->height != height || scene->window != window || !SameObject(scene->device.get(), device.get())))
			DestroyScene();
		if (!scene) CreateScene(device.get(), window, width, height);
		auto& s = *scene;
		if (!s.runtime || sceneRuntime.load(std::memory_order_acquire) != s.runtime)
			throw std::runtime_error("Scene runtime was destroyed outside its owner");
		if (!s.runtime->get_effects_state() || s.hdrFrame == frame) return;
		// A skipped late hook must not leave ReShade's render-once flag latched
		// across frames. Advance/discard the unfinished UI frame, never queue it.
		if (s.effectsIssued) {
			const ContextScope contextScope(s);
			AdvanceRuntime(s);
			if (!s.missingLateLogged) {
				s.missingLateLogged = true;
				logger::warn("[Scene ReShade] Previous frame had no LDR menu hook; discarded its UI, continuing with current HDR/depth");
			}
		}
		PrepareInput(s, color, desc.Format);
		{
			const ContextScope contextScope(s);
			const BlitConstants toFloat{ { float(width) / desc.Width, float(height) / desc.Height }, { 0, 0 }, kSceneToScRGBScale, 0, 0, 1 };
			Blit(s, s.inputSRV.get(), s.work.rtv.get(), width, height, toFloat);
			// This is the same immediate context after world depth writes. Unbind
			// DSV/UAVs via ContextScope, copy the active rect, and consume it now.
			// No NR-owned history, interop ownership transfer, fence or CPU wait.
			SnapshotDepth(s, depth, depthRect);
			s.mainDepth = s.depth.srv;
			const reshade::api::resource_view hdrRTV{ reinterpret_cast<std::uint64_t>(s.work.rtv.get()) };
			s.runtime->render_effects(s.runtime->get_command_queue()->get_immediate_command_list(), hdrRTV, hdrRTV);
			s.effectsIssued = true;
		}
		s.hdrFrame = frame;
		{
			const ContextScope contextScope(s);
			const BlitConstants fromFloat{ { 1, 1 }, { 0, 0 }, 1.0f / kSceneToScRGBScale, desc.Format == DXGI_FORMAT_R11G11B10_FLOAT ? 1.0f : 0.0f, 0, 1 };
			Blit(s, s.work.srv.get(), s.inputRTV.get(), width, height, fromFloat);
		}
	} catch (const std::exception& e) {
		logger::error("[Scene ReShade] HDR pass failed: {}", e.what());
		DestroyScene();
		retryAfter = GetTickCount64() + 5000;
	}
}

void SceneReShade::RenderUI(ID3D11RenderTargetView* target, HWND window, std::uint64_t frame)
{
	if (!IsAvailable()) return;
	if (!target || !window) return;
	const Entry entry;
	if (!entry.entered) {
		return;
	}
	try {
		if (resetRequested.exchange(false, std::memory_order_acq_rel)) { DestroyScene(); retryAfter = 0; }
		if (GetTickCount64() < retryAfter) return;
		com_ptr<ID3D11Resource> resource;
		target->GetResource(resource.put());
		const auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture) throw std::runtime_error("UI RTV does not reference a Texture2D");
		auto* color = texture.get(); // Retain through composition; never borrow RT0.texture.
		D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
		target->GetDesc(&targetDesc);
		if (targetDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || targetDesc.Texture2D.MipSlice != 0)
			throw std::runtime_error("UI requires a non-array, single-sample mip-zero RTV");
		D3D11_TEXTURE2D_DESC desc{};
		color->GetDesc(&desc);
		const auto width = desc.Width, height = desc.Height;
		// Late output is deliberately SDR. Never label LDR samples as HDR or
		// feed the menu through ImageSpaceEffectHDR/the engine's tonemapper.
		DXGI_FORMAT format{};
		switch (desc.Format) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
		default: throw std::runtime_error("LDR menu output requires RGBA8/BGRA8, not an HDR output surface");
		}
		if (!width || !height || desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
			throw std::runtime_error("Invalid LDR output extent");
		com_ptr<ID3D11Device> device;
		color->GetDevice(device.put());
		if (scene && (scene->window != window || !SameObject(scene->device.get(), device.get()))) DestroyScene();
		// Menus still work when an engine path does not run ImageSpaceEffectHDR.
		// In that case AdvanceRuntime explicitly skips effects (no old HDR reuse).
		if (!scene) CreateScene(device.get(), window, width, height);
		auto& s = *scene;
		if (!s.runtime || sceneRuntime.load(std::memory_order_acquire) != s.runtime)
			throw std::runtime_error("Scene runtime was destroyed outside its owner");
		if (s.uiFrame == frame) {
			return;
		}
		const ContextScope contextScope(s);
		if (s.hdrFrame != frame || !s.runtime->get_effects_state()) InvalidateDepth(s);
		if (s.outputColor.get() != color || !s.outputRTV) {
			s.outputRTV = nullptr; s.outputColor.copy_from(color);
			D3D11_RENDER_TARGET_VIEW_DESC view{};
			view.Format = format; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			Check(s.device->CreateRenderTargetView(color, &view, s.outputRTV.put()), "Create SDR menu output RTV");
		}
		AdvanceRuntime(s); // Effects already ran before tonemapping; only UI is drawn here.
		const BlitConstants ui{ { 1, 1 }, { 0, 0 }, 1, 0, 0, 0 };
		Blit(s, s.work.srv.get(), s.outputRTV.get(), width, height, ui, true);
		s.uiFrame = frame;
	} catch (const std::exception& e) {
		logger::error("[Scene ReShade] Menu pass failed: {}", e.what());
		DestroyScene();
		retryAfter = GetTickCount64() + 5000;
	}
}
