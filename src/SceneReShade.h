#pragma once
#include <d3d11.h>

namespace SceneReShade
{
	void Initialize();
	void Reset();
	void Render(ID3D11Texture2D* color, ID3D11ShaderResourceView* depth, HWND window, UINT width, UINT height);
}
