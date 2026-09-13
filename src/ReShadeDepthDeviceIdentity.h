#pragma once
#include <atomic>
#include <d3d12.h>

namespace ReShadeDepth
{
	// Our own D3D private-data key, not a ReShade/Streamline private interface.
	// Wrappers forward Get/SetPrivateData to the same underlying device even when
	// Resource::GetDevice returns a proxy and api::device::get_native does not.
	inline constexpr GUID deviceIdentityKey{
		0x88bc8ef2, 0x602c, 0x4532, { 0x8d, 0xa1, 0x13, 0x99, 0x40, 0x9e, 0x71, 0x52 }
	};
	inline HRESULT DeviceIdentity(ID3D12Object* device, uint64_t& identity)
	{
		identity = 0;
		if (!device) return E_POINTER;
		UINT size = sizeof(identity);
		const HRESULT read = device->GetPrivateData(deviceIdentityKey, &size, &identity);
		if (SUCCEEDED(read) && size == sizeof(identity) && identity) return S_OK;
		if (FAILED(read) && read != DXGI_ERROR_NOT_FOUND) return read;
		static std::atomic_uint64_t next{ 0 };
		identity = ++next;
		const HRESULT write = device->SetPrivateData(deviceIdentityKey, sizeof(identity), &identity);
		if (FAILED(write)) identity = 0;
		return write;
	}
}
