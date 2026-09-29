#include "XeSS.h"
#include "ColorRange.h"
#include "DX12SwapChain.h"
#include "XeSSCompat/XeFGUnlock.h"
#include "XeSSCompat/XeLLUnLock.h"
#include <filesystem>

namespace
{
    // Load from this plugin's directory, never from a game's unrelated XeSS installation.
    // Keep mapped for process lifetime: compatibility detours retain code addresses.
    HMODULE LoadRuntime(const wchar_t* file)
    {
        HMODULE host = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&LoadRuntime), &host)) return nullptr;
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(host, path, static_cast<DWORD>(std::size(path)))) return nullptr;
        const auto dll = std::filesystem::path(path).parent_path() / file;
        auto module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) logger::warn("[XeSS] Cannot load {}: {}", dll.string(), GetLastError());
        return module;
    }
#define XESS_FUNCTION(name) decltype(&name) p_##name = nullptr
    XESS_FUNCTION(xessD3D12CreateContext); XESS_FUNCTION(xessD3D12Init); XESS_FUNCTION(xessD3D12Execute);
    XESS_FUNCTION(xessDestroyContext); XESS_FUNCTION(xessSetVelocityScale); XESS_FUNCTION(xessForceLegacyScaleFactors);
    XESS_FUNCTION(xefgSwapChainD3D12CreateContext); XESS_FUNCTION(xefgSwapChainD3D12InitFromSwapChainDesc);
    XESS_FUNCTION(xefgSwapChainD3D12GetSwapChainPtr); XESS_FUNCTION(xefgSwapChainGetProperties);
    XESS_FUNCTION(xefgSwapChainSetLatencyReduction); XESS_FUNCTION(xefgSwapChainSetEnabled);
    XESS_FUNCTION(xefgSwapChainSetPresentId); XESS_FUNCTION(xefgSwapChainSetNumInterpolatedFrames);
    XESS_FUNCTION(xefgSwapChainTagFrameConstants); XESS_FUNCTION(xefgSwapChainD3D12TagFrameResource);
    XESS_FUNCTION(xefgSwapChainGetLastPresentStatus); XESS_FUNCTION(xefgSwapChainDestroy);
    XESS_FUNCTION(xefgSwapChainSetUiCompositionState);
    XESS_FUNCTION(xellD3D12CreateContext); XESS_FUNCTION(xellDestroyContext); XESS_FUNCTION(xellSleep);
    XESS_FUNCTION(xellSetSleepMode); XESS_FUNCTION(xellAddMarkerData); XESS_FUNCTION(xellGetFramesReports);
#undef XESS_FUNCTION
#define XESS_LOAD(name) p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(module, #name)); if (!p_##name) return false
    bool LoadSR()
    {
        static std::mutex loadMutex;
        static HMODULE module = nullptr;
        static bool ready = false;
        std::scoped_lock lock(loadMutex);
        if (ready) return true;
        // Keep a successfully loaded module once. Only successful export
        // resolution is cached, so a missing DLL can be retried after install.
        if (!module) module = LoadRuntime(L"libxess.dll");
        if (!module) return false;
#define XESS_LOAD_SR(name) p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(module, #name)); if (!p_##name) { logger::warn("[XeSS SR] Missing export {}", #name); return false; }
        XESS_LOAD_SR(xessD3D12CreateContext); XESS_LOAD_SR(xessD3D12Init); XESS_LOAD_SR(xessD3D12Execute);
        XESS_LOAD_SR(xessDestroyContext); XESS_LOAD_SR(xessSetVelocityScale); XESS_LOAD_SR(xessForceLegacyScaleFactors);
#undef XESS_LOAD_SR
        ready = true;
        return ready;
    }
    bool LoadFG()
    {
        static const bool ready = [] {
            auto module = LoadRuntime(L"libxell.dll"); if (!module) return false;
            XESS_LOAD(xellD3D12CreateContext); XESS_LOAD(xellDestroyContext); XESS_LOAD(xellSleep);
            XESS_LOAD(xellSetSleepMode); XESS_LOAD(xellAddMarkerData);
            p_xellGetFramesReports = reinterpret_cast<decltype(p_xellGetFramesReports)>(GetProcAddress(module, "xellGetFramesReports"));
            XeLLUnlock::Apply(module);
            module = LoadRuntime(L"libxess_fg.dll"); if (!module) return false;
            XESS_LOAD(xefgSwapChainD3D12CreateContext); XESS_LOAD(xefgSwapChainD3D12InitFromSwapChainDesc);
            XESS_LOAD(xefgSwapChainD3D12GetSwapChainPtr); XESS_LOAD(xefgSwapChainGetProperties);
            XESS_LOAD(xefgSwapChainSetLatencyReduction); XESS_LOAD(xefgSwapChainSetEnabled);
            XESS_LOAD(xefgSwapChainSetPresentId); XESS_LOAD(xefgSwapChainSetNumInterpolatedFrames);
            XESS_LOAD(xefgSwapChainTagFrameConstants); XESS_LOAD(xefgSwapChainD3D12TagFrameResource);
            XESS_LOAD(xefgSwapChainGetLastPresentStatus); XESS_LOAD(xefgSwapChainDestroy);
            XESS_LOAD(xefgSwapChainSetUiCompositionState);
            XeFGUnlock::Apply(module); // Strict layout + byte checks protect all internal pacing offsets.
            return true;
        }();
        return ready;
    }
#undef XESS_LOAD
    bool FGResult(xefg_swapchain_result_t result, const char* operation)
    {
        if (result >= 0) return true;
        logger::warn("[XeSS FG] {} failed: {}", operation, static_cast<int>(result));
        return false;
    }

    bool SRRequiresConfigurationChange(xess_result_t result)
    {
        switch (result) {
        case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
        case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:
        case XESS_RESULT_ERROR_INVALID_ARGUMENT:
        case XESS_RESULT_ERROR_NOT_IMPLEMENTED:
        case XESS_RESULT_ERROR_UNSUPPORTED:
        case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:
            return true;
        default:
            return false;
        }
    }
}

void XeSS::DestroySR()
{
    if (sr) p_xessDestroyContext(sr);
    sr = nullptr; srDevice = nullptr; srWidth = srHeight = 0; srReset = 0;
}

bool XeSS::PrepareSR(ID3D12Device* device, UINT width, UINT height, uint quality, DXGI_FORMAT format)
{
    srFailureRequiresConfigurationChange = false;
    if (!device || !width || !height) return false;
    if (!LoadSR()) { srFailureRequiresConfigurationChange = true; return false; }
    quality = std::min(quality, 4u);
    if (sr && srDevice == device && srWidth == width && srHeight == height && srQuality == quality && srFormat == format) return true;
    if (sr && !DX12SwapChain::GetSingleton()->WaitForInteropIdle()) return false;
    DestroySR();
    const auto createResult = p_xessD3D12CreateContext(device, &sr);
    if (createResult < 0 || !sr) {
        srFailureRequiresConfigurationChange = SRRequiresConfigurationChange(createResult);
        logger::warn("[XeSS SR] CreateContext failed: {}", static_cast<int>(createResult));
        DestroySR();
        return false;
    }
    constexpr xess_quality_settings_t qualities[] = { XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_QUALITY,
        XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE };
    // Keep the existing engine's 1/1.5/1.7/2/3 render-resolution contract.
    const auto scaleResult = p_xessForceLegacyScaleFactors(sr, true);
    if (scaleResult < 0) {
        srFailureRequiresConfigurationChange = SRRequiresConfigurationChange(scaleResult);
        logger::warn("[XeSS SR] ForceLegacyScaleFactors failed: {}", static_cast<int>(scaleResult));
        DestroySR();
        return false;
    }
    xess_d3d12_init_params_t init{};
    init.outputResolution = { width, height };
    init.qualitySetting = qualities[quality];
    init.initFlags = ColorRange::IsExtended(format) ? XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : XESS_INIT_FLAG_LDR_INPUT_COLOR;
    const auto result = p_xessD3D12Init(sr, &init);
    if (result < 0) {
        srFailureRequiresConfigurationChange = SRRequiresConfigurationChange(result);
        logger::warn("[XeSS SR] Init failed: {}", static_cast<int>(result));
        DestroySR();
        return false;
    }
    srDevice = device; srWidth = width; srHeight = height; srQuality = quality; srFormat = format;
    logger::info("[XeSS SR] Initialized {}x{} quality={} hdr={}", width, height, quality, ColorRange::IsExtended(format));
    return true;
}

bool XeSS::Upscale(ID3D12GraphicsCommandList* commands, ID3D12Resource* color, ID3D12Resource* output,
    ID3D12Resource* motion, ID3D12Resource* depth, float2 jitter, float2 renderSize)
{
    if (!sr || !commands || !color || !output || !motion || !depth) return false;
    p_xessSetVelocityScale(sr, renderSize.x, renderSize.y);
    xess_d3d12_execute_params_t exec{};
    exec.pColorTexture = color; exec.pOutputTexture = output; exec.pVelocityTexture = motion; exec.pDepthTexture = depth;
    exec.inputWidth = static_cast<uint32_t>(renderSize.x); exec.inputHeight = static_cast<uint32_t>(renderSize.y);
    exec.jitterOffsetX = -jitter.x; exec.jitterOffsetY = -jitter.y; // Same projection-pixel convention as NGX: offsetX=-2*jx/w, offsetY=+2*jy/h.
    exec.exposureScale = 1.0f;
    const auto serial = resetSerial.load(std::memory_order_relaxed);
    exec.resetHistory = srReset != serial;
    const auto result = p_xessD3D12Execute(sr, commands, &exec);
    if (result < 0) { logger::warn("[XeSS SR] Execute failed: {}", static_cast<int>(result)); return false; }
    srReset = serial;
    return true;
}

bool XeSS::CreateSwapChain(ID3D12Device* device, IDXGIFactory2* factory, ID3D12CommandQueue* queue,
    HWND window, const DXGI_SWAP_CHAIN_DESC1& desc, IDXGISwapChain4** chain)
{
    if (!chain || !device || !factory || !queue || fg) return false;
    // SDK 3.0.2 supports SDR and HDR10, not FP16/scRGB. Do not silently
    // truncate RenoDX output or advertise a non-functional FG provider.
    if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        failureReason = "XeSS FG does not support FP16/scRGB output.";
        logger::warn("[XeSS FG] {}", failureReason);
        return false;
    }
    failureReason = "XeSS FG initialization failed (SDK/hardware unavailable). See Upscaling.log.";
    if (!LoadFG()) return false;
    *chain = nullptr;
    if (p_xellD3D12CreateContext(device, &ll) < 0) return false;
    if (!FGResult(p_xefgSwapChainD3D12CreateContext(device, &fg), "CreateContext")) { DestroyFG(); return false; }
    xefg_swapchain_properties_t properties{};
    if (!FGResult(p_xefgSwapChainGetProperties(fg, &properties), "GetProperties")) { DestroyFG(); return false; }
    maxGenerated = std::clamp(properties.maxSupportedInterpolations, 1u, 7u);
    // Never advertise >4x if XeLL's independent count validation was not patched.
    if (!XeLLUnlock::Applied()) maxGenerated = std::min(maxGenerated, 3u);
    // On the patched generic provider, MFG also needs working per-frame pacing.
    if (XeFGUnlock::Applied() && !XeFGPacing::g_enabled) maxGenerated = 1;
    xell_sleep_params_t sleep{}; sleep.bLowLatencyMode = 1;
    if (p_xellSetSleepMode(ll, &sleep) < 0 || !FGResult(p_xefgSwapChainSetLatencyReduction(fg, ll), "SetLatencyReduction")) {
        DestroyFG(); return false;
    }
    xefg_swapchain_d3d12_init_params_t init{};
    init.maxInterpolatedFrames = maxGenerated;
    init.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;
    if (!FGResult(p_xefgSwapChainD3D12InitFromSwapChainDesc(fg, window, &desc, nullptr, queue, factory, &init), "InitSwapChain") ||
        !FGResult(p_xefgSwapChainD3D12GetSwapChainPtr(fg, IID_PPV_ARGS(chain)), "GetSwapChain")) { DestroyFG(); return false; }
    fgInitialized = true; fgEnabled = false; taggedId = 0; fgReset = 0; fgFormat = desc.Format; generatedFrames = 0;
    if (!FGResult(p_xefgSwapChainSetEnabled(fg, false), "InitializeDisabled") ||
        !FGResult(p_xefgSwapChainSetUiCompositionState(fg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED), "SetUiComposition")) {
        DestroyFG(); return false;
    }
    failureReason = nullptr;
    logger::info("[XeSS FG] Swapchain initialized format={} maximum={}x", static_cast<uint32_t>(desc.Format), maxGenerated + 1);
    return true;
}

bool XeSS::DestroyFG()
{
    if (!DisableFG()) return false;
    if (fg && !FGResult(p_xefgSwapChainDestroy(fg), "Destroy")) return false;
    fg = nullptr; fgInitialized = false;
    if (ll) p_xellDestroyContext(ll);
    ll = nullptr;
    {
        std::scoped_lock lock(markerMutex);
        sleepFrame = UINT_MAX;
        sleepSucceeded = false;
        sleptPresentId = 0;
        simulationPresentId.store(0, std::memory_order_relaxed);
        completedSimulationId = renderStartedId = presentStartedId = presentAttemptId = 0;
        reportFloorId = presentId.load(std::memory_order_relaxed);
    }
    return true;
}

bool XeSS::DisableFG()
{
    const auto wasEnabled = fgEnabled;
    if (fgInitialized && fgEnabled && !FGResult(p_xefgSwapChainSetEnabled(fg, false), "Disable")) return false;
    fgEnabled = false; taggedId = 0; fgReset = 0;
    if (wasEnabled) ResetPacingHistory();
    return true;
}

bool XeSS::TagFrame(ID3D12GraphicsCommandList* commands, ID3D12Resource* color, ID3D12Resource* motion,
    ID3D12Resource* depth, float2 jitter, float2 renderSize, float2 displaySize, uint generated)
{
    if (!fgInitialized || !commands || !color || !motion || !depth) return false;
    // HUD-less color and the proxy backbuffer must share the same transfer/format.
    if (color->GetDesc().Format != fgFormat) { DisableFG(); return false; }
    const auto camera = Util::GetCameraProjection();
    if (!camera.cameraState || !camera.usedMatrixFOV) { DisableFG(); return false; }
    xefg_swapchain_frame_constant_data_t constants{};
    const auto projection = camera.cameraViewToClip;
    const auto view = DirectX::XMMatrixMultiply(Util::ToXMMatrix(camera.cameraState->camViewData.currentViewProjUnjittered),
        DirectX::XMMatrixInverse(nullptr, projection));
    memcpy(constants.viewMatrix, &view, sizeof(view)); memcpy(constants.projectionMatrix, &projection, sizeof(projection));
    constants.jitterOffsetX = -jitter.x; constants.jitterOffsetY = -jitter.y;
    constants.motionVectorScaleX = renderSize.x; constants.motionVectorScaleY = renderSize.y;
    generated = std::clamp(generated, 1u, maxGenerated);
    // Coldwood's count-change workaround: reset pacing when multiplier changes.
    if (generatedFrames != generated) {
        if (!DisableFG()) return false;
        if (!FGResult(p_xefgSwapChainSetNumInterpolatedFrames(fg, generated), "SetGeneratedCount")) return false;
        generatedFrames = generated;
    }
    const auto serial = resetSerial.load(std::memory_order_relaxed);
    constants.resetHistory = fgReset != serial || !fgEnabled;
    const auto now = std::chrono::steady_clock::now();
    const auto appTime = std::chrono::duration<float, std::milli>(now - lastTagTime).count();
    const auto pacedTime = static_cast<float>(XeFGPacing::RenderTimeMs());
    constants.frameRenderTime = std::clamp(pacedTime > 0.0f ? pacedTime : appTime, 0.1f, 250.0f);
    // Image history resets (including optional NR fallback) do not reset
    // measured pacing. Only an actual timing discontinuity uses a warmup.
    if (pacingWarmupPending.exchange(false, std::memory_order_acq_rel))
        constants.frameRenderTime = 1000.0f / 60.0f;
    lastTagTime = now;
    XeFGPacing::NoteFedFrameTime(constants.frameRenderTime);
    if (!FGResult(p_xefgSwapChainTagFrameConstants(fg, presentId, &constants), "TagConstants")) { DisableFG(); return false; }
    const auto tag = [&](ID3D12Resource* resource, xefg_swapchain_resource_type_t type, float2 size) {
        xefg_swapchain_d3d12_resource_data_t data{};
        data.type = type; data.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        data.pResource = resource; data.resourceSize = { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y) };
        data.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        // Copy into SDK-owned storage on our existing interop submission. The XeFG
        // async pacer must never retain mutable D3D11/shared per-slot guides.
        return FGResult(p_xefgSwapChainD3D12TagFrameResource(fg, commands, presentId, &data), "TagResource");
    };
    if (!tag(depth, XEFG_SWAPCHAIN_RES_DEPTH, renderSize) || !tag(motion, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, renderSize) ||
        !tag(color, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, displaySize) || !FGResult(p_xefgSwapChainSetEnabled(fg, true), "Enable")) {
        DisableFG(); return false;
    }
    generatedFrames = generated; fgEnabled = true; fgReset = serial; taggedId = presentId;
    return true;
}

void XeSS::RequestReset()
{
    resetSerial.fetch_add(1, std::memory_order_relaxed);
}

void XeSS::ResetPacingHistory()
{
    XeFGPacing::RequestTimingReset();
    pacingWarmupPending.store(true, std::memory_order_release);
    std::scoped_lock lock(markerMutex);
    reportFloorId = presentId.load(std::memory_order_relaxed);
}

void XeSS::Sleep(uint32_t engineFrame)
{
    std::scoped_lock lock(markerMutex);
    if (!ll || !fgInitialized || sleepFrame == engineFrame) return;
    auto id = presentId.load(std::memory_order_relaxed);
    if (sleepFrame != UINT_MAX && sleptPresentId == id) {
        // Renderer::End may skip Present altogether. The next real engine
        // frame must not reuse the abandoned logical XeLL/XeFG frame ID.
        ++id; if (!id) ++id;
        presentId.store(id, std::memory_order_relaxed);
        simulationPresentId.store(0, std::memory_order_relaxed);
        reportFloorId = id;
    }
    sleepFrame = engineFrame; // One Sleep attempt per engine frame, even on failure.
    sleptPresentId = id;
    const auto result = p_xellSleep(ll, sleptPresentId);
    sleepSucceeded = result >= 0;
    if (!sleepSucceeded) logger::warn("[XeLL] Sleep failed frame={} id={} result={}", engineFrame, sleptPresentId, static_cast<int>(result));
}
void XeSS::Marker(xell_latency_marker_type_t marker)
{
    std::scoped_lock lock(markerMutex);
    if (!ll || !fgInitialized) return;
    const auto id = presentId.load(std::memory_order_relaxed);
    if (!sleepSucceeded || sleptPresentId != id) return;
    if (marker == XELL_RENDERSUBMIT_START && completedSimulationId != id) return;
    if (marker == XELL_RENDERSUBMIT_END && renderStartedId != id) return;
    if (marker == XELL_PRESENT_START && renderStartedId != id) return;
    if (marker == XELL_PRESENT_END && presentStartedId != id) return;
    if (marker == XELL_SIMULATION_START) {
        simulationPresentId.store(id, std::memory_order_relaxed);
        completedSimulationId = renderStartedId = presentStartedId = 0;
    }
    const auto result = p_xellAddMarkerData(ll, id, marker);
    if (result < 0) {
        if (marker == XELL_SIMULATION_START) simulationPresentId.store(0, std::memory_order_relaxed);
        logger::warn("[XeLL] Marker {} failed id={} result={}", static_cast<int>(marker), id, static_cast<int>(result));
    } else if (marker == XELL_RENDERSUBMIT_START) {
        renderStartedId = id;
    } else if (marker == XELL_PRESENT_START) {
        presentStartedId = id;
    }
}
void XeSS::EndSimulation(uint32_t engineFrame)
{
    // Loading draws can advance Present while an old simulation is still blocked.
    // Do not attach its end marker to a newer frame or send it twice.
    std::scoped_lock lock(markerMutex);
    const auto id = simulationPresentId.exchange(0, std::memory_order_relaxed);
    if (ll && fgInitialized && sleepSucceeded && sleepFrame == engineFrame && id && id == presentId) {
        const auto result = p_xellAddMarkerData(ll, id, XELL_SIMULATION_END);
        if (result < 0) logger::warn("[XeLL] Simulation end failed id={} result={}", id, static_cast<int>(result));
        else completedSimulationId = id;
    }
}
bool XeSS::GetCPUFrameSpanMs(float& a_spanMs)
{
    std::scoped_lock lock(markerMutex);
    if (!ll || !fgInitialized || !p_xellGetFramesReports) return false;
    xell_frame_report_t reports[64]{};
    const auto result = p_xellGetFramesReports(ll, reports);
    if (result < 0) return false;
    const auto currentId = presentId.load(std::memory_order_relaxed);
    const xell_frame_report_t* latest = nullptr;
    for (const auto& report : reports) {
        if (!report.m_frame_id || report.m_frame_id < reportFloorId ||
            report.m_frame_id >= currentId || currentId - report.m_frame_id > 64 ||
            !report.m_sim_start_ts || !report.m_sim_end_ts || !report.m_render_submit_start_ts ||
            !report.m_render_submit_end_ts || !report.m_present_start_ts || !report.m_present_end_ts ||
            report.m_sim_end_ts < report.m_sim_start_ts ||
            report.m_present_end_ts <= report.m_sim_start_ts) continue;
        if (!latest || report.m_frame_id > latest->m_frame_id) latest = &report;
    }
    if (!latest) return false;
    a_spanMs = static_cast<float>(static_cast<double>(latest->m_present_end_ts - latest->m_sim_start_ts) / 1'000'000.0);
    return true;
}
bool XeSS::BeforePresent()
{
    if (!fgInitialized) return true;
    if (taggedId != presentId && !DisableFG()) return false;
    std::unique_lock lock(markerMutex);
    const auto id = presentId.load(std::memory_order_relaxed);
    if (!FGResult(p_xefgSwapChainSetPresentId(fg, id), "SetPresentId")) {
        lock.unlock();
        DisableFG();
        return false;
    }
    presentAttemptId = id;
    if (presentStartedId != id && sleepSucceeded && sleptPresentId == id && renderStartedId == id) {
        const auto renderEnd = p_xellAddMarkerData(ll, id, XELL_RENDERSUBMIT_END);
        const auto presentStart = renderEnd >= 0 ? p_xellAddMarkerData(ll, id, XELL_PRESENT_START) : renderEnd;
        if (presentStart < 0) {
            logger::warn("[XeLL] Present markers failed id={} result={}", id, static_cast<int>(presentStart));
            lock.unlock();
            DisableFG();
            return false;
        }
        presentStartedId = id;
    }
    return true;
}
void XeSS::AfterPresent(HRESULT result)
{
    if (!fgInitialized) return;
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) return;
    {
        std::scoped_lock lock(markerMutex);
        const auto id = presentAttemptId;
        if (id && id == presentId.load(std::memory_order_relaxed)) {
            if (sleepSucceeded && sleptPresentId == id && presentStartedId == id) {
                const auto markerResult = p_xellAddMarkerData(ll, id, XELL_PRESENT_END);
                if (markerResult < 0) logger::warn("[XeLL] Present end failed id={} result={}", id, static_cast<int>(markerResult));
            }
            auto next = id + 1; if (!next) ++next;
            presentId.store(next, std::memory_order_relaxed);
        }
        presentAttemptId = 0;
    }
    xefg_swapchain_present_status_t status{};
    if (result == S_OK && p_xefgSwapChainGetLastPresentStatus(fg, &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        presentedFrames.fetch_add(status.framesPresented, std::memory_order_relaxed);
    if (FAILED(result)) { RequestReset(); ResetPacingHistory(); }
}
