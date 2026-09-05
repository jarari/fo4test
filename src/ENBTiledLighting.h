#pragma once

#include <array>
#include <d3d11.h>
#include <winrt/base.h>

// A render-thread transaction: prepare before the scene changes, commit only
// after a successful resize, with the GPU idle and the context unbound.
class ENBTiledLightingResize
{
public:
	HRESULT Prepare(ID3D11Device* a_device, uint32_t a_width, uint32_t a_height);
	void Commit();

private:
	// BSGraphics::StructuredBuffer, verified in OG/AE resource creation and
	// CSSetStructuredBufferSR/UAV. Keep the engine's arena object and refcount.
	struct EngineBuffer
	{
		ID3D11Buffer* buffer;                       // 00
		ID3D11ShaderResourceView* srv;               // 08
		ID3D11UnorderedAccessView* uav;              // 10
		ID3D11Buffer* staging;                      // 18
		void* mappedData;                           // 20
		HANDLE* creationEvent;                      // 28
		volatile LONG pendingRequests;             // 30
		uint32_t count;                             // 34
		volatile LONG references;                  // 38
	};
	static_assert(offsetof(EngineBuffer, count) == 0x34);
	static_assert(offsetof(EngineBuffer, references) == 0x38);
	static_assert(sizeof(EngineBuffer) == 0x40);

	struct Replacement
	{
		EngineBuffer* target = nullptr;
		winrt::com_ptr<ID3D11Buffer> buffer;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
	};
	std::array<Replacement, 2> replacements{};
	uint32_t tileCount = 0;
};
