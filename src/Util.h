#pragma once

#include <d3d11.h>

namespace Util
{
	struct CameraProjection
	{
		DirectX::XMMATRIX cameraViewToClip;
		float cameraFOV{ 0.0f };
		float fovA{ 0.0f };
		float fovB{ 0.0f };
		float playerCameraFOV{ 0.0f };
		bool usedAlternateProjectionOrder{ false };
		bool usedMatrixFOV{ false };
	};

	enum class RenderTarget
	{
		kFrameBuffer = 0,

		kRefractionNormal = 1,

		kMainPreAlpha = 2,
		kMain = 3,
		kMainTemp = 4,

		kSSRRaw = 7,
		kSSRBlurred = 8,
		kSSRBlurredExtra = 9,

		kMainVerticalBlur = 14,
		kMainHorizontalBlur = 15,

		kSSRDirection = 10,
		kSSRMask = 11,

		kUI = 17,
		kUITemp = 18,

		kGbufferNormal = 20,
		kGbufferNormalSwap = 21,
		kGbufferAlbedo = 22,
		kGbufferEmissive = 23,
		kGbufferMaterial = 24, //  Glossiness, Specular, Backlighting, SSS

		kSSAO = 28,

		kTAAAccumulation = 26,
		kTAAAccumulationSwap = 27,

		kMotionVectors = 29,

		kUIDownscaled = 36,
		kUIDownscaledComposite = 37,

		kMainDepthMips = 39,

		kUnkMask = 57,

		kSSAOTemp = 48,
		kSSAOTemp2 = 49,
		kSSAOTemp3 = 50,

		kDiffuseBuffer = 58,
		kSpecularBuffer = 59,

		kDownscaledHDR = 64,
		kDownscaledHDRLuminance2 = 65,
		kDownscaledHDRLuminance3 = 66,
		kDownscaledHDRLuminance4 = 67,
		kDownscaledHDRLuminance5Adaptation = 68,
		kDownscaledHDRLuminance6AdaptationSwap = 69,
		kDownscaledHDRLuminance6 = 70,

		kCount = 101
	};

	enum class DepthStencilTarget
	{
		kMainOtherOther = 0,
		kMainOther = 1,
		kMain = 2,
		kMainCopy = 3,
		kMainCopyCopy = 4,

		kShadowMap = 8,

		kCount = 13
	};

	[[nodiscard]] static RE::BSGraphics::State* State_GetSingleton()
	{
		REL::Relocation<RE::BSGraphics::State*> singleton{ REL::ID{ 600795, 2704621 } };
		return singleton.get();
	}

	[[nodiscard]] static RE::BSGraphics::RenderTargetManager* RenderTargetManager_GetSingleton()
	{
		REL::Relocation<RE::BSGraphics::RenderTargetManager*> singleton{ REL::ID{ 1508457, 2666735 } };
		return singleton.get();
	}

	DirectX::XMMATRIX ToXMMatrix(const __m128* a_matrix);
	float VerticalFOVFromProjection(const DirectX::XMMATRIX& a_projection);
	float VerticalFOVFromProjection(const __m128* a_projection);
	bool IsPlausibleVerticalFOV(float a_fov);
	float VerticalFOVFromHorizontalDegrees(float a_horizontalDegrees, float a_aspectRatio);
	float PlayerCameraVerticalFOV(float a_aspectRatio);
	const RE::BSGraphics::CameraStateData* GetWorldCameraStateData();
	CameraProjection GetCameraProjection(float a_aspectRatio);

	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program = "main");
}
