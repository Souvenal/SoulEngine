module GBuffer;

public struct GBufferPixel {
    public float4 albedo : SV_Target0;
    public float4 normal : SV_Target1;
    public uint   entityId : SV_Target2;
    public float4 worldPosition : SV_Target3;
};

public float4 encodeGBufferNormal(float3 normal) {
    return float4(normalize(normal) * 0.5 + 0.5, 1.0);
}

public float3 decodeGBufferNormal(float4 encodedNormal) {
    return normalize(encodedNormal.xyz * 2.0 - 1.0);
}

public static const uint gBufferBackgroundEntityId = 0u;
