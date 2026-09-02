#pragma once

#undef DEBUG

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#pragma warning(push)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "REX/REX.h"
#pragma warning(pop)

#include "Windows.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
using namespace std::literals;

#include "detours/detours.h"

#include "SimpleMath.h"

using float2 = DirectX::SimpleMath::Vector2;
using float3 = DirectX::SimpleMath::Vector3;
using float4 = DirectX::SimpleMath::Vector4;
using float4x4 = DirectX::SimpleMath::Matrix;
using uint = uint32_t;

#include <directx/d3dx12.h>

#include <magic_enum/magic_enum.hpp>

#ifdef NDEBUG
#	include <spdlog/sinks/basic_file_sink.h>
#else
#	include <spdlog/sinks/msvc_sink.h>
#endif
#include <spdlog/spdlog.h>



#define DLLEXPORT __declspec(dllexport)

namespace logger
{
	template <class... Args>
	void trace(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Trace, a_fmt, std::forward<Args>(a_args)...);
	}

	inline void trace(std::string_view a_msg)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Trace, a_msg);
	}

	template <class... Args>
	void debug(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Debug, a_fmt, std::forward<Args>(a_args)...);
	}

	inline void debug(std::string_view a_msg)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Debug, a_msg);
	}

	template <class... Args>
	void info(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Info, a_fmt, std::forward<Args>(a_args)...);
	}

	inline void info(std::string_view a_msg)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Info, a_msg);
	}

	template <class... Args>
	void warn(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Warning, a_fmt, std::forward<Args>(a_args)...);
	}

	inline void warn(std::string_view a_msg)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Warning, a_msg);
	}

	template <class... Args>
	void error(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Error, a_fmt, std::forward<Args>(a_args)...);
	}

	inline void error(std::string_view a_msg)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Error, a_msg);
	}

	template <class... Args>
	void critical(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Critical, a_fmt, std::forward<Args>(a_args)...);
	}

	inline void critical(std::string_view a_msg)
	{
		REX::Impl::Log(std::source_location::current(), REX::ELogLevel::Critical, a_msg);
	}
}

namespace stl
{
	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = REL::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

	template <class F, size_t index, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[index] };
		T::func = vtbl.write_vfunc(T::size, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		write_vfunc<F, 0, T>();
	}

	template <std::size_t idx, class T>
	void write_vfunc(REL::ID id)
	{
		REL::Relocation<std::uintptr_t> vtbl{ id };
		T::func = vtbl.write_vfunc(idx, T::thunk);
	}

	// Legacy Detours entry hook for runtime-discovered targets. Fixed Fallout 4
	// entry points should use detour_thunk_gateway() below.
	template <class T>
	void detour_thunk(REL::ID a_relId)
	{
		*(uintptr_t*)&T::func = Detours::X64::DetourFunction(a_relId.address(), (uintptr_t)&T::thunk);
	}

	struct RipRel32Patch
	{
		std::size_t instructionOffset;
		std::size_t displacementOffset;
		std::size_t instructionSize;
	};

	inline bool is_readable_memory(std::uintptr_t a_address, std::size_t a_size)
	{
		if (!a_address || !a_size) {
			return false;
		}

		MEMORY_BASIC_INFORMATION memoryInfo{};
		if (::VirtualQuery(
				reinterpret_cast<const void*>(a_address),
				std::addressof(memoryInfo),
				sizeof(memoryInfo)) != sizeof(memoryInfo)) {
			return false;
		}
		if (memoryInfo.State != MEM_COMMIT || (memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
			return false;
		}

		const auto regionBegin = reinterpret_cast<std::uintptr_t>(memoryInfo.BaseAddress);
		const auto regionEnd = regionBegin + memoryInfo.RegionSize;
		return a_address >= regionBegin && a_address <= regionEnd && a_size <= regionEnd - a_address;
	}

	inline std::int32_t make_rel32_displacement(std::uintptr_t a_sourceNext, std::uintptr_t a_destination)
	{
		const auto displacement = static_cast<std::int64_t>(a_destination) - static_cast<std::int64_t>(a_sourceNext);
		if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
			displacement > (std::numeric_limits<std::int32_t>::max)()) {
			REX::FAIL(
				"rel32 displacement out of range sourceNext={:x} destination={:x}",
				a_sourceNext,
				a_destination);
		}

		return static_cast<std::int32_t>(displacement);
	}

	inline bool write_branch5(std::uintptr_t a_source, std::uintptr_t a_destination)
	{
		auto& trampoline = REL::GetTrampoline();
		const auto branch = trampoline.allocate_branch5(a_destination);
		const REL::ASM::JMP5 assembly{ make_rel32_displacement(a_source + sizeof(REL::ASM::JMP5), branch) };
		if (!REL::WriteSafeData(a_source, assembly)) {
			return false;
		}
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<const void*>(a_source), sizeof(assembly));
		return true;
	}

	inline bool get_jump_destination(std::uintptr_t a_target, std::uintptr_t& a_destination)
	{
		a_destination = 0;
		if (!is_readable_memory(a_target, 2)) {
			return false;
		}

		const auto* code = reinterpret_cast<const std::uint8_t*>(a_target);
		if (code[0] == 0xE9) {
			if (!is_readable_memory(a_target, sizeof(REL::ASM::JMP5))) {
				return false;
			}
			std::int32_t displacement{};
			std::memcpy(&displacement, code + 1, sizeof(displacement));
			a_destination = static_cast<std::uintptr_t>(
				static_cast<std::intptr_t>(a_target + sizeof(REL::ASM::JMP5)) + displacement);
			return is_readable_memory(a_destination, 1);
		}
		if (code[0] == 0xFF && code[1] == 0x25) {
			if (!is_readable_memory(a_target, sizeof(REL::ASM::JMP6))) {
				return false;
			}
			std::int32_t displacement{};
			std::memcpy(&displacement, code + 2, sizeof(displacement));
			const auto indirectAddress = static_cast<std::uintptr_t>(
				static_cast<std::intptr_t>(a_target + sizeof(REL::ASM::JMP6)) + displacement);
			if (!is_readable_memory(indirectAddress, sizeof(a_destination))) {
				return false;
			}
			std::memcpy(&a_destination, reinterpret_cast<const void*>(indirectAddress), sizeof(a_destination));
			return is_readable_memory(a_destination, 1);
		}
		if (code[0] == 0x48 && code[1] == 0xB8 && is_readable_memory(a_target, 12) &&
			code[10] == 0xFF && code[11] == 0xE0) {
			std::memcpy(&a_destination, code + 2, sizeof(a_destination));
			return is_readable_memory(a_destination, 1);
		}
		if (code[0] == 0x68 && is_readable_memory(a_target, 6) && code[5] == 0xC3) {
			std::int32_t immediate{};
			std::memcpy(&immediate, code + 1, sizeof(immediate));
			a_destination = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(immediate));
			return is_readable_memory(a_destination, 1);
		}
		return false;
	}

	// Install a five-byte entry branch while preserving the original prologue or
	// an already-installed entry detour. The fixed prologue sizes are supplied by
	// the Fallout 4 call site and must end on instruction boundaries.
	template <class T>
	bool detour_thunk_gateway(
		REL::ID a_relId,
		std::size_t a_prologueSize,
		const char* a_name,
		std::initializer_list<RipRel32Patch> a_ripPatches = {})
	{
		const auto target = a_relId.address();
		const auto hook = reinterpret_cast<std::uintptr_t>(&T::thunk);
		if (!target || !hook || a_prologueSize < sizeof(REL::ASM::JMP5) ||
			!is_readable_memory(target, a_prologueSize)) {
			logger::error("{}: invalid entry/prologue", a_name ? a_name : "detour");
			return false;
		}

		std::uintptr_t previousDetour = 0;
		if (get_jump_destination(target, previousDetour)) {
			if (previousDetour == hook) {
				logger::error("{}: refusing to chain a hook to itself at {:x}", a_name ? a_name : "detour", target);
				return false;
			}

			auto& trampoline = REL::GetTrampoline();
			auto* gateway = trampoline.allocate<REL::ASM::JMP14>(previousDetour);
			::FlushInstructionCache(::GetCurrentProcess(), gateway, sizeof(REL::ASM::JMP14));
			if (!write_branch5(target, hook)) {
				logger::error("{}: failed to write existing-branch gateway at {:x}", a_name ? a_name : "detour", target);
				return false;
			}
			*(uintptr_t*)&T::func = reinterpret_cast<std::uintptr_t>(gateway);
			logger::info(
				"{}: chained existing entry branch {:x} -> {:x}",
				a_name ? a_name : "detour",
				target,
				previousDetour);
			return true;
		}

		auto& trampoline = REL::GetTrampoline();
		auto* gateway = static_cast<std::byte*>(trampoline.allocate(a_prologueSize + sizeof(REL::ASM::JMP14)));
		const auto* targetBytes = reinterpret_cast<const std::byte*>(target);
		std::memcpy(gateway, targetBytes, a_prologueSize);

		for (const auto& patch : a_ripPatches) {
			if (patch.displacementOffset + sizeof(std::int32_t) > a_prologueSize ||
				patch.instructionOffset + patch.instructionSize > a_prologueSize) {
				logger::error("{}: invalid RIP-relative patch descriptor", a_name ? a_name : "detour");
				return false;
			}

			std::int32_t oldDisplacement{};
			std::memcpy(&oldDisplacement, targetBytes + patch.displacementOffset, sizeof(oldDisplacement));
			const auto originalTarget = static_cast<std::uintptr_t>(
				static_cast<std::intptr_t>(target + patch.instructionOffset + patch.instructionSize) + oldDisplacement);
			const auto gatewayNext = reinterpret_cast<std::uintptr_t>(gateway) + patch.instructionOffset + patch.instructionSize;
			const auto newDisplacement = make_rel32_displacement(gatewayNext, originalTarget);
			std::memcpy(gateway + patch.displacementOffset, &newDisplacement, sizeof(newDisplacement));
		}

		const REL::ASM::JMP14 jumpBack{ target + a_prologueSize };
		std::memcpy(gateway + a_prologueSize, &jumpBack, sizeof(jumpBack));
		::FlushInstructionCache(
			::GetCurrentProcess(),
			gateway,
			a_prologueSize + sizeof(REL::ASM::JMP14));
		if (!write_branch5(target, hook)) {
			logger::error("{}: failed to write entry branch at {:x}", a_name ? a_name : "detour", target);
			return false;
		}

		*(uintptr_t*)&T::func = reinterpret_cast<std::uintptr_t>(gateway);
		logger::info(
			"{}: installed entry gateway at {:x}, prologue={} bytes",
			a_name ? a_name : "detour",
			target,
			a_prologueSize);
		return true;
	}

	template <class T>
	void detour_thunk_ignore_func(REL::ID a_relId)
	{
		std::ignore = Detours::X64::DetourFunction(a_relId.address(), (uintptr_t)&T::thunk);
	}

	template <std::size_t idx, class T>
	void detour_vfunc(void* target)
	{
		*(uintptr_t*)&T::func = Detours::X64::DetourClassVTable(*(uintptr_t*)target, &T::thunk, idx);
	}
}


namespace DX
{
	// Helper class for COM exceptions
	class com_exception : public std::exception
	{
	public:
		explicit com_exception(HRESULT hr) noexcept :
			result(hr) {}

		const char* what() const override
		{
			static char s_str[64] = {};
			sprintf_s(s_str, "Failure with HRESULT of %08X", static_cast<unsigned int>(result));
			return s_str;
		}

	private:
		HRESULT result;
	};

	// Helper utility converts D3D API failures into exceptions.
	inline void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr)) {
			throw com_exception(hr);
		}
	}
}

#include "Plugin.h"
