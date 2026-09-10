#include "PipboyTemporalMask.h"

#include "Upscaling.h"
#include "Util.h"
#include <d3dcompiler.h>
#include <cstring>

namespace PipboyTemporalMask
{
namespace
{
    std::unique_ptr<Texture2D> snapshot;
    winrt::com_ptr<ID3D11ComputeShader> extract;
    uint64_t capturedFrame = 0;

    winrt::com_ptr<ID3D12Device> mergeDevice;
    winrt::com_ptr<ID3D12RootSignature> mergeRoot;
    winrt::com_ptr<ID3D12PipelineState> mergePipeline;
    winrt::com_ptr<ID3D12DescriptorHeap> mergeHeap;

    void EnsureMerge(ID3D12Device* device)
    {
        if (mergeDevice.get() == device && mergePipeline && mergeHeap) return;
        // Device replacement is only allowed by the existing drained teardown.
        mergeDevice.copy_from(device);
        mergeRoot = nullptr;
        mergePipeline = nullptr;
        mergeHeap = nullptr;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ DXGI_FORMAT_R8_UNORM };
        DX::ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
        constexpr auto required = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
        if ((support.Support2 & required) != required) throw std::runtime_error("R8 typed UAV load/store unsupported");
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 1 };
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        winrt::com_ptr<ID3DBlob> rootBlob, errors, code;
        DX::ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.put(), errors.put()));
        DX::ThrowIfFailed(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(mergeRoot.put())));
        constexpr char source[] = R"(
Texture2D<float> Pipboy : register(t0);
RWTexture2D<float> Reactive : register(u0);
cbuffer Options : register(b0) { uint HasReactive; };
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w,h; Reactive.GetDimensions(w,h);
    if(id.x>=w || id.y>=h) return;
    float prior = 0;
    if(HasReactive != 0) prior = Reactive[id.xy];
    Reactive[id.xy] = max(prior, Pipboy.Load(int3(id.xy,0)) > 0 ? 0.8 : 0);
})";
        errors = nullptr;
        DX::ThrowIfFailed(D3DCompile(source, std::strlen(source), "PipboyReactiveMerge", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put()));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = mergeRoot.get();
        pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        DX::ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(mergePipeline.put())));
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 2 * kDX12FrameCount;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        DX::ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(mergeHeap.put())));
    }
}

void Capture()
{
    capturedFrame = 0;
    auto* upscaling = Upscaling::GetSingleton();
    if (upscaling->upscaleMethod != Upscaling::UpscaleMethod::kDLSS &&
        upscaling->upscaleMethod != Upscaling::UpscaleMethod::kFSR) return;
    // Only the wrist screen participates in world SR. PA draws in late UI.
    auto* renderer = RE::Interface3D::Renderer::GetByName(RE::BSFixedString("PipboyMenu"));
    if (!renderer || !renderer->enabled || !renderer->worldAttachedElementRoot ||
        renderer->screenmode.get() != RE::Interface3D::ScreenMode::kWorldAttached) return;
    try {
        auto* data = RE::BSGraphics::GetRendererData();
        auto& material = data->renderTargets[Util::ResolveRenderTarget(Util::RenderTarget::kGbufferMaterial)];
        auto* source = reinterpret_cast<ID3D11Texture2D*>(material.texture);
        auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(material.srView);
        if (!source || !srv) return;
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1) return;
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
        if (!extract) extract.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
            L"Data/F4SE/Plugins/Upscaling/PipboyTemporalMaskCS.hlsl", {}, "cs_5_0")));
        if (!extract) return;
        if (!snapshot || snapshot->desc.Width != desc.Width || snapshot->desc.Height != desc.Height) {
            upscaling->RetireD3D11Texture(snapshot);
            desc.Format = DXGI_FORMAT_R8_UNORM;
            desc.MipLevels = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags = desc.CPUAccessFlags = 0;
            auto replacement = std::make_unique<Texture2D>(desc);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = desc.Format;
            uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            replacement->CreateUAV(uav);
            snapshot = std::move(replacement);
        }
        // Keep the engine/wrapper context identity intact. A context-state swap
        // changes graphics state behind ordinary SetShader hooks even though this
        // pass only needs CS t0/u0 and temporary unbinding of the material RTV.
        struct Restore {
            ID3D11DeviceContext* context;
            winrt::com_ptr<ID3D11ComputeShader> shader;
            winrt::com_ptr<ID3D11ShaderResourceView> input;
            winrt::com_ptr<ID3D11DepthStencilView> depth;
            ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
            UINT instanceCount = D3D11_SHADER_MAX_INTERFACES;
            ID3D11UnorderedAccessView* outputs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
            ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            UINT targetCount = 0;
            explicit Restore(ID3D11DeviceContext* value) : context(value) {
                context->CSGetShader(shader.put(), instances, &instanceCount);
                context->CSGetShaderResources(0, 1, input.put());
                context->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, outputs);
                context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth.put());
                for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                    if (targets[i]) targetCount = i + 1;
                // Preserve pixel UAVs and their counters; only detach RTV/DSV.
                context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
                    0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
                ID3D11UnorderedAccessView* empty[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
                context->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, empty, nullptr);
            }
            ~Restore() {
                ID3D11ShaderResourceView* noInput = nullptr;
                ID3D11UnorderedAccessView* noOutput = nullptr;
                context->CSSetShaderResources(0, 1, &noInput);
                context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
                context->OMSetRenderTargetsAndUnorderedAccessViews(targetCount, targets, depth.get(),
                    0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
                auto* oldInput = input.get();
                context->CSSetShaderResources(0, 1, &oldInput);
                context->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, outputs, nullptr);
                context->CSSetShader(shader.get(), instances, instanceCount);
                for (auto* value : targets) if (value) value->Release();
                for (auto* value : outputs) if (value) value->Release();
                for (UINT i = 0; i < instanceCount; ++i) if (instances[i]) instances[i]->Release();
            }
        } restore(context);
        auto* output = snapshot->uav.get();
        context->CSSetShaderResources(0, 1, &srv);
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->CSSetShader(extract.get(), nullptr, 0);
        context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
        capturedFrame = static_cast<uint64_t>(Util::State_GetSingleton()->frameCount) + 1;
        static UINT loggedWidth = 0;
        static UINT loggedHeight = 0;
        if (loggedWidth != desc.Width || loggedHeight != desc.Height) {
            logger::info("[Pipboy SR] temporal mask captured material={}x{} snapshot={} resource={}",
                desc.Width, desc.Height, static_cast<uint32_t>(snapshot->desc.Format), static_cast<void*>(snapshot->resource.get()));
            loggedWidth = desc.Width;
            loggedHeight = desc.Height;
        }
    } catch (const std::exception& e) {
        logger::warn("[Pipboy SR] Mask capture unavailable: {}", e.what());
    }
}

ID3D11Texture2D* Current(UINT width, UINT height)
{
    if (capturedFrame != static_cast<uint64_t>(Util::State_GetSingleton()->frameCount) + 1 ||
        !snapshot || width > snapshot->desc.Width || height > snapshot->desc.Height) return nullptr;
    return snapshot->resource.get();
}

void Release()
{
    capturedFrame = 0;
    Upscaling::GetSingleton()->RetireD3D11Texture(snapshot);
    extract = nullptr;
    // Keep the merge pipeline/descriptors across quality changes; slots are
    // protected by the same fences as their SR inputs, independent of size.
}

bool MergeReactive(ID3D12Device* device, ID3D12GraphicsCommandList* commands,
    ID3D12Resource* mask, ID3D12Resource* reactive, UINT slot, bool hasReactive)
{
    if (!mask || !reactive || slot >= kDX12FrameCount) return false;
    try {
        EnsureMerge(device);
        auto cpu = mergeHeap->GetCPUDescriptorHandleForHeapStart();
        const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu.ptr += slot * 2 * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(mask, &srv, cpu);
        cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(reactive, nullptr, &uav, cpu);
        auto read = CD3DX12_RESOURCE_BARRIER::Transition(mask, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &read);
        if (hasReactive) {
            auto order = CD3DX12_RESOURCE_BARRIER::UAV(reactive);
            commands->ResourceBarrier(1, &order);
        }
        ID3D12DescriptorHeap* heaps[]{ mergeHeap.get() };
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(mergeRoot.get());
        commands->SetPipelineState(mergePipeline.get());
        auto gpu = mergeHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += slot * 2 * stride;
        commands->SetComputeRootDescriptorTable(0, gpu);
        commands->SetComputeRoot32BitConstant(1, hasReactive ? 1 : 0, 0);
        const auto desc = reactive->GetDesc();
        commands->Dispatch((static_cast<UINT>(desc.Width) + 7) / 8, (desc.Height + 7) / 8, 1);
        read = CD3DX12_RESOURCE_BARRIER::Transition(mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        commands->ResourceBarrier(1, &read);
        return true;
    } catch (const std::exception& e) {
        logger::warn("[Pipboy SR] Reactive merge unavailable: {}", e.what());
        return false;
    }
}
}
