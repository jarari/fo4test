Texture2D<float4> Material : register(t0);
RWTexture2D<float> Mask : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    Mask.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    // Raw raster coordinates: SR input and material are both jittered.
    // Applying the vanilla TAA output-space de-jitter here would misalign them.
    // DLSS AnimatedTextureMask is a binary input: 1 marks the scrolling
    // texture and 0 leaves the normal temporal path unchanged.
    Mask[id.xy] = abs(Material.Load(int3(id.xy, 0)).w * 255.0 - 4.0) < 0.25 ? 1.0 : 0.0;
}
