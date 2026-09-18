#pragma once
#include <dxgi1_6.h>
#include <optional>

namespace SwapChainCreationObserver
{
	// Called after successful registration of our ReShade add-on.
	void Initialize();
	// Does not retain the observed chain: retry must be able to destroy it.
	class Scope
	{
	public:
		explicit Scope(HWND window);
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
		std::optional<DXGI_SWAP_CHAIN_DESC1> Result() const;
	};
}
