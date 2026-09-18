#include "ReShadeDepth.h"
#include "SwapChainCreationObserver.h"
#include "ReShadeDepthFrameGraph.h"
#include "ReShadeDepthDeviceIdentity.h"
#include "DX12SwapChain.h"
#include "FrameCount.h"
#include "../extern/ReShade/include/reshade.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace
{
	namespace api = reshade::api;
	using Runtime = api::effect_runtime;
	using Stamp = ReShadeDepth::FrameGraph::Stamp;
	using winrt::com_ptr;
	std::recursive_mutex mutex;
	std::atomic_bool initialized{ false }, active{ false };
	bool callbacksRegistered = false;
	HWND outputWindow = nullptr;
	uint64_t nextSerial = 0;
	UINT displayWidth = 0, displayHeight = 0;

	struct Timeline
	{
		com_ptr<ID3D12CommandQueue> queue;
		com_ptr<ID3D12Fence> fence;
		uint64_t next = 0;
	};
	struct Slot
	{
		std::unique_ptr<D3D11D3D12SharedTexture> texture;
		com_ptr<ID3D11UnorderedAccessView> uav;
		com_ptr<ID3D12Fence> readyFence, doneFence;
		uint64_t ready = 0, done = 0, serial = 0, frame = 0, deviceIdentity = 0;
		UINT width = 0, height = 0;
		bool published = false, consumed = false, poisoned = false, retired = false;
		bool Complete() const
		{
			if (poisoned) return false;
			const auto complete = [](ID3D12Fence* f, uint64_t v) {
				if (!v) return true;
				const auto value = f ? f->GetCompletedValue() : UINT64_MAX;
				return value != UINT64_MAX && value >= v;
			};
			return complete(readyFence.get(), ready) && complete(doneFence.get(), done);
		}
	};
	std::vector<std::shared_ptr<Slot>> pool;
	std::array<std::shared_ptr<Slot>, kDX12FrameCount> pending;
	struct Surface
	{
		api::device* device = nullptr;
		api::resource resource{};
		api::resource_view srv{}, uav{};
		UINT width = 0, height = 0;
		com_ptr<ID3D12Fence> doneFence;
		uint64_t done = 0;
		bool poisoned = false;
		void Destroy()
		{
			if (!device) return;
			if (srv.handle) device->destroy_resource_view(srv);
			if (uav.handle) device->destroy_resource_view(uav);
			if (resource.handle) device->destroy_resource(resource);
			device = nullptr; srv = {}; uav = {}; resource = {};
		}
		~Surface() { Destroy(); }
		bool Complete() const
		{
			if (poisoned) return false;
			if (!done) return true;
			const auto value = doneFence->GetCompletedValue();
			return value != UINT64_MAX && value >= done;
		}
	};
	std::vector<std::shared_ptr<Surface>> surfaces;
	struct RuntimeState
	{
		bool enabled = false;
		std::shared_ptr<Surface> surface;
		// Retain the current parent until this runtime advances to a different
		// frame: a paced generated/real pair may have an idle gap between callbacks.
		std::shared_ptr<Slot> lastSource;
		bool bindingDirty = true;
		std::vector<api::effect_texture_variable> depthVariables;
	};
	std::map<Runtime*, RuntimeState> runtimes;
	struct Scope
	{
		uint64_t color = 0;
		Stamp stamp;
		std::shared_ptr<Slot> slot;
		std::shared_ptr<Surface> surface;
		std::shared_ptr<Timeline> timeline;
		Runtime* runtime = nullptr;
		bool started = false, finished = false;
	};
	// Only the consumer's callbacks are paired by chain + thread. Publication
	// never depends on the application's Present thread or its call lifetime.
	using ScopeKey = std::pair<uint64_t, DWORD>;
	std::map<ScopeKey, Scope> scopes;
	std::map<uint64_t, std::shared_ptr<Timeline>> timelines;
	ReShadeDepth::FrameGraph graph;
	std::unordered_map<api::command_list*, std::vector<ReShadeDepth::FrameGraph::Copy>> recorded;
	std::map<uint64_t, Stamp> fsrFrames;
	std::map<uint64_t, Stamp> fsrOutputs;

	struct Producer
	{
		com_ptr<ID3D11Device> device;
		com_ptr<ID3D11DeviceContext1> context;
		com_ptr<ID3DDeviceContextState> isolated;
		com_ptr<ID3D11ComputeShader> shader;
		com_ptr<ID3D11Buffer> constants;
	};
	Producer producer;

	void Check(HRESULT hr)
	{
		if (FAILED(hr)) throw std::runtime_error(std::format("D3D failure 0x{:08X}", static_cast<uint32_t>(hr)));
	}

	struct DepthConstants
	{
		UINT sourceWidth, sourceHeight, outputWidth, outputHeight;
		float jitterU, jitterV;
		float padding[2]{};
	};
	static_assert(sizeof(DepthConstants) == 32);
	// Zero offset and equal extents preserve exact texels.
	constexpr char depthShader[] = R"hlsl(
cbuffer Constants : register(b0) { uint2 SourceExtent; uint2 OutputExtent; float2 JitterUV; float2 Padding; };
Texture2D<float> Input : register(t0);
RWTexture2D<float> Output : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputExtent)) return;
    if (all(SourceExtent == OutputExtent) && all(JitterUV == 0.0)) {
        Output[id.xy] = Input.Load(int3(id.xy, 0));
        return;
    }
    float2 position = (float2(id.xy) + 0.5) * float2(SourceExtent) / float2(OutputExtent)
                    - JitterUV * float2(SourceExtent) - 0.5;
    position = clamp(position, 0.0, float2(SourceExtent) - 1.0);
    int2 p0 = int2(floor(position));
    int2 p1 = min(p0 + 1, int2(SourceExtent) - 1);
    float2 f = position - float2(p0);
    float d00 = Input.Load(int3(p0, 0));
    float d10 = Input.Load(int3(p1.x, p0.y, 0));
    float d01 = Input.Load(int3(p0.x, p1.y, 0));
    float d11 = Input.Load(int3(p1, 0));
    Output[id.xy] = lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y);
}
)hlsl";

	void CreateProducer(ID3D11Device* device)
	{
		Producer next;
		next.device.copy_from(device);
		com_ptr<ID3D11DeviceContext> context;
		device->GetImmediateContext(context.put());
		next.context = context.as<ID3D11DeviceContext1>();
		const auto device1 = next.device.as<ID3D11Device1>();
		const auto level = device->GetFeatureLevel();
		D3D_FEATURE_LEVEL chosen{};
		Check(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, next.isolated.put()));
		com_ptr<ID3DBlob> code, errors;
		Check(D3DCompile(depthShader, sizeof(depthShader) - 1, "ReShadeDepth", nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, 0, code.put(), errors.put()));
		Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, next.shader.put()));
		D3D11_BUFFER_DESC desc{}; desc.ByteWidth = sizeof(DepthConstants); desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&desc, nullptr, next.constants.put()));
		producer = std::move(next);
	}

	void SetReady(Runtime* runtime, bool ready)
	{
		runtime->enumerate_uniform_variables(nullptr, [ready](Runtime* r, api::effect_uniform_variable variable) {
			char source[32]{};
			if (r->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "bufready_depth") == 0)
				r->set_uniform_value_bool(variable, ready);
		});
	}

	void BindSurface(Runtime* runtime, RuntimeState& state)
	{
		if (!state.surface) return;
		const auto view = state.surface->srv;
		for (const auto variable : state.depthVariables) {
			api::resource_view bound{}, srgb{};
			runtime->get_texture_binding(variable, &bound, &srgb);
			if (bound != view || srgb != view) { state.bindingDirty = true; break; }
		}
		if (!state.bindingDirty) return;
		// ReShade 6.8 waits idle here even if the view has not changed. NEVER call
		// this per frame. Only creation/resize/reload/another add-on rebinding DEPTH.
		runtime->update_texture_bindings("DEPTH", view, view);
		state.depthVariables.clear();
		runtime->enumerate_texture_variables(nullptr, [&](Runtime* r, api::effect_texture_variable variable) {
			api::resource_view bound{};
			r->get_texture_binding(variable, &bound, nullptr);
			if (bound == view) state.depthVariables.push_back(variable);
		});
		state.bindingDirty = false;
	}
	void EnsureSurface(Runtime* runtime, RuntimeState& state, UINT width, UINT height)
	{
		if (state.surface && state.surface->width == width && state.surface->height == height && !state.surface->poisoned) {
			BindSurface(runtime, state); return;
		}
		std::erase_if(surfaces, [](const auto& surface) { return surface.use_count() == 1 && surface->Complete(); });
		// Resizes cannot accumulate unlimited in-flight consumer allocations.
		uint64_t bytes = uint64_t(width) * height * sizeof(float);
		for (const auto& surface : surfaces) bytes += uint64_t(surface->width) * surface->height * sizeof(float);
		if (surfaces.size() >= 12 || bytes > 96ull * 1024 * 1024) throw std::runtime_error("consumer-surface-pool-busy");
		auto next = std::make_shared<Surface>();
		next->device = runtime->get_device(); next->width = width; next->height = height;
		const api::resource_desc desc(width, height, 1, 1, api::format::r32_float, 1, api::memory_heap::default_,
			api::resource_usage::shader_resource | api::resource_usage::copy_dest | api::resource_usage::unordered_access);
		if (!next->device->create_resource(desc, nullptr, api::resource_usage::shader_resource, &next->resource) ||
			!next->device->create_resource_view(next->resource, api::resource_usage::shader_resource, api::resource_view_desc(api::format::r32_float), &next->srv) ||
			!next->device->create_resource_view(next->resource, api::resource_usage::unordered_access, api::resource_view_desc(api::format::r32_float), &next->uav))
			throw std::runtime_error("consumer-surface-creation-failed");
		surfaces.push_back(next); state.surface = std::move(next);
		state.depthVariables.clear(); state.bindingDirty = true;
		BindSurface(runtime, state);
	}
	void ClearSurface(api::command_list* commands, const std::shared_ptr<Surface>& surface)
	{
		if (!surface || surface->poisoned) return;
		commands->barrier(surface->resource, api::resource_usage::shader_resource, api::resource_usage::unordered_access);
		const float farDepth[4]{ 1, 1, 1, 1 }; // Fallout 4's non-reversed raw world depth.
		commands->clear_unordered_access_view_float(surface->uav, farDepth);
		commands->barrier(surface->resource, api::resource_usage::unordered_access, api::resource_usage::shader_resource);
	}

	bool IsOutput(Runtime* runtime)
	{
		return outputWindow && runtime->get_hwnd() == outputWindow && runtime->get_device()->get_api() == api::device_api::d3d12;
	}
	void UpdateActive()
	{
		active.store(std::any_of(runtimes.begin(), runtimes.end(), [](const auto& pair) {
			return IsOutput(pair.first) && pair.second.enabled;
		}), std::memory_order_release);
	}
	std::shared_ptr<Slot> FindSlot(Stamp stamp)
	{
		if (stamp) for (const auto& slot : pool)
			if (slot->serial == stamp->serial && !slot->retired && !slot->poisoned) return slot;
		return {};
	}
	std::shared_ptr<Timeline> GetTimeline(api::command_queue* queue)
	{
		const auto native = queue->get_native();
		if (const auto it = timelines.find(native); it != timelines.end()) return it->second;
		auto next = std::make_shared<Timeline>();
		next->queue.copy_from(reinterpret_cast<ID3D12CommandQueue*>(native));
		com_ptr<ID3D12Device> device;
		Check(next->queue->GetDevice(IID_PPV_ARGS(device.put())));
		Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(next->fence.put())));
		timelines.emplace(native, next);
		return next;
	}

	void BeginEffects(Runtime* runtime, api::command_list* commands, api::resource_view, api::resource_view)
	{
		std::lock_guard lock(mutex);
		if (!IsOutput(runtime)) return;
		auto& state = runtimes[runtime];
		if (state.lastSource && state.lastSource->retired) state.lastSource.reset();
		SetReady(runtime, false);
		const auto it = scopes.find({ runtime->get_native(), GetCurrentThreadId() });
		const auto skip = [&] {
			// Keep the descriptor stable even on a skipped frame. Withdraw readiness
			// and clear its pixels so shaders ignoring bufready_depth see no old scene.
			if (it != scopes.end() && state.surface) {
				BindSurface(runtime, state);
				it->second.surface = state.surface;
				ClearSurface(commands, state.surface);
			}
		};
		if (it == scopes.end()) { skip(); return; }
		auto& scope = it->second;
		if (scope.started) { skip(); return; }
		if (!scope.stamp || !scope.slot) { skip(); return; }
		auto& slot = *scope.slot;
		if (slot.retired || !slot.published || slot.poisoned) { skip(); return; }
		if (scope.color != runtime->get_current_back_buffer().handle) { skip(); return; }
		if (scope.timeline->queue.get() != reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native())) {
			skip(); return;
		}
		for (const auto& [key, other] : scopes) {
			if (&other != &scope && other.slot == scope.slot && other.started) {
				skip(); return;
			}
		}
		try {
			uint64_t consumerIdentity = 0;
			Check(ReShadeDepth::DeviceIdentity(reinterpret_cast<ID3D12Device*>(runtime->get_device()->get_native()), consumerIdentity));
			if (slot.deviceIdentity != consumerIdentity) {
				skip(); return;
			}
			const api::resource resource{ reinterpret_cast<uint64_t>(slot.texture->resource12.get()) };
			EnsureSurface(runtime, state, slot.width, slot.height);
			scope.surface = state.surface;
			// These values were already signalled by the producer/previous consumer.
			// No CPU wait, no signal promised by a later application frame.
			Check(scope.timeline->queue->Wait(slot.readyFence.get(), slot.ready));
			if (slot.done) Check(scope.timeline->queue->Wait(slot.doneFence.get(), slot.done));
			commands->barrier(resource, api::resource_usage::general, api::resource_usage::copy_source);
			commands->barrier(scope.surface->resource, api::resource_usage::shader_resource, api::resource_usage::copy_dest);
			commands->copy_resource(resource, scope.surface->resource);
			commands->barrier(scope.surface->resource, api::resource_usage::copy_dest, api::resource_usage::shader_resource);
			commands->barrier(resource, api::resource_usage::copy_source, api::resource_usage::general);
			scope.runtime = runtime; scope.started = true;
			state.lastSource = scope.slot;
			SetReady(runtime, true);
			slot.consumed = true;
		} catch (const std::exception&) {
			if (state.surface) {
				scope.surface = state.surface;
				ClearSurface(commands, state.surface);
			}
		}
	}

	void FinishEffects(Runtime* runtime, api::command_list* commands, api::resource_view, api::resource_view)
	{
		std::lock_guard lock(mutex);
		const auto it = scopes.find({ runtime->get_native(), GetCurrentThreadId() });
		if (it == scopes.end()) return;
		auto& scope = it->second;
		if (!scope.started || scope.finished || scope.runtime != runtime) return;
		(void)commands;
		scope.finished = true;
	}

	void OnPresent(api::command_queue* queue, api::swapchain* chain, const api::rect*, const api::rect*, uint32_t, const api::rect*)
	{
		std::lock_guard lock(mutex);
		if (!outputWindow || chain->get_hwnd() != outputWindow || queue->get_device()->get_api() != api::device_api::d3d12) return;
		std::shared_ptr<Surface> presentedSurface;
		for (auto& [runtime, state] : runtimes) if (runtime->get_native() == chain->get_native()) {
			state.enabled = runtime->get_effects_state();
			presentedSurface = state.surface; // The GUI can preview DEPTH with effects disabled.
		}
		UpdateActive();
		const ScopeKey key{ chain->get_native(), GetCurrentThreadId() };
		if (auto old = scopes.find(key); old != scopes.end()) {
			if (old->second.started && old->second.slot) old->second.slot->poisoned = true;
			if (old->second.surface) old->second.surface->poisoned = true;
			scopes.erase(old);
		}
		const auto color = chain->get_current_back_buffer().handle;
		Stamp stamp;
		if (auto explicitFrame = fsrOutputs.find(color); explicitFrame != fsrOutputs.end()) {
			stamp = explicitFrame->second; fsrOutputs.erase(explicitFrame);
		} else {
			stamp = graph.Find(color);
		}
		// Consuming an output label prevents repeated Presents from silently using
		// an old frame when a vendor does not issue observable copies.
		graph.Forget(color);
		if (!active.load(std::memory_order_acquire) && !presentedSurface) return;
		try {
			Scope scope;
			scope.color = color; scope.stamp = stamp;
			scope.slot = FindSlot(stamp); scope.timeline = GetTimeline(queue);
			scope.surface = std::move(presentedSurface);
			scopes.emplace(key, std::move(scope));
		} catch (const std::exception&) {
			// Leave this Present without a consumer scope if queue setup fails.
		}
	}

	void FinishPresent(api::command_queue*, api::swapchain* chain)
	{
		std::lock_guard lock(mutex);
		const auto it = scopes.find({ chain->get_native(), GetCurrentThreadId() });
		if (it == scopes.end()) return;
		auto& scope = it->second;
		if (scope.started && !scope.finished) {
			scope.slot->poisoned = true;
			if (scope.surface) scope.surface->poisoned = true;
		} else if (scope.surface) {
			// ReShade has flushed its immediate list before finish_present. Also
			// cover far-depth clears on skipped frames and the stable surface's GUI use.
			const auto value = ++scope.timeline->next;
			const auto hr = scope.timeline->queue->Signal(scope.timeline->fence.get(), value);
			if (SUCCEEDED(hr)) {
				scope.surface->doneFence = scope.timeline->fence; scope.surface->done = value;
				if (scope.started) { scope.slot->doneFence = scope.timeline->fence; scope.slot->done = value; }
			} else {
				if (scope.started) scope.slot->poisoned = true;
				scope.surface->poisoned = true;
			}
		}
		scopes.erase(it);
	}

	// Only full-size color resources are relevant. This excludes D3D11 world
	// rendering and the depth bridge itself from the command provenance recorder.
	bool IsColor(api::device* device, api::resource resource)
	{
		if (!resource.handle || !displayWidth || device->get_api() != api::device_api::d3d12) return false;
		const auto desc = device->get_resource_desc(resource);
		return desc.type == api::resource_type::texture_2d && desc.texture.width == displayWidth &&
			desc.texture.height == displayHeight && desc.texture.levels == 1 && desc.texture.depth_or_layers == 1 &&
			desc.texture.samples == 1 && desc.texture.format != api::format::r32_float &&
			desc.texture.format != api::format::r32_typeless;
	}
	void RecordCopy(api::command_list* commands, api::resource source, api::resource destination, bool full)
	{
		if (!IsColor(commands->get_device(), destination)) return;
		const bool proven = full && IsColor(commands->get_device(), source);
		auto copy = graph.Record(source.handle, destination.handle, proven);
		recorded[commands].push_back(copy);
	}
	void RecordWriteHint(api::command_list* commands, api::resource destination)
	{
		if (!IsColor(commands->get_device(), destination)) return;
		auto& ops = recorded[commands];
		// Repeated barriers/RT bindings for the same destination are one hint.
		if (!ops.empty() && ops.back().writeHint && ops.back().destination == destination.handle) return;
		ops.push_back(graph.WriteHint(destination.handle));
	}
	bool CopyResource(api::command_list* commands, api::resource source, api::resource destination)
	{
		if (!active.load(std::memory_order_acquire) || commands->get_device()->get_api() != api::device_api::d3d12) return false;
		std::lock_guard lock(mutex);
		RecordCopy(commands, source, destination, true);
		return false;
	}
	bool CopyRegion(api::command_list* commands, api::resource source, uint32_t srcSub, const api::subresource_box* src,
		api::resource destination, uint32_t dstSub, const api::subresource_box* dst, api::filter_mode)
	{
		if (!active.load(std::memory_order_acquire) || commands->get_device()->get_api() != api::device_api::d3d12) return false;
		std::lock_guard lock(mutex);
		const auto whole = [](const api::subresource_box* box) {
			return !box || (box->left == 0 && box->top == 0 && box->front == 0 &&
				box->right == displayWidth && box->bottom == displayHeight && box->back == 1);
		};
		RecordCopy(commands, source, destination, srcSub == 0 && dstSub == 0 && whole(src) && whole(dst));
		return false;
	}
	void Barriers(api::command_list* commands, uint32_t count, const api::resource* resources, const api::resource_usage*, const api::resource_usage* states)
	{
		if (!active.load(std::memory_order_acquire) || commands->get_device()->get_api() != api::device_api::d3d12) return;
		std::lock_guard lock(mutex);
		for (uint32_t i = 0; i < count; ++i) {
			// 'present' contains render_target bits in the abstract API; it is NOT a write.
			if (states[i] == api::resource_usage::present || states[i] == api::resource_usage::general) continue;
			if ((states[i] & (api::resource_usage::copy_dest | api::resource_usage::render_target |
				api::resource_usage::unordered_access | api::resource_usage::resolve_dest)) != api::resource_usage::undefined)
				RecordWriteHint(commands, resources[i]);
		}
	}
	void BindTargets(api::command_list* commands, uint32_t count, const api::resource_view* targets, api::resource_view)
	{
		if (!active.load(std::memory_order_acquire) || commands->get_device()->get_api() != api::device_api::d3d12) return;
		std::lock_guard lock(mutex);
		for (uint32_t i = 0; i < count; ++i)
			RecordWriteHint(commands, commands->get_device()->get_resource_from_view(targets[i]));
	}
	bool BeginRenderPass(api::command_list* commands, uint32_t count, const api::render_pass_render_target_desc* targets,
		const api::render_pass_depth_stencil_desc*, api::render_pass_flags)
	{
		for (uint32_t i = 0; i < count; ++i) BindTargets(commands, 1, &targets[i].view, {});
		return false;
	}
	bool ClearTarget(api::command_list* commands, api::resource_view target, const float[4], uint32_t, const api::rect*)
	{
		if (!active.load(std::memory_order_acquire) || commands->get_device()->get_api() != api::device_api::d3d12) return false;
		std::lock_guard lock(mutex);
		RecordCopy(commands, {}, commands->get_device()->get_resource_from_view(target), false);
		return false;
	}
	void ResetCommands(api::command_list* commands)
	{
		if (commands->get_device()->get_api() != api::device_api::d3d12) return;
		std::lock_guard lock(mutex); recorded.erase(commands);
	}
	void ExecuteCommands(api::command_queue* queue, api::command_list* commands)
	{
		if (queue->get_device()->get_api() != api::device_api::d3d12) return;
		std::lock_guard lock(mutex);
		const auto it = recorded.find(commands);
		if (it == recorded.end()) return;
		graph.Execute(it->second);
	}
	void ExecuteSecondary(api::command_list* commands, api::command_list* secondary)
	{
		if (commands->get_device()->get_api() != api::device_api::d3d12) return;
		std::lock_guard lock(mutex);
		if (const auto it = recorded.find(secondary); it != recorded.end()) {
			auto copies = it->second; // Also safe if keys happen to be identical.
			auto& destination = recorded[commands];
			destination.insert(destination.end(), copies.begin(), copies.end());
		}
	}
	void DestroyResource(api::device* device, api::resource resource)
	{
		if (device->get_api() != api::device_api::d3d12) return;
		std::lock_guard lock(mutex);
		graph.Forget(resource.handle); fsrOutputs.erase(resource.handle);
		for (auto& [commands, copies] : recorded) for (auto& copy : copies)
			if (copy.source == resource.handle || copy.destination == resource.handle) {
				copy.full = false; copy.recordedSource.reset();
				copy.writeHint = copy.applicationSource = false;
				if (copy.destination == resource.handle) copy.destination = 0;
			}
	}

	void InitRuntime(Runtime* runtime)
	{
		std::lock_guard lock(mutex);
		if (!callbacksRegistered) {
			// After generic depth's handlers, so DEPTH/bufready_depth win for this frame.
			reshade::register_event<reshade::addon_event::reshade_begin_effects>(BeginEffects);
			reshade::register_event<reshade::addon_event::reshade_finish_effects>(FinishEffects);
			callbacksRegistered = true;
		}
		if (runtime->get_device()->get_api() == api::device_api::d3d12) {
			runtimes[runtime].enabled = runtime->get_effects_state();
			UpdateActive();
		}
	}
	void DestroyRuntime(Runtime* runtime)
	{
		std::lock_guard lock(mutex);
		runtimes.erase(runtime); UpdateActive();
	}
	void RuntimePresent(Runtime* runtime)
	{
		std::lock_guard lock(mutex);
		if (auto it = runtimes.find(runtime); it != runtimes.end()) {
			const bool enabled = runtime->get_effects_state();
			if (it->second.enabled != enabled) {
				it->second.enabled = enabled; UpdateActive();
			}
		}
	}
	void ReloadedEffects(Runtime* runtime)
	{
		std::lock_guard lock(mutex);
		if (auto it = runtimes.find(runtime); it != runtimes.end()) {
			it->second.bindingDirty = true;
			it->second.depthVariables.clear(); // Handles from the previous compilation are dead.
		}
	}
	void DestroyQueue(api::command_queue* queue)
	{
		if (queue->get_device()->get_api() != api::device_api::d3d12) return;
		std::lock_guard lock(mutex);
		// Snapshot completion fences and outstanding scopes keep their own refs.
		timelines.erase(queue->get_native());
	}
	void DestroyDevice(api::device* device)
	{
		std::lock_guard lock(mutex);
		for (auto& surface : surfaces) if (surface->device == device) surface->Destroy();
	}
}

void ReShadeDepth::Initialize()
{
	if (initialized.load(std::memory_order_acquire) || !reshade::internal::get_reshade_module_handle()) return;
	HMODULE module{};
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&Initialize), &module);
	if (!reshade::register_addon(module)) return;
	initialized.store(true, std::memory_order_release);
	SwapChainCreationObserver::Initialize();
	reshade::register_event<reshade::addon_event::init_effect_runtime>(InitRuntime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(DestroyRuntime);
	reshade::register_event<reshade::addon_event::reshade_present>(RuntimePresent);
	reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(ReloadedEffects);
	reshade::register_event<reshade::addon_event::destroy_device>(DestroyDevice);
	reshade::register_event<reshade::addon_event::destroy_command_queue>(DestroyQueue);
	reshade::register_event<reshade::addon_event::present>(OnPresent);
	reshade::register_event<reshade::addon_event::finish_present>(FinishPresent);
	reshade::register_event<reshade::addon_event::copy_resource>(CopyResource);
	reshade::register_event<reshade::addon_event::copy_texture_region>(CopyRegion);
	reshade::register_event<reshade::addon_event::barrier>(Barriers);
	reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(BindTargets);
	reshade::register_event<reshade::addon_event::begin_render_pass>(BeginRenderPass);
	reshade::register_event<reshade::addon_event::clear_render_target_view>(ClearTarget);
	reshade::register_event<reshade::addon_event::reset_command_list>(ResetCommands);
	reshade::register_event<reshade::addon_event::destroy_command_list>(ResetCommands);
	reshade::register_event<reshade::addon_event::execute_command_list>(ExecuteCommands);
	reshade::register_event<reshade::addon_event::execute_secondary_command_list>(ExecuteSecondary);
	reshade::register_event<reshade::addon_event::destroy_resource>(DestroyResource);
}

void ReShadeDepth::SetOutputWindow(HWND window)
{
	std::lock_guard lock(mutex);
	outputWindow = window; UpdateActive();
}
bool ReShadeDepth::IsActive() { return active.load(std::memory_order_acquire); }

void ReShadeDepth::DiscardCapture(uint64_t frame)
{
	if (!initialized.load(std::memory_order_acquire)) return;
	std::lock_guard lock(mutex);
	for (auto& slot : pending) if (slot && slot->frame == frame) slot.reset();
}

void ReShadeDepth::Capture(ID3D11ShaderResourceView* depth, UINT sourceWidth, UINT sourceHeight, float jitterU, float jitterV, uint64_t frame)
{
	if (!IsActive()) return;
	std::lock_guard lock(mutex);
	auto* swap = DX12SwapChain::GetSingleton();
	const auto index = swap->GetFrameIndex();
	if (!swap->IsReady() || index >= pending.size()) { DiscardCapture(frame); return; }
	if (pending[index] && pending[index]->frame == frame) return;
	pending[index].reset();
	try {
		const UINT width = swap->swapChainDesc.Width, height = swap->swapChainDesc.Height;
		if (!depth || !sourceWidth || !sourceHeight || !width || !height || !std::isfinite(jitterU) || !std::isfinite(jitterV)) { DiscardCapture(frame); return; }
		D3D11_SHADER_RESOURCE_VIEW_DESC view{}; depth->GetDesc(&view);
		if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || view.Texture2D.MostDetailedMip != 0) { DiscardCapture(frame); return; }
		switch (view.Format) {
		case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: break;
		default: DiscardCapture(frame); return;
		}
		com_ptr<ID3D11Resource> source; depth->GetResource(source.put());
		const auto texture = source.as<ID3D11Texture2D>();
		D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1 || sourceWidth > desc.Width || sourceHeight > desc.Height) { DiscardCapture(frame); return; }
		com_ptr<ID3D11Device> device; texture->GetDevice(device.put());
		if (!producer.device) CreateProducer(device.get());
		if (producer.device.get() != device.get()) { DiscardCapture(frame); return; }

		// All references other than the pool itself protect CPU descriptor lifetime.
		// GPU completion, NOT host Present return or backbuffer index, protects texels.
		std::erase_if(pool, [width, height](const auto& slot) {
			return slot.use_count() == 1 && slot->Complete() && (slot->retired || slot->width != width || slot->height != height);
		});
		std::shared_ptr<Slot> slot;
		for (const auto& candidate : pool) if (candidate.use_count() == 1 && candidate->Complete() &&
			candidate->consumed && (!slot || candidate->serial < slot->serial)) slot = candidate;
		const uint64_t bytes = uint64_t(width) * height * sizeof(float);
		uint64_t allocated = 0;
		for (const auto& item : pool) allocated += uint64_t(item->width) * item->height * sizeof(float);
		if (!slot && pool.size() < 12 && allocated + bytes <= 96ull * 1024 * 1024) {
			slot = std::make_shared<Slot>();
			D3D11_TEXTURE2D_DESC shared{};
			shared.Width = width; shared.Height = height; shared.MipLevels = 1; shared.ArraySize = 1;
			shared.Format = DXGI_FORMAT_R32_FLOAT; shared.SampleDesc.Count = 1;
			shared.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			slot->texture = std::make_unique<D3D11D3D12SharedTexture>(shared, device.get(), swap->GetD3D12Device());
			Check(DeviceIdentity(swap->GetD3D12Device(), slot->deviceIdentity));
			Check(device->CreateUnorderedAccessView(slot->texture->resource11.get(), nullptr, slot->uav.put()));
			slot->width = width; slot->height = height;
			pool.push_back(slot);
		}
		if (!slot) {
			for (const auto& candidate : pool) if (candidate.use_count() == 1 && candidate->Complete() &&
				(!slot || candidate->serial < slot->serial)) slot = candidate;
		}
		if (!slot) { DiscardCapture(frame); return; }
		slot->serial = ++nextSerial; slot->frame = frame; slot->published = slot->consumed = false;
		slot->done = 0; slot->doneFence = nullptr;

		com_ptr<ID3DDeviceContextState> previous;
		producer.context->SwapDeviceContextState(producer.isolated.get(), previous.put());
		struct Restore {
			ID3D11DeviceContext1* context;
			com_ptr<ID3DDeviceContextState>& previous;
			~Restore() { context->SwapDeviceContextState(previous.get(), nullptr); }
		} restore{ producer.context.get(), previous };
		producer.context->ClearState();
		const DepthConstants parameters{ sourceWidth, sourceHeight, width, height, jitterU, jitterV };
		producer.context->UpdateSubresource(producer.constants.get(), 0, nullptr, &parameters, 0, 0);
		auto* constants = producer.constants.get(); auto* uav = slot->uav.get();
		producer.context->CSSetShader(producer.shader.get(), nullptr, 0);
		producer.context->CSSetConstantBuffers(0, 1, &constants);
		producer.context->CSSetShaderResources(0, 1, &depth);
		producer.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		producer.context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		producer.context->ClearState();
		// Existing shared producer timeline; normal application Present will flush.
		uint64_t unused = 0;
		if (!swap->GetRetirementFences(slot->ready, unused)) {
			slot->poisoned = true; DiscardCapture(frame); return;
		}
		slot->readyFence.copy_from(swap->GetInteropReadyFence());
		pending[index] = slot;
	} catch (const std::exception&) {
		DiscardCapture(frame);
	}
}

void ReShadeDepth::PublishPresent(UINT index, uint64_t frame, ID3D12Resource* backbuffer)
{
	if (!initialized.load(std::memory_order_acquire)) return;
	std::lock_guard lock(mutex);
	Stamp stamp;
	if (backbuffer) {
		const auto desc = backbuffer->GetDesc();
		displayWidth = static_cast<UINT>(desc.Width); displayHeight = desc.Height;
	}
	if (index < pending.size()) {
		if (auto& slot = pending[index]; slot && slot->frame == frame && IsActive()) {
			slot->published = true; stamp = FrameStamp{ slot->serial, frame };
		}
		pending[index].reset();
	}
	graph.SetApplication(reinterpret_cast<uint64_t>(backbuffer), stamp);
}

void ReShadeDepth::TagFSRFrame(uint64_t backendFrame, uint64_t engineFrame)
{
	if (!IsActive()) return;
	std::lock_guard lock(mutex);
	Stamp stamp;
	for (const auto& slot : pending) if (slot && slot->frame == engineFrame) stamp = FrameStamp{ slot->serial, engineFrame, backendFrame };
	fsrFrames[backendFrame] = stamp;
	while (fsrFrames.size() > 128) fsrFrames.erase(fsrFrames.begin());
}
void ReShadeDepth::TagFSRPresent(uint64_t backendFrame, ID3D12Resource* output, bool generated)
{
	if (!IsActive()) return; // An already queued SDK callback still performs its color copy.
	std::lock_guard lock(mutex);
	Stamp stamp;
	if (const auto it = fsrFrames.find(backendFrame); it != fsrFrames.end()) stamp = it->second;
	if (stamp) stamp->generated = generated;
	fsrOutputs[reinterpret_cast<uint64_t>(output)] = stamp;
}
void ReShadeDepth::ResetAfterIdle()
{
	if (!initialized.load(std::memory_order_acquire)) return;
	std::lock_guard lock(mutex);
	graph.Clear(); recorded.clear(); fsrFrames.clear(); fsrOutputs.clear(); pending = {};
	displayWidth = displayHeight = 0;
	for (auto& slot : pool) slot->retired = true;
	// The host drain alone is not a pacer drain. Keep pending consumers/descriptors
	// alive; their own callbacks detach bindings and signal completion.
	std::erase_if(pool, [](const auto& slot) { return slot.use_count() == 1 && slot->Complete(); });
	producer = {};
}
