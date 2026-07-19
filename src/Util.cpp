#include "Util.h"

#include <cmath>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include "RE/M/Main.h"

namespace Util
{
	DirectX::XMMATRIX ToXMMatrix(const __m128* a_matrix)
	{
		return DirectX::XMMATRIX(a_matrix[0], a_matrix[1], a_matrix[2], a_matrix[3]);
	}

	bool TryGetVerticalFOVFromProjection(const DirectX::XMMATRIX& a_projection, float& a_verticalFOV)
	{
		DirectX::XMFLOAT4X4 projection{};
		DirectX::XMStoreFloat4x4(&projection, a_projection);

		for (std::size_t row = 0; row < 4; ++row) {
			for (std::size_t column = 0; column < 4; ++column) {
				if (!std::isfinite(projection.m[row][column])) {
					return false;
				}
			}
		}

		// Fallout 4 builds its perspective projection with positive X/Y scales,
		// M34 == 1, and M44 == 0. Validate that shape instead of rejecting narrow
		// but legitimate scope FOVs with an arbitrary angle threshold.
		constexpr auto perspectiveTolerance = 1.0e-5f;
		if (projection._11 <= 0.0f ||
			projection._22 <= 0.0f ||
			projection._33 <= 0.0f ||
			projection._43 >= 0.0f ||
			std::abs(projection._34 - 1.0f) > perspectiveTolerance ||
			std::abs(projection._44) > perspectiveTolerance) {
			return false;
		}

		constexpr auto pi = 3.14159265358979323846f;
		const auto verticalFOV = 2.0f * std::atan2(1.0f, projection._22);
		if (!std::isfinite(verticalFOV) || verticalFOV <= 0.0f || verticalFOV >= pi) {
			return false;
		}

		a_verticalFOV = verticalFOV;
		return true;
	}

	const RE::BSGraphics::CameraStateData* GetWorldCameraStateData()
	{
		auto* state = State_GetSingleton();
		const auto* worldCamera = RE::Main::WorldRootCamera();
		if (!state || !worldCamera) {
			return nullptr;
		}

		const RE::BSGraphics::CameraStateData* selected = nullptr;
		for (const auto& candidate : state->cameraDataCache) {
			if (candidate.referenceCamera == worldCamera &&
				(!selected || (selected->useJitter && !candidate.useJitter))) {
				selected = std::addressof(candidate);
			}
		}
		return selected;
	}

	CameraProjection GetCameraProjection()
	{
		CameraProjection result{};
		result.cameraState = GetWorldCameraStateData();
		if (!result.cameraState) {
			return result;
		}

		result.cameraViewToClip = ToXMMatrix(result.cameraState->camViewData.projMat);
		result.usedMatrixFOV = TryGetVerticalFOVFromProjection(result.cameraViewToClip, result.cameraFOV);
		return result;
	}

	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program)
	{
		static auto rendererData = RE::BSGraphics::GetRendererData();
		static auto device = reinterpret_cast<ID3D11Device*>(rendererData->device);

		// Build defines (aka convert vector->D3DCONSTANT array)
		std::vector<D3D_SHADER_MACRO> macros;

		for (auto& i : Defines)
			macros.push_back({ i.first, i.second });
		
		// Add null terminating entry
		macros.push_back({ nullptr, nullptr });

		// Compiler setup
		uint32_t flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

		winrt::com_ptr<ID3DBlob> shaderBlob;
		winrt::com_ptr<ID3DBlob> shaderErrors;

		std::string str;
		std::wstring path{ FilePath };
		std::transform(path.begin(), path.end(), std::back_inserter(str), [](wchar_t c) {
			return (char)c;
		});
		if (!std::filesystem::exists(FilePath)) {
			logger::error("Failed to compile shader; {} does not exist", str);
			return nullptr;
		}
		if (FAILED(D3DCompileFromFile(FilePath, macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, Program, ProgramType, flags, 0, shaderBlob.put(), shaderErrors.put()))) {
			logger::warn("Shader compilation failed:\n\n{}", shaderErrors ? static_cast<char*>(shaderErrors->GetBufferPointer()) : "Unknown error");
			return nullptr;
		}
		if (shaderErrors)
			logger::debug("Shader logs:\n{}", static_cast<char*>(shaderErrors->GetBufferPointer()));

		if (!_stricmp(ProgramType, "ps_5_0")) {
			ID3D11PixelShader* regShader;
			device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
			return regShader;
		}
		else if (!_stricmp(ProgramType, "vs_5_0")) {
			ID3D11VertexShader* regShader;
			device->CreateVertexShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
			return regShader;
		}
		else if (!_stricmp(ProgramType, "hs_5_0")) {
			ID3D11HullShader* regShader;
			device->CreateHullShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
			return regShader;
		}
		else if (!_stricmp(ProgramType, "ds_5_0")) {
			ID3D11DomainShader* regShader;
			device->CreateDomainShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
			return regShader;
		}
		else if (!_stricmp(ProgramType, "cs_5_0")) {
			ID3D11ComputeShader* regShader;
			DX::ThrowIfFailed(device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader));
			return regShader;
		}
		else if (!_stricmp(ProgramType, "cs_4_0")) {
			ID3D11ComputeShader* regShader;
			DX::ThrowIfFailed(device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader));
			return regShader;
		}

		return nullptr;
	}
}
