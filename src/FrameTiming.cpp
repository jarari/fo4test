#include "FrameTiming.h"
#include "Util.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace FrameTiming
{
	namespace
	{
		std::atomic_bool enabled{ false };
		std::atomic_bool checkpointsEnabled{ false };
		std::mutex outputMutex;
		FILE* output = nullptr;
		uint32_t rows = 0;
		thread_local bool active11 = false;
		thread_local Gpu12Slot* active12 = nullptr;

		int64_t Counter()
		{
			LARGE_INTEGER value{};
			QueryPerformanceCounter(&value);
			return value.QuadPart;
		}

		uint64_t CounterFrequency()
		{
			static const auto frequency = [] {
				LARGE_INTEGER value{};
				QueryPerformanceFrequency(&value);
				return static_cast<uint64_t>(value.QuadPart);
			}();
			return frequency;
		}

		void Write(uint32_t frame, const char* domain, const char* name, uint64_t start, uint64_t end, uint64_t frequency, bool flush = false)
		{
			if (!frequency || end < start) return;
			std::scoped_lock lock(outputMutex);
			if (!output) return;
			std::fprintf(output, "%u,%s,%s,%lu,%llu,%llu,%.6f\n", frame, domain, name, GetCurrentThreadId(),
				static_cast<unsigned long long>(start), static_cast<unsigned long long>(frequency),
				static_cast<double>(end - start) * 1000.0 / static_cast<double>(frequency));
			if (++rows % 128 == 0 || flush) std::fflush(output);
		}

		struct Pair11
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
			const char* name = nullptr;
		};

		struct Sample11
		{
			winrt::com_ptr<ID3D11Query> disjoint;
			std::array<Pair11, 48> pairs;
			size_t count = 0;
			uint32_t frame = 0;
			bool pending = false;
		};

		struct State11
		{
			winrt::com_ptr<ID3D11Device> device;
			winrt::com_ptr<ID3D11DeviceContext> context;
			std::array<Sample11, 24> samples;
			Sample11* active = nullptr;
			size_t next = 0;
			ID3D11Query* frameEnd = nullptr;
		};

		State11& Timing11()
		{
			static thread_local State11 state;
			return state;
		}

		Pair11* StartPair(State11& state, const char* name)
		{
			if (!state.active || state.active->count == state.active->pairs.size()) return nullptr;
			auto& pair = state.active->pairs[state.active->count];
			const D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
			if (!pair.begin && FAILED(state.device->CreateQuery(&desc, pair.begin.put()))) return nullptr;
			if (!pair.end && FAILED(state.device->CreateQuery(&desc, pair.end.put()))) return nullptr;
			++state.active->count;
			pair.name = name;
			state.context->End(pair.begin.get());
			return &pair;
		}

		void Collect11(State11& state, Sample11& sample)
		{
			if (!sample.pending) return;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			const auto hr = state.context->GetData(sample.disjoint.get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (hr == S_FALSE) return;
			sample.pending = false;
			if (FAILED(hr) || disjoint.Disjoint) return;
			for (size_t i = 0; i < sample.count; ++i) {
				const auto& pair = sample.pairs[i];
				uint64_t begin = 0, end = 0;
				if (state.context->GetData(pair.begin.get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
					state.context->GetData(pair.end.get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
					Write(sample.frame, "D3D11", pair.name, begin, end, disjoint.Frequency);
				}
			}
		}
	}

	void SetEnabled(bool requested, bool checkpoints)
	{
		std::scoped_lock lock(outputMutex);
		if (!requested) {
			enabled.store(false, std::memory_order_relaxed);
			if (output) std::fflush(output);
			return;
		}
		if (!output) {
			wchar_t temporary[MAX_PATH]{};
			const auto length = GetTempPathW(MAX_PATH, temporary);
			if (!length || length >= MAX_PATH) return;
			const auto path = std::wstring(temporary) + L"Upscaling-FrameTiming-" + std::to_wstring(GetCurrentProcessId()) + L".csv";
			output = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
			if (!output) return;
			std::setvbuf(output, nullptr, _IOFBF, 32768);
			std::fputs("frame,domain,marker,thread,start_ticks,ticks_per_second,duration_ms\n", output);
		}
		checkpointsEnabled.store(checkpoints, std::memory_order_relaxed);
		enabled.store(true, std::memory_order_relaxed);
	}

	uint32_t CurrentFrame()
	{
		return Util::State_GetSingleton()->frameCount;
	}

	void SampleQueueClock(ID3D12CommandQueue* queue, uint64_t gpuFrequency, uint32_t& lastFrame)
	{
		if (!enabled.load(std::memory_order_relaxed) || !gpuFrequency) return;
		const auto frame = CurrentFrame();
		if (lastFrame != UINT32_MAX && frame - lastFrame < 128) return;
		lastFrame = frame;
		uint64_t gpu = 0, qpc = 0;
		if (FAILED(queue->GetClockCalibration(&gpu, &qpc))) return;
		Write(frame, "Clock.D3D12", "Queue.Calibration", gpu, gpu, gpuFrequency);
		Write(frame, "Clock.QPC", "Queue.Calibration", qpc, qpc, CounterFrequency());
	}

	CpuScope::CpuScope(const char* marker, uint32_t frameNumber, bool checkpoint) : name(marker), frame(frameNumber)
	{
		if (!enabled.load(std::memory_order_relaxed)) return;
		if (frame == UINT32_MAX) frame = CurrentFrame();
		start = Counter();
		if (checkpoint && checkpointsEnabled.load(std::memory_order_relaxed)) {
			// A blocked SDK call has no end sample. Flush its entry so a live
			// reader can still locate the render thread and the last frame.
			Write(frame, "CPU.Begin", name, start, start, CounterFrequency(), true);
		}
	}

	CpuScope::~CpuScope()
	{
		if (start) Write(frame, "CPU", name, start, Counter(), CounterFrequency());
	}

	void Begin11(ID3D11DeviceContext* context, uint32_t frame)
	{
		if (!enabled.load(std::memory_order_relaxed) || !context) return;
		Finish11();
		auto& state = Timing11();
		if (state.context.get() != context) {
			state = {};
			state.context.copy_from(context);
			context->GetDevice(state.device.put());
		}
		for (auto& sample : state.samples) Collect11(state, sample);
		auto& sample = state.samples[state.next];
		state.next = (state.next + 1) % state.samples.size();
		if (sample.pending) return;
		const D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		if (!sample.disjoint && FAILED(state.device->CreateQuery(&desc, sample.disjoint.put()))) return;
		sample.frame = frame;
		sample.count = 0;
		state.active = &sample;
		active11 = true;
		context->Begin(sample.disjoint.get());
		if (const auto* pair = StartPair(state, "Frame.RendererBeginToUIReady")) state.frameEnd = pair->end.get();
	}

	void Finish11()
	{
		if (!active11) return;
		auto& state = Timing11();
		if (state.frameEnd) state.context->End(state.frameEnd);
		state.context->End(state.active->disjoint.get());
		state.active->pending = true;
		state.active = nullptr;
		state.frameEnd = nullptr;
		active11 = false;
	}

	Gpu11Scope::Gpu11Scope(const char* name)
	{
		if (!enabled.load(std::memory_order_relaxed)) return;
		auto& state = Timing11();
		if (const auto* pair = StartPair(state, name)) {
			context = state.context.get();
			end = pair->end.get();
		}
	}

	Gpu11Scope::~Gpu11Scope()
	{
		if (end) context->End(end);
	}

	void Gpu12Slot::Begin(ID3D12Device* device, ID3D12GraphicsCommandList* list, const char* marker)
	{
		if (!enabled.load(std::memory_order_relaxed)) return;
		if (!heap) {
			D3D12_QUERY_HEAP_DESC query{};
			query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			query.Count = kMaxRanges * 2;
			if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(heap.put())))) return;
		}
		if (!readback) {
			const auto props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(kMaxRanges * 2 * sizeof(uint64_t));
			if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.put())))) return;
		}
		frame = CurrentFrame();
		recording = list;
		rangeCount = 0;
		StartRange(marker);
		active12 = this;
	}

	uint32_t Gpu12Slot::StartRange(const char* marker)
	{
		if (rangeCount == kMaxRanges) return UINT32_MAX;
		const auto index = rangeCount++;
		names[index] = marker;
		recording->EndQuery(heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2);
		return index;
	}

	void Gpu12Slot::EndRange(uint32_t index)
	{
		recording->EndQuery(heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2 + 1);
	}

	void Gpu12Slot::End(ID3D12GraphicsCommandList* list)
	{
		if (recording != list) return;
		EndRange(0);
		list->ResolveQueryData(heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, rangeCount * 2, readback.get(), 0);
		active12 = nullptr;
		recording = nullptr;
		pending = true;
	}

	void Gpu12Slot::Collect(uint64_t frequency)
	{
		if (pending) {
			const D3D12_RANGE read{ 0, rangeCount * 2 * sizeof(uint64_t) };
			void* data = nullptr;
			if (SUCCEEDED(readback->Map(0, &read, &data))) {
				const auto* times = static_cast<const uint64_t*>(data);
				for (uint32_t i = 0; i < rangeCount; ++i) {
					Write(frame, "D3D12", names[i], times[2 * i], times[2 * i + 1], frequency);
				}
				const D3D12_RANGE written{ 0, 0 };
				readback->Unmap(0, &written);
			}
		}
		pending = false;
		rangeCount = 0;
	}

	Gpu12Scope::Gpu12Scope(ID3D12GraphicsCommandList* list, const char* name)
	{
		if (!active12 || active12->recording != list) return;
		slot = active12;
		range = slot->StartRange(name);
	}

	Gpu12Scope::~Gpu12Scope()
	{
		if (range != UINT32_MAX) slot->EndRange(range);
	}
}
