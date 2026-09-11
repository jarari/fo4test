#pragma once

#include <Windows.h>
#include <cstdint>

struct ID3D12Device;

namespace RTX40MFGUnlock
{
	// Scans already-loaded Streamline/DLSS-G modules and applies the fail-closed
	// wrapper, NGX, and pre-CreateFeature provider patches adapted from
	// RTX40MFG-Unlock. Provider entry observers are only a fallback for an
	// ambiguous multi-provider process.
	bool PatchLoadedModules() noexcept;

	// Supplies the active D3D12 adapter identity required by the Ada midpoint
	// correction, then retries provider discovery and publication.
	bool ObserveD3D12Device(ID3D12Device* a_device) noexcept;

	bool AdaAdapterVerified() noexcept;
	bool Ready() noexcept;
	void InstallLoaderDiscovery(HMODULE a_module) noexcept;
	void InspectLoadedModule(HMODULE a_module, bool a_tryEarlyProviderPatch = false) noexcept;
	void ObserveWrapper(const void* a_function) noexcept;
	std::uint32_t MaximumGeneratedFrames() noexcept;
}
