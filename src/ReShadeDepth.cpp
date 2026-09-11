#include "ReShadeDepth.h"
#include "DX12SwapChain.h"
#include "Upscaling.h"
#include "../extern/ReShade/include/reshade.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
	struct DepthSlot
	{
		winrt::com_ptr<ID3D12Resource> texture;
		reshade::api::device* device = nullptr;
		reshade::api::resource_view view{};
		reshade::api::resource_view clearView{};
		uint64_t generation = 0;
		uint64_t copyValue = 0;
		uint64_t readValue = 0;
		uint64_t sourcePresent = 0;
		~DepthSlot()
		{
			if (device && view.handle) device->destroy_resource_view(view);
			if (device && clearView.handle) device->destroy_resource_view(clearView);
		}
	};
	struct PendingRead
	{
		DWORD thread;
		uint64_t backBuffer;
		std::shared_ptr<DepthSlot> slot;
	};
	std::recursive_mutex g_mutex;
	std::mutex g_completionMutex;
	std::vector<reshade::api::effect_runtime*> g_runtimes;
	std::unordered_map<reshade::api::effect_runtime*, bool> g_effectDemand;
	reshade::api::effect_runtime* g_runtime = nullptr;
	// Independent of the proxy/output backbuffer indices: one snapshot can be
	// read by several asynchronous FG outputs after the proxy advances its slot.
	std::array<std::shared_ptr<DepthSlot>, kDX12FrameCount * 2> g_slots;
	std::shared_ptr<DepthSlot> g_published;
	std::shared_ptr<DepthSlot> g_bound;
	std::vector<std::shared_ptr<DepthSlot>> g_retiredOutputs;
	uint64_t g_generation = 0;
	uint64_t g_sourceWidth = 0;
	uint32_t g_sourceHeight = 0;
	reshade::api::effect_texture_variable g_depthBinding{};
	uint64_t g_minSourcePresent = 0;
	std::vector<PendingRead> g_pendingReads;
	winrt::com_ptr<ID3D12Fence> g_copyReady;
	winrt::com_ptr<ID3D12Fence> g_readDone;
	uint64_t g_copyValue = 0;
	uint64_t g_readValue = 0;
	bool g_prepared = false;
	bool g_presenting = false;
	bool g_failed = false;
	bool g_stopping = false;
	uint32_t g_slot = 0;
	bool g_loggedBinding = false;
	uint64_t g_presentSerial = 0;




	bool IsComplete(ID3D12Fence* a_fence, uint64_t a_value)
	{
		if (!a_value) return true;
		const auto completed = a_fence->GetCompletedValue();
		return completed != UINT64_MAX && completed >= a_value;
	}

	void BindDepth(reshade::api::effect_runtime* a_runtime, reshade::api::resource_view a_view, bool a_updateBinding = true, bool a_ready = true)
	{
		if (a_updateBinding) a_runtime->update_texture_bindings("DEPTH", a_view, a_view);
		a_runtime->enumerate_uniform_variables(nullptr, [a_view, a_ready](reshade::api::effect_runtime* runtime, reshade::api::effect_uniform_variable variable) {
			char source[32]{};
			if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) &&
				std::strcmp(source, "bufready_depth") == 0) {
				runtime->set_uniform_value_bool(variable, a_ready && a_view.handle != 0);
			}
		});
	}

	ID3D12CommandQueue* EffectQueue()
	{
		return reinterpret_cast<ID3D12CommandQueue*>(g_runtime->get_command_queue()->get_native());
	}

	void OnInitRuntime(reshade::api::effect_runtime* a_runtime)
	{
		if (a_runtime->get_device()->get_api() != reshade::api::device_api::d3d12) return;
		const std::lock_guard lock(g_mutex);
		g_runtimes.push_back(a_runtime);
		g_effectDemand[a_runtime] = false;
		logger::info("[ReShade depth] D3D12 runtime detected hwnd={}", a_runtime->get_hwnd());
	}

	void OnReloadedEffects(reshade::api::effect_runtime* a_runtime)
	{
		const std::lock_guard lock(g_mutex);
		if (a_runtime == g_runtime) g_depthBinding = {};
	}

	void OnDestroyRuntime(reshade::api::effect_runtime* a_runtime)
	{
		std::unique_lock lock(g_mutex);
		std::erase(g_runtimes, a_runtime);
		g_effectDemand.erase(a_runtime);
		if (a_runtime == g_runtime) {
			g_stopping = true;
			g_published.reset();
			g_prepared = false;
			// Do not hold the publication lock across ReShade/queue idle calls:
			// an output callback can own the queue lock while acquiring ours.
			lock.unlock();
			BindDepth(a_runtime, {});
			a_runtime->get_command_queue()->wait_idle();
			// Producer command contexts already retain both copy resources until
			// their submission completes (including copies not yet submitted).
			// Do not touch the render-thread fence counter, event or reuse arrays
			// here: runtime destruction can run on an asynchronous FG output thread.
			lock.lock();
			// Destroy API views before the owning ReShade device can disappear,
			// including slots temporarily retained by a completion callback.
			for (auto& slot : g_slots) {
				if (slot && slot->view.handle) {
					slot->device->destroy_resource_view(slot->view);
					slot->view = {};
					slot->device = nullptr;
				}
			}
			g_pendingReads.clear();
			g_bound.reset();
			g_retiredOutputs.clear();
			g_depthBinding = {};
			g_slots = {};
			g_copyReady = nullptr;
			g_readDone = nullptr;
			g_copyValue = g_readValue = 0;
			g_prepared = g_presenting = false;
			g_runtime = nullptr;
			g_loggedBinding = false;
			g_stopping = false;
		}
	}

	void OnBeginEffects(reshade::api::effect_runtime* a_runtime, reshade::api::command_list* a_list, reshade::api::resource_view, reshade::api::resource_view)
	{
		// Run effect enumeration on ReShade's own runtime thread, not the
		// producer thread. Global effects enabled does not imply any technique
		// is active. Empty presets must not cause copies/clears or queue flushes.
		bool enabled = false;
		if (a_runtime->get_effects_state()) {
			a_runtime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime* runtime, reshade::api::effect_technique technique) {
				enabled = enabled || runtime->get_technique_state(technique);
			});
		}
		std::unique_lock lock(g_mutex);
		if (g_effectDemand.contains(a_runtime)) g_effectDemand[a_runtime] = enabled;
		if (!enabled) return;
		if (a_runtime != g_runtime || g_stopping || !a_list) return;
		std::erase_if(g_retiredOutputs, [](const auto& output) {
			return output.use_count() == 1 && IsComplete(g_readDone.get(), output->readValue);
		});
		// FG output queues can already be dependencies of the producer through
		// Streamline's private queues. Even a submitted-but-incomplete copy signal
		// is unsafe to wait for here: that creates an indirect GPU wait cycle.
		// Search the pool, not just the latest publication (usually incomplete).
		std::shared_ptr<DepthSlot> snapshot;
		if (!g_failed && g_copyReady) {
			const auto completed = g_copyReady->GetCompletedValue();
			if (completed != UINT64_MAX) {
				for (const auto& candidate : g_slots) {
					if (candidate && candidate->generation == g_generation && candidate->sourcePresent >= g_minSourcePresent &&
						candidate->copyValue != 0 && candidate->copyValue <= completed &&
						(!snapshot || candidate->copyValue > snapshot->copyValue)) {
						snapshot = candidate;
					}
				}
			}
		}
		// Only the output queue writes this stable texture. All copies and effect
		// reads are ordered on that queue; no producer wait or per-frame rebinding.
		auto output = g_bound;
		try {
			if (snapshot && (!output || output->texture->GetDesc().Width != snapshot->texture->GetDesc().Width ||
				output->texture->GetDesc().Height != snapshot->texture->GetDesc().Height)) {
				auto next = std::make_shared<DepthSlot>();
				auto desc = snapshot->texture->GetDesc();
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
				winrt::com_ptr<ID3D12Device> device;
				DX::ThrowIfFailed(snapshot->texture->GetDevice(IID_PPV_ARGS(device.put())));
				const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(next->texture.put())));
				next->device = a_runtime->get_device();
				const reshade::api::resource resource{ reinterpret_cast<uint64_t>(next->texture.get()) };
				reshade::api::resource_view_desc srv(reshade::api::format::r32_float);
				srv.type = reshade::api::resource_view_type::texture_2d;
				srv.texture.levels = srv.texture.layers = 1;
				if (!next->device->create_resource_view(resource, reshade::api::resource_usage::shader_resource, srv, &next->view) ||
					!next->device->create_resource_view(resource, reshade::api::resource_usage::render_target, srv, &next->clearView)) {
					throw std::runtime_error("ReShade stable depth view creation failed");
				}
				if (output) g_retiredOutputs.push_back(output);
				output = std::move(next);
			}
		} catch (const std::exception& e) {
			g_failed = true;
			snapshot.reset();
			logger::error("[ReShade depth] Stable output allocation failed: {}", e.what());
		}
		const auto view = output ? output->view : reshade::api::resource_view{};
		const auto copyReady = g_copyReady;
		const auto copyValue = snapshot ? snapshot->copyValue : 0;
		auto depthBinding = g_depthBinding;
		const bool sameBinding = output && output == g_bound && depthBinding.handle;
		// A producer copy can still be in flight when ReShade begins its effects
		// pass. Keep the last completed stable output in that case. Clearing it to
		// far depth made bufready_depth false for every poll before the next fence
		// completion, so effects only saw depth on rare timing coincidences.
		const bool outputGenerationValid = output && output->generation == g_generation && output->copyValue != 0;
		const bool depthReady = snapshot || outputGenerationValid;
		if (output) {
			// Pin before unlocking or recording: a completed old read fence does
			// not protect a new, not-yet-submitted effect read. Deduplicate when
			// an output is retried without a finish_present notification.
			const auto thread = GetCurrentThreadId();
			const auto backBuffer = a_runtime->get_back_buffer(0).handle;
			for (const auto& retained : { snapshot, output }) {
				if (retained && std::none_of(g_pendingReads.begin(), g_pendingReads.end(), [&](const auto& read) {
					return read.thread == thread && read.backBuffer == backBuffer && read.slot == retained;
				})) {
					g_pendingReads.push_back({ thread, backBuffer, retained });
				}
			}
		}
		lock.unlock();
		// ReShade 6.8 waits idle even for identical descriptor updates. Check
		// the actual binding too, since Generic Depth can replace our semantic.
		reshade::api::resource_view current{}, currentSRGB{};
		if (sameBinding) a_runtime->get_texture_binding(depthBinding, &current, &currentSRGB);
		const bool updateBinding = !sameBinding || current != view || currentSRGB != view;
		BindDepth(a_runtime, view, updateBinding, depthReady);
		if (output) {
			const reshade::api::resource destination{ reinterpret_cast<uint64_t>(output->texture.get()) };
			if (snapshot && output->copyValue != copyValue) {
				a_list->barrier(destination, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::copy_dest);
				a_list->copy_resource({ reinterpret_cast<uint64_t>(snapshot->texture.get()) }, destination);
				a_list->barrier(destination, reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::shader_resource);
				output->copyValue = copyValue;
				output->generation = snapshot->generation;
			} else if (!snapshot && !outputGenerationValid) {
				// Do not expose an old generation or an uninitialized target. A
				// temporary fence miss at the current generation keeps the previous
				// stable output above instead of erasing it.
				const float farDepth[4]{ 1, 1, 1, 1 };
				a_list->barrier(destination, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::render_target);
				a_list->clear_render_target_view(output->clearView, farDepth);
				a_list->barrier(destination, reshade::api::resource_usage::render_target, reshade::api::resource_usage::shader_resource);
				output->copyValue = 0;
			}
		}
		if (updateBinding) {
			depthBinding = {};
			if (view.handle) a_runtime->enumerate_texture_variables(nullptr,
				[&](reshade::api::effect_runtime* runtime, reshade::api::effect_texture_variable variable) {
					if (depthBinding.handle) return;
					reshade::api::resource_view bound{};
					runtime->get_texture_binding(variable, &bound, nullptr);
					if (bound == view) depthBinding = variable;
				});
		}
		lock.lock();
		if (g_runtime != a_runtime || g_copyReady.get() != copyReady.get() || g_stopping) return;
		g_bound = output;
		g_depthBinding = depthBinding;
		if (view.handle && !g_loggedBinding) {
			logger::info("[ReShade depth] DEPTH bound: stable output texture; completed jitter-corrected snapshot copied on effect queue");
			g_loggedBinding = true;
		}
	}

	void OnFinishPresent(reshade::api::command_queue*, reshade::api::swapchain* a_swapchain)
	{
		if (a_swapchain->get_device()->get_api() != reshade::api::device_api::d3d12) return;
		// Serialize fence value allocation AND submission, without holding the
		// publication mutex during a queue call. Otherwise concurrent output
		// threads could signal the shared fence in decreasing value order.
		const std::lock_guard completionLock(g_completionMutex);
		std::unique_lock lock(g_mutex);
		if (!g_runtime || g_stopping || g_failed || g_pendingReads.empty()) return;
		const auto thread = GetCurrentThreadId();
		const auto backBuffer = a_swapchain->get_back_buffer(0).handle;
		const auto matches = [=](const auto& read) { return read.thread == thread && read.backBuffer == backBuffer; };
		if (std::none_of(g_pendingReads.begin(), g_pendingReads.end(), matches)) return;
		const auto readDone = g_readDone;
		const auto value = ++g_readValue;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		queue.copy_from(EffectQueue());
		lock.unlock();
		// ReShade's DXGI path flushes effects before the underlying Present;
		// this is the OUTPUT swapchain callback, not the proxy Present return.
		const auto result = queue->Signal(readDone.get(), value);
		lock.lock();
		if (readDone.get() != g_readDone.get() || g_stopping) return;
		if (FAILED(result)) {
			g_failed = true;
			logger::error("[ReShade depth] Output read completion signal failed; snapshots retained");
			return;
		}
		for (auto& read : g_pendingReads) {
			if (matches(read)) read.slot->readValue = value;
		}
		std::erase_if(g_pendingReads, matches);
	}
}

void ReShadeDepth::Initialize()
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&Initialize), &module)) return;
	if (!reshade::register_addon(module)) {
		logger::info("[ReShade depth] Add-on API unavailable; integration inactive");
		return;
	}
	reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitRuntime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
	reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
	reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);
	reshade::register_event<reshade::addon_event::finish_present>(OnFinishPresent);
	logger::info("[ReShade depth] Registered D3D12 DEPTH provider");
}

bool ReShadeDepth::IsRequested()
{
	const std::lock_guard lock(g_mutex);
	if (g_failed || g_stopping || g_runtimes.empty()) return false;
	auto* swap = DX12SwapChain::GetSingleton();
	if (!swap->IsReady()) return false;
	DXGI_SWAP_CHAIN_DESC desc{};
	if (FAILED(swap->swapChain->GetDesc(&desc))) return false;
		return std::any_of(g_runtimes.begin(), g_runtimes.end(), [&desc](auto* runtime) {
		return runtime->get_hwnd() == desc.OutputWindow && runtime->get_effects_state();
	});
}

bool ReShadeDepth::Prepare(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_source, uint32_t a_slot,
	winrt::com_ptr<ID3D12Resource>& a_retainedSnapshot, bool a_beforePresent)
{
	const std::lock_guard lock(g_mutex);
	g_prepared = false;
	a_retainedSnapshot = nullptr;
	if (!a_source || !a_list || a_slot >= kDX12FrameCount || !IsRequested()) {
		// Leave the last completed publication available to ReShade. The next
		// begin_effects callback can still consume it while the producer is
		// between scene submissions or its demand state is being refreshed.
		g_minSourcePresent = g_presentSerial + 1;
		return false;
	}
	try {
		auto* swap = DX12SwapChain::GetSingleton();
		winrt::com_ptr<ID3D12Device> sourceDevice;
		DX::ThrowIfFailed(a_source->GetDevice(IID_PPV_ARGS(sourceDevice.put())));
		DXGI_SWAP_CHAIN_DESC swapDesc{};
		DX::ThrowIfFailed(swap->swapChain->GetDesc(&swapDesc));
		if (!g_runtime) {
			// Runtime registration already filters D3D12. Match the game window,
			// not device identities exposed through ReShade/Streamline wrappers.
			for (auto* runtime : g_runtimes) {
				if (runtime->get_hwnd() == swapDesc.OutputWindow) {
					g_runtime = runtime;
					break;
				}
			}
			if (!g_runtime) return false;
			logger::info("[ReShade depth] Matched game D3D12 runtime; jitter-corrected display-resolution depth, no linearization");
		}
		if (!g_copyReady) {
			DX::ThrowIfFailed(sourceDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g_copyReady.put())));
		}
		if (!g_readDone) {
			DX::ThrowIfFailed(sourceDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g_readDone.put())));
		}
		auto desc = a_source->GetDesc();
		if (desc.Format != DXGI_FORMAT_R32_FLOAT || desc.SampleDesc.Count != 1 ||
			desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.MipLevels != 1) {
			g_published.reset();
			g_minSourcePresent = g_presentSerial + 1;
			return false;
		}
		if (g_sourceWidth != desc.Width || g_sourceHeight != desc.Height) {
			g_sourceWidth = desc.Width;
			g_sourceHeight = desc.Height;
			++g_generation;
			g_minSourcePresent = g_presentSerial;
			g_published.reset();
			logger::info("[ReShade depth] Source generation {}: {}x{}; old reads retained", g_generation, desc.Width, desc.Height);
		}
		// Neither queue waits for the other through this bridge. The consumer
		// only selects completed copies; polling also protects producer reuse.
		// Published, descriptor-bound and not-yet-submitted reads each retain a
		// shared_ptr. Fence completion alone is NOT sufficient for those slots.
		const auto available = std::find_if(g_slots.begin(), g_slots.end(), [](const auto& slot) {
			return !slot || (slot.use_count() == 1 && IsComplete(g_copyReady.get(), slot->copyValue) &&
				IsComplete(g_readDone.get(), slot->readValue));
		});
		// On pool pressure keep publishing the last snapshot, never block the
		// producer or allocate an unbounded queue of render-resolution textures.
		if (available == g_slots.end()) {
			return false;
		}
		auto& slot = *available;
		if (!slot || slot->texture->GetDesc().Width != desc.Width || slot->texture->GetDesc().Height != desc.Height) {
			auto replacement = std::make_shared<DepthSlot>();
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			DX::ThrowIfFailed(sourceDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(replacement->texture.put())));
			slot = std::move(replacement);
			logger::info("[ReShade depth] Snapshot slot {} resized to {}x{}", std::distance(g_slots.begin(), available), desc.Width, desc.Height);
		}
		// Quarantine an unsubmitted copy too. If submission/Present aborts,
		// the previous copy fence must not accidentally make this slot reusable.
		slot->copyValue = UINT64_MAX;
		D3D12_RESOURCE_BARRIER before[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(a_source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(slot->texture.get(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST)
		};
		a_list->ResourceBarrier(static_cast<UINT>(std::size(before)), before);
		a_list->CopyResource(slot->texture.get(), a_source);
		D3D12_RESOURCE_BARRIER after[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(a_source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(slot->texture.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ)
		};
		a_list->ResourceBarrier(static_cast<UINT>(std::size(after)), after);
		g_slot = static_cast<uint32_t>(std::distance(g_slots.begin(), available));
		// Evaluation happens before AdvancePresent advances the proxy serial.
		slot->sourcePresent = g_presentSerial + (a_beforePresent ? 1 : 0);
		slot->generation = g_generation;
		// Also retain the destination through the producer submission, even if
		// the ReShade runtime is reset between recording and publication.
		a_retainedSnapshot = slot->texture;
		g_prepared = true;
		return true;
	} catch (const std::exception& e) {
		g_failed = true;
		logger::error("[ReShade depth] Provider disabled: {}", e.what());
		return false;
	}
}

void ReShadeDepth::PublishSubmittedDepth()
{
	std::unique_lock lock(g_mutex);
	g_presenting = false;
	if (!g_prepared || !g_runtime || g_failed || g_stopping) return;
	const auto snapshot = g_slots[g_slot];
	const auto copyReady = g_copyReady;
	const auto value = ++g_copyValue;
	lock.unlock();
	const auto result = DX12SwapChain::GetSingleton()->commandQueue->Signal(copyReady.get(), value);
	lock.lock();
	if (copyReady.get() != g_copyReady.get() || g_stopping) return;
	if (FAILED(result)) {
		g_failed = true;
		logger::error("[ReShade depth] Copy-ready queue synchronization failed");
		return;
	}
	snapshot->copyValue = value;
	g_published = snapshot;
	g_presenting = true;
	g_prepared = false;
}

void ReShadeDepth::EndPresent()
{
	const std::lock_guard lock(g_mutex);
	// FG output callbacks may not even have started yet. Keep publication and
	// leave read completion to the corresponding ReShade finish_present event.
	g_presenting = g_prepared = false;
}

void ReShadeDepth::Invalidate()
{
	const std::lock_guard lock(g_mutex);
	g_published.reset();
	g_minSourcePresent = g_presentSerial + 1;
	g_prepared = false;
	// Existing output reads remain pinned until their own completion event.
}
void ReShadeDepth::AdvancePresent()
{
	const std::lock_guard lock(g_mutex);
	if (!g_runtimes.empty()) ++g_presentSerial;
}
