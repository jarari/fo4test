#include "NRDiagnosticCapture.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <wrl/client.h>

namespace NRDiagnosticCapture
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using Clock = std::chrono::steady_clock;
		std::atomic<bool> requested{ false }, hasRecording{ false };
		struct Image
		{
			std::string name;
			ComPtr<ID3D12Resource> readback;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			UINT rows = 0;
			UINT64 sourceWidth = 0;
			UINT sourceHeight = 0;
			UINT64 rowBytes = 0, bytes = 0;
		};
		struct Frame
		{
			std::filesystem::path directory;
			std::string metadata;
			std::vector<Image> images;
			ComPtr<ID3D12Fence> fence;
			UINT64 fenceValue = 0;
			UINT number = 0;
			bool nr = false, sr = false;
			UINT passes = 0;
		};
		struct State
		{
			std::mutex mutex;
			std::condition_variable cv;
			bool armed = false, menuOpen = false, running = false;
			Clock::time_point due{};
			UINT recorded = 0, written = 0, outstanding = 0;
			uint32_t lastFrame = UINT32_MAX;
			UINT64 allocated = 0;
			std::string status = "Idle";
			std::filesystem::path directory;
			std::unordered_map<ID3D12GraphicsCommandList*, Frame> recording;
			std::deque<Frame> pending;
			std::jthread writer;
		};
		State& Get()
		{
			// Process-lifetime storage also retains un-fenced copies if submission
			// fails. Never destroy their readbacks based on a timeout or frame count.
			static auto* state = new State;
			return *state;
		}
		void Fail(State& state, const char* reason)
		{
			state.armed = state.running = false;
			requested = false;
			state.status = std::string("Capture aborted: ") + reason;
			logger::warn("[NR capture] {}", state.status);
		}
		void Write(Frame& frame)
		{
			std::filesystem::create_directories(frame.directory);
			const auto stem = std::format("frame_{:02}", frame.number);
			std::ofstream meta(frame.directory / (stem + ".json"));
			meta.exceptions(std::ios::badbit | std::ios::failbit);
			meta << "{\n" << frame.metadata << ",\n\"nr_succeeded\":" << (frame.nr ? "true" : "false")
				<< ",\"sr_succeeded\":" << (frame.sr ? "true" : "false")
				<< ",\"evaluated_passes\":" << frame.passes
				<< ",\"complete\":" << (frame.nr && frame.sr && frame.images.size() >= 5 ? "true" : "false")
				<< ",\n\"images\":[\n";
			bool first = true;
			for (auto& image : frame.images) {
				const auto filename = stem + "_" + image.name + ".bin";
				std::ofstream output(frame.directory / filename, std::ios::binary);
				output.exceptions(std::ios::badbit | std::ios::failbit);
				void* mapped = nullptr;
				D3D12_RANGE range{ 0, static_cast<SIZE_T>(image.bytes) };
				if (FAILED(image.readback->Map(0, &range, &mapped))) {
					throw std::runtime_error("readback Map failed");
				}
				const D3D12_RANGE noWrites{ 0, 0 };
				try {
					for (UINT row = 0; row < image.rows; ++row) {
						output.write(static_cast<const char*>(mapped) + image.footprint.Offset +
							static_cast<size_t>(row) * image.footprint.Footprint.RowPitch,
							static_cast<std::streamsize>(image.rowBytes));
					}
				} catch (...) {
					image.readback->Unmap(0, &noWrites);
					throw;
				}
				image.readback->Unmap(0, &noWrites);
				output.close();
				if (!first) { meta << ",\n"; }
				first = false;
				meta << std::format("{{\"file\":\"{}\",\"width\":{},\"height\":{},\"dxgi_format\":{},\"row_bytes\":{},\"source_width\":{},\"source_height\":{}}}",
					filename, image.footprint.Footprint.Width, image.footprint.Footprint.Height,
					static_cast<UINT>(image.footprint.Footprint.Format), image.rowBytes, image.sourceWidth, image.sourceHeight);
			}
			meta << "\n]}\n";
			meta.close();
			if (frame.number == 0) {
				std::ofstream readme(frame.directory / "README.txt");
				readme << "NR diagnostic capture: 32 consecutive engine frames, before UI/ReShade/FG.\n"
					"Each JSON describes the accompanying raw little-endian .bin images.\n"
					"Rows are tightly packed (row_bytes), top-left origin, no D3D12 pitch padding.\n"
					"DXGI formats: 28=RGBA8 UNORM; 34=RG16 FLOAT (motion); 41=R32 FLOAT (depth).\n"
					"Optional nr_mv: format 34=RG16 FLOAT, NR-only pixel motion; jitter delta applies only before SR.\n"
					"Optional reshade_depth: corrected ReShade depth snapshot. JSON records physical source, valid sample, output extents, and engine/Streamline jitter.\n"
					"ReShade sampling is bilinear at (id+0.5)*sample_extent/output_extent+engine_jitter; Streamline jitter is the negated engine jitter.\n"
					"Other formats retain their original DXGI numeric format; no gamma or range conversion.\n"
					"input is render-size; SR is display-size before NIS/UI. NR extent follows nr_position (before_sr/after_sr).\n"
					"jitter is the published Streamline jitter in pixels. MV pixels are unmodified engine values.\n"
					"nr_mv/nr_depth are NR guides; nr_guide_sample_offset is applied when reading the raw raster guides.\n"
					"nr_pass_N contains actual direct-NGX reset and scale parameters, not inferred values.\n"
					"Missing frames or complete=false indicate an incomplete capture; retain the plugin log.\n";
			}
		}
		void Writer(State& state)
		{
			for (;;) {
				Frame frame;
				{
					std::unique_lock lock(state.mutex);
					state.cv.wait(lock, [&] { return !state.pending.empty(); });
					frame = std::move(state.pending.front());
					state.pending.pop_front();
				}
				UINT64 completed = 0;
				while ((completed = frame.fence->GetCompletedValue()) < frame.fenceValue) {
					// Only this background writer waits. The render thread never maps
					// or waits for a capture, and the existing queue/fence is unchanged.
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				std::string error;
				try {
					if (completed == UINT64_MAX) { throw std::runtime_error("device removed"); }
					Write(frame);
				} catch (const std::exception& e) { error = e.what(); }
				std::scoped_lock lock(state.mutex);
				for (const auto& image : frame.images) { state.allocated -= image.bytes; }
				--state.outstanding;
				if (!error.empty()) { Fail(state, error.c_str()); }
				else if (++state.written == 32 && state.running) {
					state.running = false;
					requested = false;
					state.status = "Complete: 32 frames saved";
					logger::info("[NR capture] Complete: {}", state.directory.string());
				}
			}
		}
	}

	void Arm()
	{
		auto& s = Get();
		std::scoped_lock lock(s.mutex);
		if (s.running || s.outstanding) { return; }
		if (!s.writer.joinable()) { s.writer = std::jthread([&s] { Writer(s); }); }
		s.armed = true;
		requested = true;
		s.due = Clock::time_point::max();
		s.status = "Armed: close the menu; capture starts after 2 seconds";
		logger::info("[NR capture] Armed; waiting for framework menu close");
	}
	void MenuChanged(bool open)
	{
		auto& s = Get();
		std::scoped_lock lock(s.mutex);
		s.menuOpen = open;
		if (s.running && s.recorded < 32 && open) { Fail(s, "menu reopened during capture"); }
		if (s.armed) {
			s.due = open ? Clock::time_point::max() : Clock::now() + std::chrono::seconds(2);
			s.status = open ? "Armed: close the menu" : "Waiting 2 seconds, then waiting for NR evaluation";
		}
	}
	std::string Status()
	{
		auto& s = Get();
		std::scoped_lock lock(s.mutex);
		return s.running ? std::format("Captured {}/32; saved {}/32", s.recorded, s.written) : s.status;
	}
	bool Requested() { return requested.load(std::memory_order_relaxed); }
	void Annotate(ID3D12GraphicsCommandList* list, std::string metadata)
	{
		if (!hasRecording.load(std::memory_order_relaxed)) { return; }
		auto& s = Get();
		std::scoped_lock lock(s.mutex);
		if (auto it = s.recording.find(list); it != s.recording.end()) { it->second.metadata += ",\n" + metadata; }
	}
	bool Begin(ID3D12GraphicsCommandList* list, uint32_t frame, std::string metadata)
	{
		auto& s = Get();
		std::scoped_lock lock(s.mutex);
		if (s.menuOpen) { return false; }
		if (s.armed && Clock::now() >= s.due) {
			s.armed = false;
			s.running = true;
			s.recorded = s.written = 0;
			s.lastFrame = UINT32_MAX;
			s.directory = std::filesystem::absolute("Data/F4SE/Plugins/Upscaling/NR-Captures") /
				std::format("{}-{}", GetCurrentProcessId(), GetTickCount64());
			logger::info("[NR capture] Starting 32 frames: {}", s.directory.string());
		}
		if (!s.running || s.recorded >= 32 || s.lastFrame == frame) { return false; }
		if (s.recording.contains(list)) { Fail(s, "unsubmitted capture list reused"); return false; }
		if (s.recorded && frame != s.lastFrame + 1u) {
			Fail(s, "NR frame discontinuity; partial capture retained"); return false;
		}
		s.lastFrame = frame;
		Frame entry;
		entry.number = s.recorded++;
		entry.directory = s.directory;
		entry.metadata = std::move(metadata);
		entry.metadata += std::format(",\"capture_time_us\":{}", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
		s.recording.emplace(list, std::move(entry));
		++s.outstanding;
		hasRecording = true;
		return true;
	}
	void Copy(ID3D12GraphicsCommandList* list, const char* name, ID3D12Resource* source, UINT width, UINT height)
	{
		auto& s = Get();
		std::scoped_lock lock(s.mutex);
		auto it = s.recording.find(list);
		if (it == s.recording.end() || !s.running) { return; }
		if (!source || !width || !height) { Fail(s, "missing source"); return; }
		auto desc = source->GetDesc();
		if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
			desc.DepthOrArraySize != 1 || width > desc.Width || height > desc.Height) {
			Fail(s, "invalid source extent"); return;
		}
		const auto sourceWidth = desc.Width;
		const auto sourceHeight = desc.Height;
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		ComPtr<ID3D12Device> device;
		if (FAILED(source->GetDevice(IID_PPV_ARGS(&device)))) { Fail(s, "missing device"); return; }
		Image image;
		image.sourceWidth = sourceWidth;
		image.sourceHeight = sourceHeight;
		image.name = name;
		device->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, &image.rows, &image.rowBytes, &image.bytes);
		// ReShade depth is captured at presentation resolution so the diagnostic
		// image matches the actual DEPTH binding. Keep the bounded budget large
		// enough for that extra full-resolution readback, but still abort rather
		// than stall or silently omit frames if disk/GPU progress cannot keep up.
		constexpr auto readbackBudget = 2ull * 1024 * 1024 * 1024;
		if (image.bytes > readbackBudget || s.allocated > readbackBudget - image.bytes) {
			Fail(s, "2 GiB readback budget exceeded"); return;
		}
		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
		const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(image.bytes);
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image.readback)))) {
			Fail(s, "readback allocation failed"); return;
		}
		it->second.images.push_back(std::move(image));
		auto& retained = it->second.images.back();
		s.allocated += retained.bytes;
		D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->ResourceBarrier(1, &barrier);
		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = retained.readback.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		destination.PlacedFootprint = retained.footprint;
		D3D12_TEXTURE_COPY_LOCATION input{};
		input.pResource = source;
		input.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		const D3D12_BOX box{ 0, 0, 0, width, height, 1 };
		list->CopyTextureRegion(&destination, 0, 0, 0, &input, &box);
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		list->ResourceBarrier(1, &barrier);
	}
	void Finish(ID3D12GraphicsCommandList* list, bool nrSucceeded, bool srSucceeded, UINT passes)
	{
		auto& s = Get();
		std::scoped_lock lock(s.mutex);
		if (auto it = s.recording.find(list); it != s.recording.end()) {
			it->second.nr = nrSucceeded;
			it->second.sr = srSucceeded;
			it->second.passes = passes;
			if (!nrSucceeded || !srSucceeded) { Fail(s, "NR/SR evaluation failed; partial capture retained"); }
		}
	}
	void Submitted(ID3D12GraphicsCommandList* list, ID3D12Fence* fence, UINT64 value)
	{
		if (!hasRecording.load(std::memory_order_relaxed)) { return; }
		auto& s = Get();
		std::scoped_lock lock(s.mutex);
		if (auto it = s.recording.find(list); it != s.recording.end()) {
			it->second.fence = fence;
			it->second.fenceValue = value;
			s.pending.push_back(std::move(it->second));
			s.recording.erase(it);
			hasRecording = !s.recording.empty();
			s.cv.notify_one();
		}
	}
}
