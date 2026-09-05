#include "ENBEffectDiagnostics.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ENBRenderDomain.h"
#include "ENB/ENBSeriesSDK.h"
#include "Upscaling.h"
#include "Util.h"

namespace
{
	// Verified against the supplied ENB 0.501 WrapperVersion/d3d11.dll in IDA.
	// SHA-256: D1F26F47DA0CA68BD6CF24C6E20C2DBB3953263D5CAA21142089EBBEEE433B9B.
	// These are ENB offsets, not game IDs; other 0.501 builds may differ.
	uintptr_t base = 0;
	HMODULE registeredModule = nullptr;
	bool installationAttempted = false;
	std::atomic<uint64_t> epoch{ 0 };
	std::atomic<uint64_t> captureNotBefore{ 0 };
	using Render = uint32_t (*)(void*, uint32_t, uint32_t, int32_t);
	using SetTexture = void (*)(void*, const char*, ID3D11ShaderResourceView*);
	using SetVector = void (*)(void*, const char*, const float*);
	using SetArray = void (*)(void*, const char*, const float*, uint32_t);
	using Apply = HRESULT (*)(void*, uint32_t, ID3D11DeviceContext*);
	REL::Trampoline drawTrampoline{ "ENBEffect draw call sites" };
	Render originalRender = nullptr;
	SetTexture originalTexture = nullptr;
	SetVector originalVector = nullptr;
	SetArray originalArray = nullptr;
	Apply originalApply = nullptr;
	bool installed = false;
	const char* installationFailure = "Capture hooks have not been installed. Restart with capture enabled.";

	struct PassInfo
	{
		void* pass = nullptr;
		const char* stage = nullptr;
		unsigned index = 0;
	};
	struct Scope
	{
		void* effect = nullptr;
		std::array<PassInfo, 64> passes{};
		const PassInfo* armedPass = nullptr;
		ID3D11DeviceContext* drawContext = nullptr;
		unsigned draws = 0;
		unsigned customDraws = 0;
		unsigned gammaDraws = 0;
		bool incomplete = false;
		ID3D11ShaderResourceView* adaptation = nullptr; // Borrowed only within synchronous render.
		bool sample = false;
		uint64_t frame = 0;
		uint64_t epoch = 0;
		unsigned invocation = 0;
		unsigned applyCount = 0;
		unsigned queued = 0;
		unsigned uiFound = 0;
		bool adaptationQueued = false;
	};
	thread_local Scope* active = nullptr;

	struct Readback
	{
		winrt::com_ptr<ID3D11DeviceContext> context;
		winrt::com_ptr<ID3D11Resource> staging;
		winrt::com_ptr<ID3D11Resource> source; // Retain through the GPU copy, including resize.
		winrt::com_ptr<ID3D11Query> ready;
		std::string name;
		uint32_t bytes = 0;
		uint32_t width = 0, height = 1, rowBytes = 0, sourceMip = 0;
		DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		unsigned polls = 0;
		uint64_t frame = 0;
		uint64_t epoch = 0;
	};
	thread_local std::vector<Readback> pending;
	thread_local uint64_t sequence = 0;
	thread_local uint64_t failedReadbackEpoch = UINT64_MAX;

	bool Enabled()
	{
		return Upscaling::GetSingleton()->settings.enbGPUTiming && ENBRenderDomain::Get().Active();
	}

	template <class F>
	F Method(void* a_object, size_t a_offset)
	{
		return reinterpret_cast<F>((*reinterpret_cast<uintptr_t**>(a_object))[a_offset / sizeof(void*)]);
	}

	void Drain()
	{
		for (auto it = pending.begin(); it != pending.end();) {
			if (++it->polls > 600) {
				failedReadbackEpoch = it->epoch;
				logger::warn("[ENBEffect] Readback timed out without waiting: {}", it->name);
				it = pending.erase(it);
				continue;
			}
			const auto status = it->context->GetData(it->ready.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (status == S_FALSE) {
				++it;
				continue;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const auto result = FAILED(status) ? status : it->context->Map(it->staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
				++it;
				continue;
			}
			if (SUCCEEDED(result)) {
				try {
					const auto directory = std::filesystem::temp_directory_path() / "Upscaling-ENBEffect" / std::to_string(GetCurrentProcessId());
					std::filesystem::create_directories(directory);
					const auto path = directory / (it->name + ".bin");
					std::ofstream output(path, std::ios::binary);
					// Texture rows can contain driver padding. Store tightly packed
					// original texels, not RowPitch * Height or a converted image.
					for (uint32_t row = 0; row < it->height; ++row) {
						output.write(static_cast<const char*>(mapped.pData) + size_t(row) * mapped.RowPitch, it->rowBytes);
					}
					output.close();
					std::ofstream metadata(directory / (it->name + ".json"));
					metadata << std::format("{{\"width\":{},\"height\":{},\"row_bytes\":{},\"bytes\":{},\"resource_format\":{},\"view_format\":{},\"source_mip\":{},\"epoch\":{},\"frame\":{},\"packing\":\"tight-rows\"}}",
						it->width, it->height, it->rowBytes, it->bytes, static_cast<unsigned>(it->format), static_cast<unsigned>(it->viewFormat), it->sourceMip, it->epoch, it->frame);
					metadata.close();
					if (!output || !metadata) {
						failedReadbackEpoch = it->epoch;
						logger::warn("[ENBEffect] Failed to write {}", path.string());
					} else {
						std::array<float, 4> first{};
						std::memcpy(first.data(), mapped.pData, std::min<size_t>(it->bytes, sizeof(first)));
						logger::info("[ENBEffect readback] file={} bytes={} dxgi-format={} raw-first-32bit-float4=({},{},{},{}); decode according to format (CB format=0)", path.string(), it->bytes, static_cast<unsigned>(it->format), first[0], first[1], first[2], first[3]);
					}
				} catch (const std::exception& e) {
					failedReadbackEpoch = it->epoch;
					logger::warn("[ENBEffect] Readback file failure: {}", e.what());
				}
				it->context->Unmap(it->staging.get(), 0);
			} else {
				failedReadbackEpoch = it->epoch;
				logger::warn("[ENBEffect] Readback failed hr=0x{:X}", static_cast<uint32_t>(result));
			}
			it = pending.erase(it);
		}
	}

	void Queue(ID3D11DeviceContext* a_context, ID3D11Resource* a_source, const char* a_name, unsigned a_slot,
		DXGI_FORMAT a_viewFormat = DXGI_FORMAT_UNKNOWN, unsigned a_mip = 0)
	{
		if (!a_source) { return; }
		const auto reject = [&](const char* reason) {
			active->incomplete = true;
			logger::warn("[ENBEffect] e={} f={} capture={}{} SKIPPED: {}", active->epoch, active->frame, a_name, a_slot, reason);
		};
		if (pending.size() >= 96 || a_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
			reject("queue full or deferred context");
			return;
		}
		winrt::com_ptr<ID3D11Device> device;
		a_context->GetDevice(device.put());
		Readback item;
		item.viewFormat = a_viewFormat;
		item.sourceMip = a_mip;
		winrt::com_ptr<ID3D11Buffer> buffer;
		winrt::com_ptr<ID3D11Texture2D> texture;
		HRESULT result = E_INVALIDARG;
		if (SUCCEEDED(a_source->QueryInterface(IID_PPV_ARGS(buffer.put())))) {
			D3D11_BUFFER_DESC desc{};
			buffer->GetDesc(&desc);
			if (!desc.ByteWidth || desc.ByteWidth > 65536) { reject("unsupported buffer size"); return; }
			item.bytes = desc.ByteWidth;
			item.rowBytes = item.bytes;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Buffer> staging;
			result = device->CreateBuffer(&desc, nullptr, staging.put());
			if (staging) { item.staging.copy_from(staging.get()); }
		} else if (SUCCEEDED(a_source->QueryInterface(IID_PPV_ARGS(texture.put())))) {
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			item.format = desc.Format;
			if (desc.ArraySize != 1 || a_mip >= desc.MipLevels || desc.SampleDesc.Count != 1) { reject("array/MSAA/invalid mip unsupported"); return; }
			unsigned pixelBytes = 0;
			switch (desc.Format) {
			case DXGI_FORMAT_R8_UNORM: pixelBytes = 1; break;
			case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_TYPELESS: pixelBytes = 2; break;
			case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT: pixelBytes = 4; break;
			case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_UNORM: pixelBytes = 8; break;
			case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: pixelBytes = 16; break;
			default: reject("unsupported texture format"); return;
			}
			desc.Width = std::max(1u, desc.Width >> a_mip);
			desc.Height = std::max(1u, desc.Height >> a_mip);
			item.width = desc.Width;
			item.height = desc.Height;
			item.rowBytes = desc.Width * pixelBytes;
			uint64_t queuedBytes = 0;
			for (const auto& entry : pending) { queuedBytes += entry.bytes; }
			const auto bytes = uint64_t(item.rowBytes) * desc.Height;
			if (bytes + queuedBytes > 256ull * 1024 * 1024) { reject("256 MiB readback budget"); return; }
			item.bytes = static_cast<uint32_t>(bytes);
			desc.MipLevels = 1;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Texture2D> staging;
			result = device->CreateTexture2D(&desc, nullptr, staging.put());
			if (staging) { item.staging.copy_from(staging.get()); }
		}
		if (FAILED(result) || !item.staging) { reject("staging allocation failed"); return; }
		const D3D11_QUERY_DESC query{ D3D11_QUERY_EVENT, 0 };
		if (FAILED(device->CreateQuery(&query, item.ready.put()))) { reject("query allocation failed"); return; }
		item.context.copy_from(a_context);
		item.source.copy_from(a_source);
		item.frame = active->frame;
		item.epoch = active->epoch;
		item.name = std::format("e{}-f{}-call{}-pass{}-{}{}-{}", active->epoch, active->frame, active->invocation, active->applyCount, a_name, a_slot, sequence++);
		if (texture) { a_context->CopySubresourceRegion(item.staging.get(), 0, 0, 0, 0, a_source, a_mip, nullptr); }
		else { a_context->CopyResource(item.staging.get(), a_source); }
		a_context->End(item.ready.get());
		pending.push_back(std::move(item));
		++active->queued;
		if (std::strcmp(a_name, "TextureAdaptation") == 0) { active->adaptationQueued = true; }
	}

	void Resource(const char* a_name, ID3D11ShaderResourceView* a_view)
	{
		if (!a_view) {
			logger::info("[ENBEffect resource] e={} f={} name={} null", active->epoch, active->frame, a_name);
			return;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		a_view->GetDesc(&view);
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		D3D11_TEXTURE2D_DESC texture{};
		if (auto tex = resource.try_as<ID3D11Texture2D>()) { tex->GetDesc(&texture); }
		logger::info("[ENBEffect resource] e={} f={} name={} srv={} resource={} size={}x{} tex-format={} view-format={} dimension={}",
			active->epoch, active->frame, a_name, static_cast<void*>(a_view), static_cast<void*>(resource.get()), texture.Width, texture.Height,
			static_cast<unsigned>(texture.Format), static_cast<unsigned>(view.Format), static_cast<unsigned>(view.ViewDimension));
	}

	void CaptureView(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view, const char* name, unsigned slot)
	{
		if (!view) { return; }
		Resource(name, view);
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		view->GetDesc(&desc);
		if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
			active->incomplete = true;
			logger::warn("[ENBEffect] {}{} unsupported SRV dimension={}", name, slot, static_cast<unsigned>(desc.ViewDimension));
			return;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		view->GetResource(resource.put());
		Queue(context, resource.get(), name, slot, desc.Format, desc.Texture2D.MostDetailedMip);
	}

	void CaptureTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const char* name, unsigned slot)
	{
		if (!view) { return; }
		D3D11_RENDER_TARGET_VIEW_DESC desc{};
		view->GetDesc(&desc);
		winrt::com_ptr<ID3D11Resource> resource;
		view->GetResource(resource.put());
		logger::info("[ENBEffect target] e={} f={} name={}{} rtv={} resource={} view-format={} dimension={}",
			active->epoch, active->frame, name, slot, static_cast<void*>(view), static_cast<void*>(resource.get()),
			static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.ViewDimension));
		if (desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) { active->incomplete = true; return; }
		Queue(context, resource.get(), name, slot, desc.Format, desc.Texture2D.MipSlice);
	}

	void DrawState(ID3D11DeviceContext* context, const char* stage)
	{
		winrt::com_ptr<ID3D11BlendState> blend;
		float factors[4]{};
		UINT mask = 0;
		context->OMGetBlendState(blend.put(), factors, &mask);
		D3D11_BLEND_DESC blendDesc{};
		if (blend) { blend->GetDesc(&blendDesc); }
		else {
			blendDesc.RenderTarget[0].SrcBlend = blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlend = blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			blendDesc.RenderTarget[0].BlendOp = blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		const auto& rt = blendDesc.RenderTarget[0];
		logger::info("[ENBEffect state] e={} f={} stage={} blend={} default={} alpha-to-coverage={} independent={} enabled={} color={}/{}/{} alpha={}/{}/{} write-mask={} factor=({},{},{},{}) sample-mask={:08X}",
			active->epoch, active->frame, stage, static_cast<void*>(blend.get()), !blend, blendDesc.AlphaToCoverageEnable, blendDesc.IndependentBlendEnable,
			rt.BlendEnable, static_cast<unsigned>(rt.SrcBlend), static_cast<unsigned>(rt.DestBlend), static_cast<unsigned>(rt.BlendOp),
			static_cast<unsigned>(rt.SrcBlendAlpha), static_cast<unsigned>(rt.DestBlendAlpha), static_cast<unsigned>(rt.BlendOpAlpha), rt.RenderTargetWriteMask,
			factors[0], factors[1], factors[2], factors[3], mask);
		winrt::com_ptr<ID3D11DepthStencilState> depth;
		winrt::com_ptr<ID3D11DepthStencilView> dsv;
		UINT stencilRef = 0;
		context->OMGetDepthStencilState(depth.put(), &stencilRef);
		context->OMGetRenderTargets(0, nullptr, dsv.put());
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		if (depth) { depth->GetDesc(&depthDesc); }
		else { depthDesc.DepthEnable = TRUE; depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; depthDesc.DepthFunc = D3D11_COMPARISON_LESS; }
		logger::info("[ENBEffect state] e={} f={} stage={} depth-state={} default={} dsv={} depth-enable={} write={} func={} stencil={} ref={}",
			active->epoch, active->frame, stage, static_cast<void*>(depth.get()), !depth, static_cast<void*>(dsv.get()), depthDesc.DepthEnable,
			static_cast<unsigned>(depthDesc.DepthWriteMask), static_cast<unsigned>(depthDesc.DepthFunc), depthDesc.StencilEnable, stencilRef);
		winrt::com_ptr<ID3D11RasterizerState> raster;
		context->RSGetState(raster.put());
		D3D11_RASTERIZER_DESC rasterDesc{};
		if (raster) { raster->GetDesc(&rasterDesc); }
		else { rasterDesc.CullMode = D3D11_CULL_BACK; rasterDesc.FillMode = D3D11_FILL_SOLID; }
		logger::info("[ENBEffect state] e={} f={} stage={} raster={} default={} scissor-enable={} cull={} fill={}", active->epoch, active->frame, stage,
			static_cast<void*>(raster.get()), !raster, rasterDesc.ScissorEnable, static_cast<unsigned>(rasterDesc.CullMode), static_cast<unsigned>(rasterDesc.FillMode));
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
		std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
		UINT count = static_cast<UINT>(viewports.size());
		context->RSGetViewports(&count, viewports.data());
		for (UINT i = 0; i < count; ++i) {
			const auto& v = viewports[i];
			logger::info("[ENBEffect viewport] stage={} index={} xy=({}, {}) size={}x{} depth={}..{}", stage, i, v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth);
		}
		count = static_cast<UINT>(scissors.size());
		context->RSGetScissorRects(&count, scissors.data());
		for (UINT i = 0; i < count; ++i) {
			const auto& r = scissors[i];
			logger::info("[ENBEffect scissor] stage={} index={} rect=({}, {}, {}, {})", stage, i, r.left, r.top, r.right, r.bottom);
		}
	}

	void TraceDrawIndexed(ID3D11DeviceContext* context, UINT count, UINT start, INT vertexBase, const char* stage)
	{
		if (!active || !active->sample) {
			context->DrawIndexed(count, start, vertexBase);
			return;
		}
		const PassInfo pass{ nullptr, stage, active->armedPass ? active->armedPass->index : 0 };
		logger::info("[ENBEffect boundary-context] e={} f={} stage={} apply-context={} draw-context={} apply-matched={}",
			active->epoch, active->frame, stage, static_cast<void*>(active->drawContext), static_cast<void*>(context),
			active->armedPass && std::strcmp(active->armedPass->stage, stage) == 0);
		active->armedPass = nullptr;
		if (++active->draws > 16) {
			active->incomplete = true;
			context->DrawIndexed(count, start, vertexBase);
			return;
		}
		std::array<winrt::com_ptr<ID3D11RenderTargetView>, 8> targets;
		std::array<ID3D11RenderTargetView*, 8> rawTargets{};
		try {
			context->OMGetRenderTargets(static_cast<UINT>(rawTargets.size()), rawTargets.data(), nullptr);
			for (unsigned i = 0; i < targets.size(); ++i) { targets[i].attach(rawTargets[i]); }
			logger::info("[ENBEffect draw] e={} f={} invocation={} draw={} stage={} pass={} index-count={} start={} base={}",
				active->epoch, active->frame, active->invocation, active->draws, pass.stage, pass.index, count, start, vertexBase);
			DrawState(context, pass.stage);
			// Internal shader registers verified in ENB 0.501's embedded source:
			// PBMask t0/t3; GammaFix t0; GammaFixD t0/t1. Custom shaders vary:
			// capture t0..t3 as candidate inputs, not proof of shader usage.
			for (unsigned i = 0; i < 4; ++i) {
				if (std::strcmp(pass.stage, "PBMask") == 0 && i != 0 && i != 3) { continue; }
				if (std::strcmp(pass.stage, "GammaFix") == 0 && i != 0) { continue; }
				if (std::strcmp(pass.stage, "GammaFixD") == 0 && i > 1) { continue; }
				winrt::com_ptr<ID3D11ShaderResourceView> view;
				context->PSGetShaderResources(i, 1, view.put());
				if (!view && i == 0) { active->incomplete = true; }
				CaptureView(context, view.get(), std::format("{}-input-PS", pass.stage).c_str(), i);
			}
			if (std::strcmp(pass.stage, "custom") == 0) {
				winrt::com_ptr<ID3D11ShaderResourceView> adaptation;
				context->VSGetShaderResources(0, 1, adaptation.put());
				CaptureView(context, adaptation.get(), "custom-input-VS", 0);
			}
			for (unsigned i = 0; i < targets.size(); ++i) {
				CaptureTarget(context, targets[i].get(), std::format("{}-before-RT", pass.stage).c_str(), i);
			}
			if (!targets[0]) { active->incomplete = true; }
		} catch (const std::exception& e) {
			active->incomplete = true;
			logger::warn("[ENBEffect] Before-draw capture failed: {}", e.what());
		}
		context->DrawIndexed(count, start, vertexBase);
		try {
			for (unsigned i = 0; i < targets.size(); ++i) {
				CaptureTarget(context, targets[i].get(), std::format("{}-after-RT", pass.stage).c_str(), i);
			}
			if (std::strcmp(pass.stage, "custom") == 0) { ++active->customDraws; }
			if (std::strncmp(pass.stage, "GammaFix", 8) == 0) { ++active->gammaDraws; }
		} catch (const std::exception& e) {
			active->incomplete = true;
			logger::warn("[ENBEffect] After-draw capture failed: {}", e.what());
		}
	}

	template <unsigned Stage>
	void STDMETHODCALLTYPE DrawAtCallSite(ID3D11DeviceContext* context, UINT count, UINT start, INT vertexBase)
	{
		const char* stage = Stage == 0 ? "custom" : Stage == 1 ? "PBMask" :
			*reinterpret_cast<uint32_t*>(base + 0x1A65A0) == 1 ? "GammaFixD" : "GammaFix";
		TraceDrawIndexed(context, count, start, vertexBase, stage);
	}

	bool InstallDrawCallSites()
	{
		// ENB 0.501: MOV EDX,R14D (3 bytes); CALL [RAX+60h] (3 bytes).
		// Replace the pair with CALL [RIP+disp32]. A leaf shim restores EDX
		// then tail-jumps to C++; the existing return address, shadow space,
		// stack alignment and following INC EBX/EDI are unchanged.
		constexpr std::array<uintptr_t, 3> offsets{ 0x79156, 0x792E5, 0x79455 };
		constexpr std::array<uint8_t, 6> expected{ 0x41, 0x8B, 0xD6, 0xFF, 0x50, 0x60 };
		const std::array<uintptr_t, 3> functions{
			reinterpret_cast<uintptr_t>(&DrawAtCallSite<0>), reinterpret_cast<uintptr_t>(&DrawAtCallSite<1>), reinterpret_cast<uintptr_t>(&DrawAtCallSite<2>) };
		for (const auto offset : offsets) {
			if (!stl::is_readable_memory(base + offset, expected.size()) ||
				std::memcmp(reinterpret_cast<void*>(base + offset), expected.data(), expected.size()) != 0) {
				logger::error("[ENBEffect] Draw call-site signature mismatch RVA={:X}; capture disabled", offset);
				return false;
			}
		}
		// Allocate beside ENB, not the game's F4SE trampoline (+/-2 GiB).
		drawTrampoline.create(128, reinterpret_cast<void*>(base + offsets[0]));
		std::array<std::array<uint8_t, 6>, 3> patches{};
		for (unsigned i = 0; i < offsets.size(); ++i) {
			auto* shim = static_cast<uint8_t*>(drawTrampoline.allocate(32));
			constexpr uint8_t prefix[]{ 0x41, 0x8B, 0xD6, 0xFF, 0x25, 0, 0, 0, 0 };
			std::memcpy(shim, prefix, sizeof(prefix));
			std::memcpy(shim + sizeof(prefix), &functions[i], sizeof(uintptr_t));
			auto* slot = reinterpret_cast<uintptr_t*>(shim + 24);
			*slot = reinterpret_cast<uintptr_t>(shim);
			const auto delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(slot)) - static_cast<int64_t>(base + offsets[i] + 6);
			if (delta < INT32_MIN || delta > INT32_MAX) { return false; }
			const auto displacement = static_cast<int32_t>(delta);
			patches[i][0] = 0xFF;
			patches[i][1] = 0x15;
			std::memcpy(patches[i].data() + 2, &displacement, sizeof(displacement));
			FlushInstructionCache(GetCurrentProcess(), shim, 17);
		}
		for (unsigned i = 0; i < offsets.size(); ++i) {
			if (!Detours::DetourCopyMemory(base + offsets[i], reinterpret_cast<uintptr_t>(patches[i].data()), 6)) {
				logger::error("[ENBEffect] Draw call-site write failed RVA={:X}; sampling disabled", offsets[i]);
				return false;
			}
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(base + offsets[i]), 6);
		}
		logger::info("[ENBEffect] Boundary capture v2: 3 ENB draw call sites installed; context vtables untouched");
		return true;
	}

	HRESULT ApplyPass(void* a_pass, uint32_t a_flags, ID3D11DeviceContext* a_context)
	{
		if (active) { active->armedPass = nullptr; }
		const auto result = originalApply(a_pass, a_flags, a_context);
		if (!active || !active->sample || FAILED(result)) { return result; }
		const auto pass = std::find_if(active->passes.begin(), active->passes.end(), [&](const PassInfo& entry) { return entry.pass == a_pass; });
		if (pass == active->passes.end()) { return result; }
		++active->applyCount;
		if (active->applyCount > 16) { active->incomplete = true; return result; }
		active->armedPass = &*pass;
		active->drawContext = a_context;
		// Pass::Apply has completed; these are the bindings used by the following draw.
		winrt::com_ptr<ID3D11VertexShader> vs;
		winrt::com_ptr<ID3D11PixelShader> ps;
		a_context->VSGetShader(vs.put(), nullptr, nullptr);
		a_context->PSGetShader(ps.put(), nullptr, nullptr);
		logger::info("[ENBEffect pass] e={} f={} pass={} stage={} technique-pass={} VS={} PS={} context={}", active->epoch, active->frame, active->applyCount, pass->stage, pass->index,
			static_cast<void*>(vs.get()), static_cast<void*>(ps.get()), static_cast<void*>(a_context));
		for (unsigned i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i) {
			winrt::com_ptr<ID3D11Buffer> vsBuffer, psBuffer;
			a_context->VSGetConstantBuffers(i, 1, vsBuffer.put());
			a_context->PSGetConstantBuffers(i, 1, psBuffer.put());
			Queue(a_context, vsBuffer.get(), "VS-CB", i);
			Queue(a_context, psBuffer.get(), "PS-CB", i);
		}
		for (unsigned i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) {
			winrt::com_ptr<ID3D11ShaderResourceView> vsView, psView;
			a_context->VSGetShaderResources(i, 1, vsView.put());
			a_context->PSGetShaderResources(i, 1, psView.put());
			if (vsView) { Resource(std::format("VS-SRV{}", i).c_str(), vsView.get()); }
			if (psView) { Resource(std::format("PS-SRV{}", i).c_str(), psView.get()); }
		}
		if (active->adaptation) {
			winrt::com_ptr<ID3D11Resource> adaptation;
			active->adaptation->GetResource(adaptation.put());
			Queue(a_context, adaptation.get(), "TextureAdaptation", 0);
		}
		return result;
	}

	void Texture(void* a_effect, const char* a_name, ID3D11ShaderResourceView* a_view)
	{
		originalTexture(a_effect, a_name, a_view);
		if (active && active->sample && active->effect == a_effect && a_name) {
			Resource(a_name, a_view);
			if (std::strcmp(a_name, "TextureAdaptation") == 0) { active->adaptation = a_view; }
		}
	}

	void Values(void* a_effect, const char* a_name, const float* a_values, uint32_t a_count)
	{
		if (!active || !active->sample || active->effect != a_effect || !a_name || !a_values) { return; }
		for (unsigned i = 0; i < std::min(a_count, 16u); ++i) {
			logger::info("[ENBEffect value] e={} f={} {}[{}]=({},{},{},{})", active->epoch, active->frame,
				a_name, i, a_values[i * 4], a_values[i * 4 + 1], a_values[i * 4 + 2], a_values[i * 4 + 3]);
		}
	}
	void Vector(void* a_effect, const char* a_name, const float* a_values)
	{
		originalVector(a_effect, a_name, a_values);
		Values(a_effect, a_name, a_values, 1);
	}
	void Array(void* a_effect, const char* a_name, const float* a_values, uint32_t a_count)
	{
		originalArray(a_effect, a_name, a_values, a_count);
		Values(a_effect, a_name, a_values, a_count);
	}

	void RegisterPasses(Scope& scope, void* technique, const char* stage)
	{
		if (!technique || !Method<bool (*)(void*)>(technique, 0)(technique)) { return; }
		for (unsigned i = 0; i < 16; ++i) {
			auto* pass = Method<void* (*)(void*, unsigned)>(technique, 0x20)(technique, i);
			if (!pass || !Method<bool (*)(void*)>(pass, 0)(pass)) { break; }
			const auto free = std::find_if(scope.passes.begin(), scope.passes.end(), [](const PassInfo& entry) { return !entry.pass; });
			if (free == scope.passes.end() || Method<Apply>(pass, 0x50) != &ApplyPass) { scope.incomplete = true; break; }
			*free = { pass, stage, i };
		}
	}

	uint32_t Effect(void* a_this, uint32_t a_count, uint32_t a_start, int32_t a_base)
	{
		if (!installed || active) { return originalRender(a_this, a_count, a_start, a_base); }
		Drain();
		if (!Enabled()) { return originalRender(a_this, a_count, a_start, a_base); }
		const auto deadline = captureNotBefore.load();
		if (deadline) {
			if (GetTickCount64() < deadline) { return originalRender(a_this, a_count, a_start, a_base); }
			if (captureNotBefore.exchange(0)) {
				logger::info("[ENBEffect capture] Manual capture started e={}", ++epoch);
			}
		}
		Scope scope;
		scope.frame = Util::State_GetSingleton()->frameCount;
		scope.epoch = epoch.load();
		static thread_local uint64_t sampledEpoch = UINT64_MAX, firstFrame = 0, nextAttempt = 0;
		static thread_local unsigned samples = 0;
		static thread_local unsigned attempts = 0;
		static thread_local uint64_t callFrame = UINT64_MAX, callEpoch = UINT64_MAX;
		static thread_local unsigned invocations = 0;
		static thread_local bool sampledFrame = false;
		if (scope.frame != callFrame || scope.epoch != callEpoch) {
			if (sampledFrame) {
				logger::info("[ENBEffect frame] e={} f={} effect-function-invocations={} (not a count of successful draws)", callEpoch, callFrame, invocations);
			}
			callFrame = scope.frame;
			callEpoch = scope.epoch;
			invocations = 0;
			sampledFrame = false;
		}
		scope.invocation = ++invocations;
		if (sampledEpoch != scope.epoch) {
			sampledEpoch = scope.epoch;
			firstFrame = nextAttempt = scope.frame;
			samples = attempts = 0;
		}
		if (failedReadbackEpoch == scope.epoch) {
			logger::warn("[ENBEffect] Epoch {} readback/file failure; invalidating sample budget and scheduling bounded retry", scope.epoch);
			failedReadbackEpoch = UINT64_MAX;
			samples = 0;
		}
		const bool newSample = scope.invocation == 1 && attempts < 10 && scope.frame >= nextAttempt &&
			(samples == 0 || (samples == 1 && scope.frame - firstFrame >= 120));
		if (newSample) {
			++attempts;
			nextAttempt = scope.frame + 120;
			sampledFrame = true;
			logger::info("[ENBEffect attempt] e={} f={} attempt={} queued-samples={}", scope.epoch, scope.frame, attempts, samples);
		}
		scope.sample = sampledFrame && scope.invocation <= 4;
		if (!scope.sample) { return originalRender(a_this, a_count, a_start, a_base); }
		bool reported = false;
		struct ReportMissing
		{
			Scope& scope;
			bool& reported;
			~ReportMissing()
			{
				if (!reported) {
					logger::warn("[ENBEffect] e={} f={} collection=INCOMPLETE: effect/technique unavailable; sample budget preserved", scope.epoch, scope.frame);
				}
			}
		} reportMissing{ scope, reported };
		scope.effect = reinterpret_cast<void* (*)(int)>(base + 0x403F0)(6);
		if (!scope.effect) { return originalRender(a_this, a_count, a_start, a_base); }
		auto* bytes = static_cast<std::byte*>(scope.effect);
		const auto total = *reinterpret_cast<uint32_t*>(bytes + 0x23E930);
		auto index = *reinterpret_cast<uint32_t*>(bytes + 0x2BE934);
		if (index >= total) { index = 0; }
		if (!total || index >= 2048) { return originalRender(a_this, a_count, a_start, a_base); }
		const auto* name = reinterpret_cast<const char*>(bytes + 0x23E934 + index * 256);
		const std::string selected(name, strnlen(name, 256));
		const bool original = *reinterpret_cast<uint32_t*>(base + 0x1A6588) == 1;
		void* effectObject = *reinterpret_cast<void**>(scope.effect);
		if (!effectObject) { return originalRender(a_this, a_count, a_start, a_base); }
		using GetTechnique = void* (*)(void*, const char*);
		auto* technique = Method<GetTechnique>(effectObject, 0x70)(effectObject, selected.c_str());
		if (original) {
			auto* overrideTechnique = Method<GetTechnique>(effectObject, 0x70)(effectObject, "ORIGINALPOSTPROCESS");
			if (overrideTechnique && Method<bool (*)(void*)>(overrideTechnique, 0)(overrideTechnique)) { technique = overrideTechnique; }
		}
		const bool techniqueValid = technique && Method<bool (*)(void*)>(technique, 0)(technique);
		RegisterPasses(scope, technique, "custom");
		if (auto* internal = reinterpret_cast<void* (*)(int)>(base + 0x403F0)(0)) {
			if (auto* internalObject = *reinterpret_cast<void**>(internal)) {
				for (const auto* stage : { "PBMask", "GammaFix", "GammaFixD" }) {
					RegisterPasses(scope, Method<GetTechnique>(internalObject, 0x70)(internalObject, stage), stage);
				}
			}
		}
		logger::info("[ENBEffect begin] e={} f={} sample={} invocation={} technique-index={} technique={} original-post={} use-effect={} original-adaptation={} enable-adaptation={}",
			scope.epoch, scope.frame, samples, scope.invocation, index, selected, original, *reinterpret_cast<uint32_t*>(base + 0x1A6580),
			*reinterpret_cast<uint32_t*>(base + 0x1A6594), *reinterpret_cast<uint32_t*>(base + 0x1A65A4));
		struct Reset { ~Reset() { active = nullptr; } } reset;
		active = &scope;
		// Read both the SDK registry and the actual compiled effect. Registry lookup
		// failure must not hide live values or substitute values from the INI.
		const auto get = reinterpret_cast<ENB_SDK::_ENBGetParameterA>(GetProcAddress(reinterpret_cast<HMODULE>(base), "ENBGetParameter"));
		{
			struct Parameter { const char* label; const char* symbol; };
			constexpr Parameter keys[]{
				{ "|- Day - Exposure (F-Stops)", "Day_UIHCG_Exposure" },
				{ "|- Day - Contrast", "Day_UIHCG_Contrast" },
				{ "|- Day - Saturation", "Day_UIHCG_Saturation" },
				{ "|- Day - Min Adapt", "Day_UI_AdaptationMin" },
				{ "|- Day - Max Adapt", "Day_UI_AdaptationMax" },
				{ "|- Day - Exposure Bias", "Day_UITM_ExposureBias" },
				{ "Disable ADAPTATION", "ENABLE_ADPATATIONOFF" },
				{ "Use the Night eye enhancer", "ENABLE_NightEyefix" }
			};
			for (const auto& key : keys) {
				ENB_SDK::ENBParameter parameter{};
				// ENB's registry compares category names case-sensitively.
				const bool found = get && get(nullptr, "ENBEFFECT.FX", key.label, &parameter);
				std::array<uint32_t, 4> words{};
				std::memcpy(words.data(), parameter.Data, sizeof(words));
				logger::info("[ENBEffect UI] e={} f={} key={} found={} type={} bytes={} raw={:08X},{:08X},{:08X},{:08X}",
					scope.epoch, scope.frame, key.label, found, static_cast<long>(parameter.Type), parameter.Size, words[0], words[1], words[2], words[3]);
				// Effects11 variable slots: IsValid=0, GetRawValue=27 (0xD8).
				// GetVariableByName=0x48 is also used by ENB's own named setters.
				auto* variable = Method<void* (*)(void*, const char*)>(effectObject, 0x48)(effectObject, key.symbol);
				uint32_t raw = 0;
				HRESULT result = E_FAIL;
				if (variable && Method<bool (*)(void*)>(variable, 0)(variable)) {
					result = Method<HRESULT (*)(void*, void*, uint32_t, uint32_t)>(variable, 0xD8)(variable, &raw, 0, sizeof(raw));
				}
				float value = 0;
				std::memcpy(&value, &raw, sizeof(value));
				scope.uiFound += SUCCEEDED(result) ? 1u : 0u;
				logger::info("[ENBEffect live-variable] e={} f={} symbol={} hr=0x{:X} raw={:08X} as-float={} (boolean variables use raw)",
					scope.epoch, scope.frame, key.symbol, static_cast<uint32_t>(result), raw, value);
			}
		}
		const auto result = originalRender(a_this, a_count, a_start, a_base);
		// Optimized-out UI constants/adaptation are valid in bypass shaders.
		// Usability is determined by actual draw boundaries and successful queues.
		const bool fallback = !techniqueValid && result == 0 && scope.draws == 0;
		const bool collected = !scope.incomplete && scope.customDraws && scope.gammaDraws && scope.queued;
		if (newSample && (collected || fallback)) {
			if (samples == 0) { firstFrame = scope.frame; }
			++samples;
		}
		reported = true;
		logger::info("[ENBEffect boundaries] e={} f={} valid-technique={} custom-draws={} gamma-draws={} all-tracked-draws={} status={}",
			scope.epoch, scope.frame, techniqueValid, scope.customDraws, scope.gammaDraws, scope.draws,
			fallback ? "FALLBACK-NO-CUSTOM-DRAW (game fallback output not captured)" : collected ? "QUEUED-check-readback-files" : "INCOMPLETE");
		logger::info("[ENBEffect end] e={} f={} result={} captured-passes={} queued-readbacks={} adaptation-queued={} live-variables={}/8 collection={} (queued files still require readback confirmation)",
			scope.epoch, scope.frame, result, scope.applyCount, scope.queued, scope.adaptationQueued, scope.uiFound, fallback ? "FALLBACK" : collected ? "queued" : "INCOMPLETE");
		if (newSample && !collected && !fallback && attempts == 10) {
			logger::error("[ENBEffect] Collection retries exhausted for epoch {}; do not treat this epoch as a usable baseline", scope.epoch);
		}
		return result;
	}
}

void ENBEffectDiagnostics::BeforeResize()
{
	if (installed && Enabled()) {
		logger::info("[ENBEffect resize] e={} use-effect={} original-post={} original-adaptation={} enable-adaptation={}", epoch.load(),
			*reinterpret_cast<uint32_t*>(base + 0x1A6580), *reinterpret_cast<uint32_t*>(base + 0x1A6588),
			*reinterpret_cast<uint32_t*>(base + 0x1A6594), *reinterpret_cast<uint32_t*>(base + 0x1A65A4));
	}
	++epoch;
}

void ENBEffectDiagnostics::RegisterModule(HMODULE a_module)
{
	registeredModule = a_module;
}

const char* ENBEffectDiagnostics::CaptureUnavailableReason()
{
	if (!installed) { return installationFailure; }
	if (!Upscaling::GetSingleton()->settings.enbGPUTiming) { return "Enable ENB GPU / Post-Processing Capture before requesting a capture."; }
	if (!ENBRenderDomain::Get().Active()) { return "Capture requires the active ENB render domain."; }
	return nullptr;
}

bool ENBEffectDiagnostics::RequestCapture()
{
	if (const auto* reason = CaptureUnavailableReason()) {
		logger::warn("[ENBEffect capture] Unavailable: {}", reason);
		return false;
	}
	captureNotBefore.store(GetTickCount64() + 3000);
	logger::info("[ENBEffect capture] Requested; close menus within 3 seconds. Captures first draw frame and another 120 frames later");
	return true;
}

void ENBEffectDiagnostics::Install()
{
	// Registration runs before settings are loaded. Do not inspect the opt-in
	// there, or its default zero silently prevents installation for this session.
	if (installationAttempted) { return; }
	installationAttempted = true;
	const auto a_module = registeredModule;
	const bool enabled = Upscaling::GetSingleton()->settings.enbGPUTiming != 0;
	logger::info("[ENBEffect] Install after settings load: bENBGPUTiming={} module={}", enabled, static_cast<void*>(a_module));
	if (!a_module || !enabled) {
		installationFailure = a_module ? "Capture was disabled at startup. Enable capture and restart." : "ENB module was unavailable at startup.";
		logger::info("[ENBEffect] Diagnostic hooks NOT installed: {}", a_module ? "disabled in startup settings" : "ENB module unavailable");
		return;
	}
	base = reinterpret_cast<uintptr_t>(a_module);
	if (reinterpret_cast<uintptr_t>(GetProcAddress(a_module, "D3D11CreateDeviceAndSwapChain")) != base + 0x2D230) {
		installationFailure = "This ENB binary does not match the supported capture layout. See Upscaling.log.";
		logger::warn("[ENBEffect] Unsupported ENB layout; diagnostic hooks NOT installed (validated: 0.501)");
		return;
	}
	installationFailure = "Capture hook validation or installation failed. See Upscaling.log for the failing address.";
	struct Signature { uintptr_t offset; std::array<uint8_t, 8> bytes; };
	constexpr Signature signatures[]{
		{ 0x78AD0, { 0x40, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 } },
		{ 0x40250, { 0x48, 0x85, 0xD2, 0x74, 0x41, 0x53, 0x48, 0x83 } },
		{ 0x40340, { 0x48, 0x85, 0xD2, 0x74, 0x3E, 0x53, 0x48, 0x83 } },
		{ 0x40390, { 0x48, 0x85, 0xD2, 0x74, 0x51, 0x48, 0x89, 0x5C } },
		{ 0x403F0, { 0x83, 0xF9, 0x40, 0x7C, 0x03, 0x33, 0xC0, 0xC3 } },
		{ 0x101198, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B } },
		{ 0x79156, { 0x41, 0x8B, 0xD6, 0xFF, 0x50, 0x60, 0xFF, 0xC3 } },
		{ 0x792E5, { 0x41, 0x8B, 0xD6, 0xFF, 0x50, 0x60, 0xFF, 0xC3 } },
		{ 0x79455, { 0x41, 0x8B, 0xD6, 0xFF, 0x50, 0x60, 0xFF, 0xC7 } }
	};
	for (const auto& signature : signatures) {
		if (!stl::is_readable_memory(base + signature.offset, signature.bytes.size()) ||
			std::memcmp(reinterpret_cast<void*>(base + signature.offset), signature.bytes.data(), signature.bytes.size()) != 0) {
			logger::warn("[ENBEffect] Signature mismatch at 0x{:X}; diagnostic hooks NOT installed", signature.offset);
			return;
		}
	}
	// Verified CEffectPass vtable in 0.501: Apply is slot 0x50 at RVA 0x13EBF8.
	// Patch the pointer once, before collecting any baseline. No rel32 trampoline
	// is needed, and replacing effect instances during resize keeps this vtable.
	const auto slotAddress = base + 0x13EBF8;
	if (!stl::is_readable_memory(slotAddress, sizeof(void*)) ||
		*reinterpret_cast<uintptr_t*>(slotAddress) != base + 0x101198) {
		logger::error("[ENBEffect] Pass::Apply vtable validation failed; sampling NOT installed");
		return;
	}
	DWORD protection = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(slotAddress), sizeof(void*), PAGE_READWRITE, &protection)) {
		logger::error("[ENBEffect] Pass::Apply vtable protection failed error={}; sampling NOT installed", GetLastError());
		return;
	}
	originalApply = reinterpret_cast<Apply>(base + 0x101198);
	InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slotAddress), reinterpret_cast<void*>(&ApplyPass));
	DWORD ignored = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(slotAddress), sizeof(void*), protection, &ignored)) {
		logger::error("[ENBEffect] Pass::Apply vtable protection restore failed error={}", GetLastError());
	}
	logger::info("[ENBEffect] Pass::Apply vtable hook installed before first sample (no inline trampoline)");
	originalTexture = reinterpret_cast<SetTexture>(Detours::X64::DetourFunction(base + 0x40250, reinterpret_cast<uintptr_t>(&Texture)));
	originalVector = reinterpret_cast<SetVector>(Detours::X64::DetourFunction(base + 0x40340, reinterpret_cast<uintptr_t>(&Vector)));
	originalArray = reinterpret_cast<SetArray>(Detours::X64::DetourFunction(base + 0x40390, reinterpret_cast<uintptr_t>(&Array)));
	if (!originalTexture || !originalVector || !originalArray) {
		logger::error("[ENBEffect] Setter hook installation incomplete; sampling disabled");
		return;
	}
	if (!InstallDrawCallSites()) { return; }
	originalRender = reinterpret_cast<Render>(Detours::X64::DetourFunction(base + 0x78AD0, reinterpret_cast<uintptr_t>(&Effect)));
	installed = originalRender != nullptr;
	logger::info("[ENBEffect] 0.501 effect-6 tracking installed={}; first/120-frame samples per quality transition; binary readbacks in TEMP/Upscaling-ENBEffect/PID", installed);
}
