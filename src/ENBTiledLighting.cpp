#include "ENBTiledLighting.h"

namespace
{
	constexpr uint64_t TileCount(uint32_t a_width, uint32_t a_height)
	{
		return ((uint64_t{ a_width } + 7) / 8) * ((uint64_t{ a_height } + 7) / 8);
	}
	static_assert(TileCount(2293, 960) == 34440);
	static_assert(TileCount(3440, 1440) == 77400);
	static_assert(TileCount(1, 1) == 1);
}

HRESULT ENBTiledLightingResize::Prepare(ID3D11Device* a_device, uint32_t a_width, uint32_t a_height)
{
	const auto count = TileCount(a_width, a_height);
	if (!a_device || !a_width || !a_height || count > UINT_MAX / 512u) {
		return E_INVALIDARG;
	}
	tileCount = static_cast<uint32_t>(count);
	// InitSDM: OG 0x286DC90, AE 0x2207200. These globals are separate
	// from RenderTargetManager, so destroyTargets/createTargets never resize them.
	static REL::Relocation<EngineBuffer**> indices{ REL::ID{ 928298, 2713005 } };
	static REL::Relocation<EngineBuffer**> depth{ REL::ID{ 1147648, 2713006 } };
	const std::array targets{ *indices.get(), *depth.get() };
	if (!targets[0] && !targets[1]) {
		// Tiled lighting was disabled during engine initialization.
		return S_OK;
	}
	constexpr std::array strides{ 512u, 8u };
	for (size_t i = 0; i < targets.size(); ++i) {
		auto* target = targets[i];
		if (!target || target->pendingRequests || target->references != 1 ||
			!target->buffer || !target->srv || !target->uav || target->staging || target->mappedData) {
			logger::error("[ENB tiled lighting] Buffer {} is unavailable or still owned by a resource request", i);
			return E_UNEXPECTED;
		}
		D3D11_BUFFER_DESC desc{};
		target->buffer->GetDesc(&desc);
		if (desc.StructureByteStride != strides[i] ||
			desc.ByteWidth != uint64_t{ target->count } * strides[i] ||
			desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0 ||
			desc.BindFlags != (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS) ||
			desc.MiscFlags != D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
			logger::error("[ENB tiled lighting] Unexpected buffer {} layout; resize cancelled", i);
			return E_UNEXPECTED;
		}
		// Retain sufficient capacity on downward transitions. Dispatch dimensions
		// are recomputed by DeferredLightsImpl every frame; capacity is independent.
		if (target->count >= tileCount) {
			continue;
		}
		auto& replacement = replacements[i];
		desc.ByteWidth = tileCount * strides[i];
		auto result = a_device->CreateBuffer(&desc, nullptr, replacement.buffer.put());
		if (SUCCEEDED(result)) {
			result = a_device->CreateShaderResourceView(replacement.buffer.get(), nullptr, replacement.srv.put());
		}
		if (SUCCEEDED(result)) {
			result = a_device->CreateUnorderedAccessView(replacement.buffer.get(), nullptr, replacement.uav.put());
		}
		if (FAILED(result)) {
			logger::error("[ENB tiled lighting] Buffer {} allocation failed hr=0x{:08X}; retaining active buffers", i, static_cast<uint32_t>(result));
			return result;
		}
		replacement.target = target;
	}
	return S_OK;
}

void ENBTiledLightingResize::Commit()
{
	for (size_t i = 0; i < replacements.size(); ++i) {
		auto& replacement = replacements[i];
		auto* target = replacement.target;
		if (!target) {
			continue;
		}
		const auto oldCount = target->count;
		// Transfer the engine's old COM ownership to this transaction, retaining
		// it until the caller's post-rebuild GPU drain. Engine shutdown still owns
		// and releases the replacement through the same StructuredBuffer object.
		winrt::com_ptr<ID3D11Buffer> oldBuffer;
		winrt::com_ptr<ID3D11ShaderResourceView> oldSRV;
		winrt::com_ptr<ID3D11UnorderedAccessView> oldUAV;
		oldBuffer.attach(target->buffer);
		oldSRV.attach(target->srv);
		oldUAV.attach(target->uav);
		target->buffer = replacement.buffer.detach();
		target->srv = replacement.srv.detach();
		target->uav = replacement.uav.detach();
		target->count = tileCount;
		replacement.buffer = std::move(oldBuffer);
		replacement.srv = std::move(oldSRV);
		replacement.uav = std::move(oldUAV);
		replacement.target = nullptr;
		logger::info("[ENB tiled lighting] {} capacity {} -> {} tiles (8x8)", i == 0 ? "light indices" : "depth bounds", oldCount, tileCount);
	}
}
