// White-box lifecycle tests against the production implementation. WARP is
// used only for a valid device and tiny intermediate textures; no NGX DLLs.
#include "nr_test_support.h"
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <dxgi1_6.h>
#include "../src/NRPassOptions.h"
#define private public
#include "../src/nvngx_dlss_nr_private.h"
#undef private
#include "../src/nvngx_dlss_nr_private.cpp"
#include "../src/TextureMemoryReserve.h"
#include <stdexcept>

using namespace nvngx::dlss_nr;
using Microsoft::WRL::ComPtr;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
static NVSDK_NGX_Result NVSDK_CONV ReleaseFake(NVSDK_NGX_Handle*) { return NVSDK_NGX_Result_Success; }
static NVSDK_NGX_Result NVSDK_CONV DestroyFake(NVSDK_NGX_Parameter*) { return NVSDK_NGX_Result_Success; }

int main()
{
	try {
		ComPtr<IDXGIFactory4> factory;
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<ID3D12Device> device;
		CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
		CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
		CHECK(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
		D3D12Backend backend;
		backend.device_ = device.Get();
		backend.device_->AddRef();
		backend.initialized_ = backend.initializationAttempted_ = true;
		backend.destroyParameters_ = DestroyFake;
		D3D12EvaluationParameters p;
		p.inputWidth = p.outputWidth = p.guideWidth = 32;
		p.inputHeight = p.outputHeight = p.guideHeight = 32;
		p.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		p.passCount = 2;
		p.separatePassSettings = true;
		p.options.preset = 1;
		p.additionalPassOptions[0].preset = 2;
		CHECK(backend.EnsureIntermediateResources(p));
		for (unsigned i = 0; i < 2; ++i) {
			auto& state = backend.features_[i];
			state.feature = reinterpret_cast<NVSDK_NGX_Handle*>(static_cast<uintptr_t>(i + 1));
			state.parameters = reinterpret_cast<NVSDK_NGX_Parameter*>(static_cast<uintptr_t>(i + 1));
			state.activeReleaseFeature = ReleaseFake;
			state.inputWidth = state.inputHeight = state.outputWidth = state.outputHeight = 32;
			state.performanceMode = kNativePerformanceMode;
			state.preset = p.ForPass(i).options.preset;
			state.memoryEstimate = TextureMemoryReserve::NRFeatureBytes(32, 32);
		}
		CHECK(!backend.NeedsFeaturePreparation(p));
		const auto retained = backend.GetRetainedMemoryEstimate();
		CHECK(retained > 2 * TextureMemoryReserve::NRFeatureBytes(32, 32));
		p.additionalPassOptions[0].intensity = 0.3f;
		CHECK(!backend.NeedsFeaturePreparation(p)); // strength does not recreate
		p.additionalPassOptions[0].preset = 3;
		CHECK(backend.NeedsFeaturePreparation(p));
		CHECK(!backend.NeedsFeatureRecreation(backend.features_[0], p.ForPass(0)));
		CHECK(backend.NeedsFeatureRecreation(backend.features_[1], p.ForPass(1)));
		auto& failed = backend.features_[1];
		for (unsigned i = 0; i < 7; ++i) {
			backend.LatchFeatureFailure(failed, p.ForPass(1), NVSDK_NGX_Result_Fail);
			CHECK(failed.retryable);
			CHECK(!backend.NeedsFeaturePreparation(p)); // cooldown blocks this preset
			failed.retryAfter = std::chrono::steady_clock::now() - std::chrono::seconds(1);
			CHECK(backend.NeedsFeaturePreparation(p)); // including >4 failures
		}
		CHECK(backend.GetRetainedMemoryEstimate() == retained); // failure isn't release
		backend.LatchFeatureFailure(failed, p.ForPass(1), NVSDK_NGX_Result_FAIL_InvalidParameter);
		CHECK(!failed.retryable && !backend.NeedsFeaturePreparation(p));
		p.additionalPassOptions[0].preset = 2;
		CHECK(backend.NeedsFeaturePreparation(p)); // changed failed-pass configuration
		CHECK(backend.ReleaseFeature());
		CHECK(backend.GetRetainedMemoryEstimate() == 0);
		std::puts("PASS: production per-pass recreation/failure keys, transient recovery and retained lifetime.");
		return 0;
	} catch (const std::exception& e) {
		std::printf("FAIL: %s\n", e.what());
		return 1;
	}
}
