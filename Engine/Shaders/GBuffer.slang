module GBuffer;

/// Shared read-only resources for visibility and deferred passes.
public struct GBuffer {
    public Texture2D<float4> albedo;
    public Texture2D<float3> normal;
    public Texture2D<uint>   materialId;
    public Texture2D<uint>   entityId;
    public Texture2D<float>  depth;
};

/// Raster MRT output; hardware depth is written through the depth attachment.
public struct GBufferPixel {
    public float4 albedo    : SV_Target0;
    public float3 normal    : SV_Target1;
    public uint   materialId : SV_Target2;
    public uint   entityId  : SV_Target3;

    public __init(float4 albedo, float3 normal, uint materialId, uint entityId)
    {
        this.albedo = albedo;
        this.normal = normalize(normal);
        this.materialId = materialId;
        this.entityId = entityId;
    }
};

/// Reconstructs a world position for Vulkan zero-to-one depth and row-vector matrices.
public float3 reconstructGBufferWorldPosition(
    uint2 pixel,
    float depth,
    float2 viewportSize,
    float4x4 inverseViewProjection)
{
    float2 uv = (float2(pixel) + 0.5) / viewportSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 worldPosition = mul(float4(ndc, depth, 1.0), inverseViewProjection);
    return worldPosition.xyz / worldPosition.w;
}