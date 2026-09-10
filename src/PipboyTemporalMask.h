#pragma once

#include <d3d11.h>
#include <d3d12.h>

namespace PipboyTemporalMask
{
// Snapshot before imagespace/Interface3D can reuse the world material target.
void Capture();
ID3D11Texture2D* Current(UINT width, UINT height);
void Release();

// Both resources enter/leave COMMON and UAV respectively. The caller owns
// frame-slot reuse fences. Preserve an existing FSR reactive mask with max().
bool MergeReactive(ID3D12Device* device, ID3D12GraphicsCommandList* commands,
    ID3D12Resource* mask, ID3D12Resource* reactive, UINT slot, bool hasReactive);
}
