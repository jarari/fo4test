#include "ReShadeDepth.h"
#include "DX12SwapChain.h"
#include "Upscaling.h"
#include "../extern/ReShade/include/reshade.hpp"

#include <algorithm>
#include <cstring>
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
		uint64_t generation = 0;
		uint64_t readyValue = 0;
		uint64_t readValue = 0;
		uint32_t sourceSlot = 0;
		~DepthSlot()
		{
			if (device && view.handle) device->destroy_resource_view(view);
		}
	};
	struct PendingRead
	{
		void* hwnd = nullptr;
		std::shared_ptr<DepthSlot> slot;
	};
	std::recursive_mutex g_mutex;
	std::mutex g_completionMutex;
	std::vector<reshade::api::effect_runtime*> g_runtimes;
	std::unordered_map<reshade::api::effect_runtime*, bool> g_effectDemand;
	reshade::api::effect_runtime* g_runtime = nullptr;
	std::array<std::shared_ptr<DepthSlot>, kDX12FrameCount> g_slots;
	std::shared_ptr<DepthSlot> g_published;
	std::shared_ptr<DepthSlot> g_bound;
	std::vector<PendingRead> g_pendingReads;
	winrt::com_ptr<ID3D12Fence> g_readyFence;
	winrt::com_ptr<ID3D12Fence> g_readFence;
	uint64_t g_generation = 0;
	uint64_t g_sourceWidth = 0;
	uint64_t g_sourceHeight = 0;
	uint64_t g_readValue = 0;
	reshade::api::effect_texture_variable g_depthBinding{};
	bool g_failed = false;
	bool g_stopping = false;
	thread_local std::shared_ptr<DepthSlot> g_threadEffectSlot;

	void BindDepth(reshade::api::effect_runtime* a_runtime, reshade::api::resource_view a_view,
		bool a_updateBinding, bool a_ready)
	{
		if (a_updateBinding) a_runtime->update_texture_bindings("DEPTH", a_view, a_view);
		a_runtime->enumerate_uniform_variables(nullptr, [a_view, a_ready](reshade::api::effect_runtime* runtime,
			reshade::api::effect_uniform_variable variable) {
			char source[32]{};
			if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) &&
				std::strcmp(source, "bufready_depth") == 0) {
				runtime->set_uniform_value_bool(variable, a_ready && a_view.handle != 0);
			}
		});
	}

	bool EnsureView(reshade::api::effect_runtime* a_runtime, const std::shared_ptr<DepthSlot>& a_slot)
	{
		if (!a_slot || !a_slot->texture) return false;
		if (a_slot->device == a_runtime->get_device() && a_slot->view.handle) return true;
		if (a_slot->device && a_slot->view.handle) a_slot->device->destroy_resource_view(a_slot->view);
		a_slot->view = {};
		a_slot->device = a_runtime->get_device();
		const reshade::api::resource resource{ reinterpret_cast<uint64_t>(a_slot->texture.get()) };
		reshade::api::resource_view_desc desc(reshade::api::format::r32_float);
		desc.type = reshade::api::resource_view_type::texture_2d;
		desc.texture.levels = 1;
		desc.texture.layers = 1;
		if (!a_slot->device->create_resource_view(resource, reshade::api::resource_usage::shader_resource, desc, &a_slot->view)) {
			a_slot->device = nullptr;
			return false;
		}
		return true;
	}

	void OnInitRuntime(reshade::api::effect_runtime* a_runtime)
	{
		if (a_runtime->get_device()->get_api() != reshade::api::device_api::d3d12) return;
		const std::lock_guard lock(g_mutex);
		g_runtimes.push_back(a_runtime);
		g_effectDemand[a_runtime] = false;
		if (!g_runtime) g_runtime = a_runtime;
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
		if (a_runtime != g_runtime) return;
		g_stopping = true;
		g_published.reset();
		g_bound.reset();
		lock.unlock();
		BindDepth(a_runtime, {}, true, false);
		a_runtime->get_command_queue()->wait_idle();
		lock.lock();
		for (auto& slot : g_slots) {
			if (slot && slot->device && slot->view.handle) {
				slot->device->destroy_resource_view(slot->view);
				slot->view = {};
				slot->device = nullptr;
			}
		}
		g_slots = {};
		g_pendingReads.clear();
		g_depthBinding = {};
		g_readyFence = nullptr;
		g_readFence = nullptr;
		g_readValue = 0;
		g_generation = 0;
		g_sourceWidth = g_sourceHeight = 0;
		g_runtime = nullptr;
		g_stopping = false;
	}

	void OnBeginEffects(reshade::api::effect_runtime* a_runtime, reshade::api::command_list* a_list,
		reshade::api::resource_view, reshade::api::resource_view)
	{
		g_threadEffectSlot.reset();
		bool enabled = false;
		if (a_runtime->get_effects_state()) {
			a_runtime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime* runtime,
				reshade::api::effect_technique technique) {
				enabled = enabled || runtime->get_technique_state(technique);
			});
		}
		std::shared_ptr<DepthSlot> slot;
		reshade::api::resource_view view{};
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0;
		bool updateBinding = false;
		{
			std::unique_lock lock(g_mutex);
			if (g_effectDemand.contains(a_runtime)) g_effectDemand[a_runtime] = enabled;
			if (!enabled || a_runtime != g_runtime || g_stopping || !a_list) return;
			g_readyFence.copy_from(DX12SwapChain::GetSingleton()->GetReShadeDepthReadyFence());
			g_readFence.copy_from(DX12SwapChain::GetSingleton()->GetReShadeDepthReadFence());
			slot = g_published;
			if (!slot || slot->generation != g_generation || !slot->readyValue || !EnsureView(a_runtime, slot)) {
				slot.reset();
			} else {
				view = slot->view;
				readyValue = slot->readyValue;
				updateBinding = !g_bound || g_bound != slot || !g_depthBinding.handle;
			}
		}

		if (slot && readyFence && readyValue) {
			const auto completed = readyFence->GetCompletedValue();
			if (completed < readyValue) {
				auto* queue = reinterpret_cast<ID3D12CommandQueue*>(a_runtime->get_command_queue()->get_native());
				if (!queue || FAILED(queue->Wait(readyFence.get(), readyValue))) {
					logger::warn("[ReShade depth] Direct ready-fence wait could not be queued; keeping previous DEPTH");
					slot.reset();
				}
			}
			if (slot) {
				const reshade::api::resource resource{ reinterpret_cast<uint64_t>(slot->texture.get()) };
				a_list->barrier(resource, reshade::api::resource_usage::general, reshade::api::resource_usage::shader_resource);
			}
		}

		{
			const std::lock_guard lock(g_mutex);
			if (slot && g_runtime == a_runtime && !g_stopping) {
				const auto hwnd = a_runtime->get_hwnd();
				if (std::none_of(g_pendingReads.begin(), g_pendingReads.end(), [&](const auto& read) {
					return read.hwnd == hwnd && read.slot == slot;
				})) g_pendingReads.push_back({ hwnd, slot });
				g_bound = slot;
				g_threadEffectSlot = slot;
			}
		}
		BindDepth(a_runtime, view, updateBinding, slot != nullptr);
	}

	void OnFinishEffects(reshade::api::effect_runtime*, reshade::api::command_list* a_list,
		reshade::api::resource_view, reshade::api::resource_view)
	{
		const auto slot = g_threadEffectSlot;
		if (slot && a_list) {
			const reshade::api::resource resource{ reinterpret_cast<uint64_t>(slot->texture.get()) };
			a_list->barrier(resource, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::general);
		}
		g_threadEffectSlot.reset();
	}

	void OnFinishPresent(reshade::api::command_queue* a_queue, reshade::api::swapchain* a_swapchain)
	{
		if (!a_queue || !a_swapchain || a_swapchain->get_device()->get_api() != reshade::api::device_api::d3d12) return;
		const std::lock_guard completionLock(g_completionMutex);
		std::unique_lock lock(g_mutex);
		if (!g_runtime || g_stopping || g_pendingReads.empty() || a_swapchain->get_hwnd() != g_runtime->get_hwnd()) return;
		const auto hwnd = a_swapchain->get_hwnd();
		const auto matches = [=](const auto& read) { return read.hwnd == hwnd; };
		const auto released = static_cast<std::size_t>(std::count_if(g_pendingReads.begin(), g_pendingReads.end(), matches));
		if (!released) return;
		const auto readFence = g_readFence;
		const auto value = ++g_readValue;
		auto* queue = reinterpret_cast<ID3D12CommandQueue*>(a_queue->get_native());
		lock.unlock();
		const auto result = queue && readFence ? queue->Signal(readFence.get(), value) : E_POINTER;
		lock.lock();
		if (FAILED(result) || g_stopping) return;
		for (auto& read : g_pendingReads) {
			if (matches(read)) {
				read.slot->readValue = value;
				DX12SwapChain::GetSingleton()->MarkReShadeDepthRead(read.slot->sourceSlot, value);
			}
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
	reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
	reshade::register_event<reshade::addon_event::finish_present>(OnFinishPresent);
	logger::info("[ReShade depth] Registered direct shared DEPTH provider");
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

void ReShadeDepth::PublishCapturedDepth(ID3D12Resource* a_resource, uint32_t a_sourceSlot,
	uint64_t, uint64_t a_readyFence)
{
	if (!a_resource || !a_readyFence || a_sourceSlot >= kDX12FrameCount) return;
	const std::lock_guard lock(g_mutex);
	if (g_stopping || g_failed || g_runtimes.empty()) return;
	auto* swap = DX12SwapChain::GetSingleton();
	g_readyFence.copy_from(swap->GetReShadeDepthReadyFence());
	g_readFence.copy_from(swap->GetReShadeDepthReadFence());
	const auto desc = a_resource->GetDesc();
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != DXGI_FORMAT_R32_FLOAT ||
		desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.DepthOrArraySize != 1) return;
	if (g_sourceWidth != desc.Width || g_sourceHeight != desc.Height) {
		g_sourceWidth = desc.Width;
		g_sourceHeight = desc.Height;
		++g_generation;
		g_published.reset();
		g_bound.reset();
		g_depthBinding = {};
	}
	auto slot = std::make_shared<DepthSlot>();
	slot->texture.copy_from(a_resource);
	slot->generation = g_generation;
	slot->readyValue = a_readyFence;
	slot->sourceSlot = a_sourceSlot;
	g_slots[a_sourceSlot] = slot;
	g_published = std::move(slot);
}

void ReShadeDepth::Invalidate()
{
	const std::lock_guard lock(g_mutex);
	++g_generation;
	g_published.reset();
	g_bound.reset();
	g_depthBinding = {};
}

void ReShadeDepth::AdvancePresent()
{
}
