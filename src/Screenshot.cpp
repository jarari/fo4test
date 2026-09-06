#include "Screenshot.h"

#include <array>
#include <cstring>
#include <memory>

#include "DX12SwapChain.h"
#include "Util.h"

namespace
{
	thread_local std::shared_ptr<D3D11D3D12SharedTexture> pendingImage;

	struct ScreenshotCopy
	{
		static void thunk(int a_source, int a_destination)
		{
			pendingImage.reset();
			if (a_source == 0 && a_destination == 63) {
				pendingImage = DX12SwapChain::GetSingleton()->CaptureScreenshot();
			}
			if (!pendingImage) {
				func(a_source, a_destination);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ScreenshotSave
	{
		static void thunk(const char* a_filename, uint32_t a_format, int a_target)
		{
			auto image = std::move(pendingImage);
			if (!image) {
				func(a_filename, a_format, a_target);
				return;
			}
			auto* manager = Util::RenderTargetManager_GetSingleton();
			auto* renderer = RE::BSGraphics::GetRendererData();
			const auto mappingOffset = REX::FModule::IsRuntimeOG() ? 0xDC4u : 0xDF4u;
			const auto* mapping = manager ? reinterpret_cast<const uint32_t*>(
				reinterpret_cast<const std::byte*>(manager) + mappingOffset) : nullptr;
			if (a_target != 63 || !renderer || !mapping || mapping[63] >= std::size(renderer->renderTargets)) {
				// Capture succeeded and skipped the copy; reconstruct the original
				// scratch result if the save arguments no longer match this call site.
				ScreenshotCopy::func(0, 63);
				func(a_filename, a_format, a_target);
				return;
			}
			auto& target = renderer->renderTargets[mapping[63]];
			struct RestoreTexture
			{
				RE::BSGraphics::RenderTarget& target;
				REX::W32::ID3D11Texture2D* original;
				~RestoreTexture() { target.texture = original; }
			} restore{ target, target.texture };
			// Both file and clipboard paths read only this texture member, then
			// synchronously encode it. Leave RT63's shared model views/metadata intact.
			target.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(image->resource11.get());
			func(a_filename, a_format, a_target);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool Calls(std::uintptr_t a_site, std::uintptr_t a_target)
	{
		const auto* instruction = reinterpret_cast<const uint8_t*>(a_site);
		int32_t relative = 0;
		std::memcpy(&relative, instruction + 1, sizeof(relative));
		return instruction[0] == 0xE8 &&
			static_cast<std::uintptr_t>(static_cast<std::intptr_t>(a_site + 5) + relative) == a_target;
	}
}

void Screenshot::InstallHooks()
{
	const bool isOG = REX::FModule::IsRuntimeOG();
	const auto swap = REL::ID{ 1075087, 2228913 }.address();
	const auto copy = swap + (isOG ? 0x2D3 : 0x2B6);
	const auto save = copy + 0x10;
	constexpr std::array<uint8_t, 7> copyArguments{ 0xBA, 0x3F, 0, 0, 0, 0x33, 0xC9 };
	constexpr std::array<uint8_t, 11> saveArguments{ 0xBA, 3, 0, 0, 0, 0x33, 0xC9, 0x44, 0x8D, 0x42, 0x3C };
	// Validate both calls before installing either. Scope these hooks to the
	// keyboard screenshot branch; save-game thumbnails and model copies stay native.
	if (!Calls(copy, REL::ID{ 1386071, 2316602 }.address()) ||
		!Calls(save, REL::ID{ 919230, 2229158 }.address()) ||
		std::memcmp(reinterpret_cast<const void*>(copy - copyArguments.size()), copyArguments.data(), copyArguments.size()) != 0 ||
		std::memcmp(reinterpret_cast<const void*>(copy + 5), saveArguments.data(), saveArguments.size()) != 0) {
		logger::error("[Screenshot] Main::Swap capture calls did not match; hooks not installed");
		return;
	}
	stl::write_thunk_call<ScreenshotCopy>(copy);
	stl::write_thunk_call<ScreenshotSave>(save);
	logger::info("[Screenshot] Installed same-frame world/UI capture for {}", isOG ? "OG" : "AE");
}
