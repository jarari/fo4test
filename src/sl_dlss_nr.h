#pragma once

// Provisional DLSS-NR Streamline ABI.
//
// NVIDIA's Streamline 2.13 DLSS-NR preview runtime does not currently ship a
// public matching header.  Keep these declarations isolated and use NVIDIA's
// public naming/layout conventions so this file can be replaced by the SDK
// header when one becomes available.

#include <sl.h>

#include <cstdint>

namespace sl
{
	inline constexpr Feature kFeatureDLSS_NR = 1004;

	inline constexpr BufferType kBufferTypeUpliftInputColor = 70;
	inline constexpr BufferType kBufferTypeUpliftOutputColor = 71;
	inline constexpr BufferType kBufferTypeUpliftControlMask = 72;
	inline constexpr std::uint64_t kSDKVersionDLSSNRPreview =
		(std::uint64_t{ 2 } << 48) |
		(std::uint64_t{ 13 } << 32) |
		kSDKVersionMagic;

	enum class DLSSNRMode : std::uint32_t
	{
		eOff = 0,
		eOn = 1,
	};

	// {29DFDFE0-273A-4E72-B492-2DC823D5B1AD}
	SL_STRUCT_BEGIN(
		DLSSNROptions,
		StructType({ 0x29dfdfe0, 0x273a, 0x4e72, { 0xb4, 0x92, 0x2d, 0xc8, 0x23, 0xd5, 0xb1, 0xad } }),
		kStructVersion3)
		DLSSNRMode mode = DLSSNRMode::eOff;
		float intensity = 1.0f;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float globalToneStrength = 1.0f;

		// Version 2
		std::uint32_t style = 0;
		std::uint32_t preset = 0;
		Boolean useAutoMask = Boolean::eFalse;
		float skinStructureStrength = 1.0f;

		// Version 3
		std::uint32_t performanceMode = 3;
	SL_STRUCT_END()
}

using PFun_slDLSSNRSetOptions = sl::Result(
	const sl::ViewportHandle& viewport,
	const sl::DLSSNROptions& options);
