#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <d3d12.h>
#include <winrt/base.h>

namespace FrameTiming
{
	void SetEnabled(bool enabled, bool checkpoints = false);
	uint32_t CurrentFrame();
	void SampleQueueClock(ID3D12CommandQueue* queue, uint64_t gpuFrequency, uint32_t& lastFrame);

	class CpuScope
	{
	public:
		explicit CpuScope(const char* name, uint32_t frame = UINT32_MAX, bool checkpoint = false);
		~CpuScope();
		CpuScope(const CpuScope&) = delete;
		CpuScope& operator=(const CpuScope&) = delete;

	private:
		const char* name;
		uint32_t frame;
		int64_t start = 0;
	};

	void Begin11(ID3D11DeviceContext* context, uint32_t frame);
	void Finish11();

	class Gpu11Scope
	{
	public:
		explicit Gpu11Scope(const char* name);
		~Gpu11Scope();
		Gpu11Scope(const Gpu11Scope&) = delete;
		Gpu11Scope& operator=(const Gpu11Scope&) = delete;

	private:
		ID3D11DeviceContext* context = nullptr;
		ID3D11Query* end = nullptr;
	};

	// Lives with a command allocator. Collect after its existing fence has completed.
	class Gpu12Slot
	{
	public:
		void Begin(ID3D12Device* device, ID3D12GraphicsCommandList* list, const char* name);
		void End(ID3D12GraphicsCommandList* list);
		void Collect(uint64_t frequency);

	private:
		friend class Gpu12Scope;
		static constexpr uint32_t kMaxRanges = 8;
		uint32_t StartRange(const char* marker);
		void EndRange(uint32_t index);

		winrt::com_ptr<ID3D12QueryHeap> heap;
		winrt::com_ptr<ID3D12Resource> readback;
		ID3D12GraphicsCommandList* recording = nullptr;
		std::array<const char*, kMaxRanges> names{};
		uint32_t rangeCount = 0;
		uint32_t frame = 0;
		bool pending = false;
	};

	class Gpu12Scope
	{
	public:
		Gpu12Scope(ID3D12GraphicsCommandList* list, const char* name);
		~Gpu12Scope();
		Gpu12Scope(const Gpu12Scope&) = delete;
		Gpu12Scope& operator=(const Gpu12Scope&) = delete;

	private:
		Gpu12Slot* slot = nullptr;
		uint32_t range = UINT32_MAX;
	};
}
