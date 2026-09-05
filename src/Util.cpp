#include "Util.h"

#include <cmath>
#include <array>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include "RE/M/Main.h"

namespace Util
{
	namespace
	{
		constexpr uint32_t kUnmappedTarget = UINT32_MAX;
		std::array<uint32_t, 101> initialColorIdentity;
		std::array<uint32_t, 13> initialDepthIdentity;
		bool identitiesCaptured = false;

		const uint32_t* TargetIDTable(bool a_depth)
		{
			const auto offset = a_depth ? (REX::FModule::IsRuntimeOG() ? 0xF54u : 0xF84u) :
				(REX::FModule::IsRuntimeOG() ? 0xDC4u : 0xDF4u);
			return reinterpret_cast<const uint32_t*>(reinterpret_cast<const std::byte*>(RenderTargetManager_GetSingleton()) + offset);
		}
	}

	bool CaptureInitialRenderTargetBindings()
	{
		if (identitiesCaptured) {
			return true;
		}
		initialColorIdentity.fill(kUnmappedTarget);
		initialDepthIdentity.fill(kUnmappedTarget);
		const auto* colors = TargetIDTable(false);
		const auto* depths = TargetIDTable(true);
		for (uint32_t logical = 0; logical < (REX::FModule::IsRuntimeOG() ? 100u : 103u); ++logical) {
			if (colors[logical] < initialColorIdentity.size() && initialColorIdentity[colors[logical]] == kUnmappedTarget) {
				initialColorIdentity[colors[logical]] = logical;
			}
		}
		for (uint32_t logical = 0; logical < 12; ++logical) {
			if (depths[logical] < initialDepthIdentity.size() && initialDepthIdentity[depths[logical]] == kUnmappedTarget) {
				initialDepthIdentity[depths[logical]] = logical;
			}
		}
		for (auto target : { RenderTarget::kMain, RenderTarget::kMainTemp, RenderTarget::kMotionVectors, RenderTarget::kMainDepthMips }) {
			if (initialColorIdentity[static_cast<uint32_t>(target)] == kUnmappedTarget) {
				return false;
			}
		}
		if (initialDepthIdentity[static_cast<uint32_t>(DepthStencilTarget::kMain)] == kUnmappedTarget) {
			return false;
		}
		identitiesCaptured = true;
		logger::info("[ENB targets] Captured initial identities: motion logical={}, depth logical={}, linear-depth logical={}",
			initialColorIdentity[static_cast<uint32_t>(RenderTarget::kMotionVectors)],
			initialDepthIdentity[static_cast<uint32_t>(DepthStencilTarget::kMain)],
			initialColorIdentity[static_cast<uint32_t>(RenderTarget::kMainDepthMips)]);
		return true;
	}

	uint32_t ResolveRenderTarget(RenderTarget a_target)
	{
		const auto legacy = static_cast<uint32_t>(a_target);
		if (!identitiesCaptured || legacy == 0 || legacy >= initialColorIdentity.size() || initialColorIdentity[legacy] == kUnmappedTarget) {
			return legacy;
		}
		const auto physical = TargetIDTable(false)[initialColorIdentity[legacy]];
		return physical < std::size(RE::BSGraphics::GetRendererData()->renderTargets) ? physical : 0;
	}

	uint32_t ResolveDepthStencilTarget(DepthStencilTarget a_target)
	{
		const auto legacy = static_cast<uint32_t>(a_target);
		if (!identitiesCaptured || legacy >= initialDepthIdentity.size() || initialDepthIdentity[legacy] == kUnmappedTarget) {
			return legacy;
		}
		const auto physical = TargetIDTable(true)[initialDepthIdentity[legacy]];
		return physical < std::size(RE::BSGraphics::GetRendererData()->depthStencilTargets) ? physical : 0;
	}

	namespace
	{
		bool Normalize(DirectX::XMFLOAT3& a_vector)
		{
			const auto lengthSq =
				a_vector.x * a_vector.x +
				a_vector.y * a_vector.y +
				a_vector.z * a_vector.z;
			if (!std::isfinite(lengthSq) || lengthSq <= 0.0f) {
				return false;
			}

			const auto invLength = 1.0f / std::sqrt(lengthSq);
			a_vector.x *= invLength;
			a_vector.y *= invLength;
			a_vector.z *= invLength;
			return true;
		}

		float AlignmentScore(const CameraBasis& a_basis, const RE::BSGraphics::ViewData& a_viewData)
		{
			const auto alignment = [](const DirectX::XMFLOAT3& a_vector, const __m128& a_reference) {
				return std::abs(
					a_vector.x * a_reference.m128_f32[0] +
					a_vector.y * a_reference.m128_f32[1] +
					a_vector.z * a_reference.m128_f32[2]);
			};

			return
				alignment(a_basis.right, a_viewData.viewRight) +
				alignment(a_basis.up, a_viewData.viewUp) +
				alignment(a_basis.forward, a_viewData.viewDir);
		}

		bool Orthonormalize(CameraBasis& a_basis)
		{
			if (!Normalize(a_basis.right) || !Normalize(a_basis.forward)) {
				return false;
			}

			// Match Streamline's left-handed camera basis convention.
			a_basis.up = {
				a_basis.forward.y * a_basis.right.z - a_basis.forward.z * a_basis.right.y,
				a_basis.forward.z * a_basis.right.x - a_basis.forward.x * a_basis.right.z,
				a_basis.forward.x * a_basis.right.y - a_basis.forward.y * a_basis.right.x
			};
			if (!Normalize(a_basis.up)) {
				return false;
			}

			a_basis.right = {
				a_basis.up.y * a_basis.forward.z - a_basis.up.z * a_basis.forward.y,
				a_basis.up.z * a_basis.forward.x - a_basis.up.x * a_basis.forward.z,
				a_basis.up.x * a_basis.forward.y - a_basis.up.y * a_basis.forward.x
			};
			return Normalize(a_basis.right);
		}

		bool TrySetCameraProjection(CameraProjection& a_result, const DirectX::XMMATRIX& a_projection)
		{
			float verticalFOV = 0.0f;
			if (!TryGetVerticalFOVFromProjection(a_projection, verticalFOV)) {
				return false;
			}

			DirectX::XMFLOAT4X4 projection{};
			DirectX::XMStoreFloat4x4(&projection, a_projection);
			const auto aspectRatio = projection._22 / projection._11;
			if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f) {
				return false;
			}

			a_result.cameraViewToClip = a_projection;
			a_result.cameraFOV = verticalFOV;
			a_result.cameraAspectRatio = aspectRatio;
			a_result.usedMatrixFOV = true;
			return true;
		}
	}

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

		const auto& viewData = result.cameraState->camViewData;
		const auto view = ToXMMatrix(viewData.viewMat);
		const auto viewProjUnjittered = ToXMMatrix(viewData.viewProjUnjittered);
		DirectX::XMVECTOR determinant{};
		const auto cameraToWorld = DirectX::XMMatrixInverse(&determinant, view);
		const auto det = DirectX::XMVectorGetX(determinant);
		if (std::isfinite(det) && det != 0.0f) {
			// Fallout stores row-major matrices, but test both multiplication
			// orders so this remains correct if a runtime exposes transposed data.
			const auto rowMajorProjection = DirectX::XMMatrixMultiply(cameraToWorld, viewProjUnjittered);
			const auto transposedProjection = DirectX::XMMatrixMultiply(viewProjUnjittered, cameraToWorld);
			if (TrySetCameraProjection(result, rowMajorProjection) ||
				TrySetCameraProjection(result, transposedProjection)) {
				return result;
			}
		}

		// A non-jittered camera-cache entry can safely provide its projection
		// directly. Never fall back to projMat on a jittered entry because
		// Streamline receives the pixel jitter separately.
		if (!result.cameraState->useJitter) {
			TrySetCameraProjection(result, ToXMMatrix(viewData.projMat));
		}
		return result;
	}

	bool TryGetCameraBasis(const RE::BSGraphics::ViewData& a_viewData, CameraBasis& a_basis)
	{
		const auto view = ToXMMatrix(a_viewData.viewMat);
		DirectX::XMVECTOR determinant{};
		const auto cameraToWorld = DirectX::XMMatrixInverse(&determinant, view);
		const auto det = DirectX::XMVectorGetX(determinant);
		if (std::isfinite(det) && det != 0.0f) {
			DirectX::XMFLOAT4X4 matrix{};
			DirectX::XMStoreFloat4x4(&matrix, cameraToWorld);

			CameraBasis rows{
				{ matrix._11, matrix._12, matrix._13 },
				{ matrix._21, matrix._22, matrix._23 },
				{ matrix._31, matrix._32, matrix._33 }
			};
			CameraBasis columns{
				{ matrix._11, matrix._21, matrix._31 },
				{ matrix._12, matrix._22, matrix._32 },
				{ matrix._13, matrix._23, matrix._33 }
			};
			const auto rowsValid = Orthonormalize(rows);
			const auto columnsValid = Orthonormalize(columns);
			if (rowsValid || columnsValid) {
				a_basis =
					columnsValid && (!rowsValid || AlignmentScore(columns, a_viewData) > AlignmentScore(rows, a_viewData)) ?
					columns :
					rows;
				return true;
			}
		}

		a_basis = {
			{ a_viewData.viewRight.m128_f32[0], a_viewData.viewRight.m128_f32[1], a_viewData.viewRight.m128_f32[2] },
			{ a_viewData.viewUp.m128_f32[0], a_viewData.viewUp.m128_f32[1], a_viewData.viewUp.m128_f32[2] },
			{ a_viewData.viewDir.m128_f32[0], a_viewData.viewDir.m128_f32[1], a_viewData.viewDir.m128_f32[2] }
		};
		return Orthonormalize(a_basis);
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
