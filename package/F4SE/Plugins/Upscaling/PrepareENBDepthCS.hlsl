// ENB native-depth bridge and Fallout depth outputs in one dispatch.
//
// u0: full-resolution linear depth used by Fallout image-space effects
// u1: render-resolution raw depth used by the ordinary DRS proxy path
// u2: full-resolution raw depth exposed to ENB TextureDepth/private passes

Texture2D<float> DepthInput : register(t0);

RWTexture2D<float> LinearDepthOutput : register(u0);
RWTexture2D<float> RenderDepthOutput : register(u1);
RWTexture2D<float> NativeDepthOutput : register(u2);

cbuffer Upscaling : register(b0)
{
    uint2 ScreenSize;
    uint2 RenderSize;
    float4 CameraData;
};

float GetScreenDepth(float depth)
{
    return CameraData.w / (-depth * CameraData.z + CameraData.x);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
    if (any(dispatchID.xy >= ScreenSize))
        return;

    const uint2 renderCoord = dispatchID.xy * RenderSize / ScreenSize;
    const float nativeDepth = DepthInput[renderCoord];

    LinearDepthOutput[dispatchID.xy] = GetScreenDepth(nativeDepth);
    NativeDepthOutput[dispatchID.xy] = nativeDepth;

    // The top-left render rectangle is the only valid region of the ordinary
    // DRS raw-depth target. Avoid touching the unused tail of its native-sized
    // allocation.
    if (all(dispatchID.xy < RenderSize))
        RenderDepthOutput[dispatchID.xy] = DepthInput[dispatchID.xy];
}
