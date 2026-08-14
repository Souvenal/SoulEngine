public struct DeferredFrameData {
    public float exposureEV100;
    public uint  lightCount;
};

public struct DeferredLightData {
    public float4 colorIntensity;
    public float4 positionRange;
    public float4 directionType;
    public float4 spotCone;
};

public struct DeferredResources {
    public RWTexture2D<float4> albedo;
    public RWTexture2D<float4> normal;
    public RWTexture2D<float4> worldPosition;
    public ConstantBuffer<DeferredFrameData> frame;
    public StructuredBuffer<DeferredLightData> lights;
};

public ParameterBlock<DeferredResources> g_deferred;

struct VSOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertMain(uint vertexId : SV_VertexID) {
    VSOutput output;
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

float3 evaluateDeferredLight(DeferredLightData light, float3 worldPosition, float3 normal) {
    uint lightType = uint(light.directionType.w);
    float3 lightDirection;
    float3 illuminance;
    if (lightType == 1u) {
        lightDirection = normalize(-light.directionType.xyz);
        illuminance = light.colorIntensity.rgb * light.colorIntensity.w;
    } else {
        float3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight <= 0.0001 || distanceToLight >= light.positionRange.w)
            return float3(0.0);
        lightDirection = toLight / distanceToLight;
        float rangeFade = saturate(1.0 - pow(distanceToLight / light.positionRange.w, 4.0));
        illuminance = light.colorIntensity.rgb * light.colorIntensity.w * rangeFade * rangeFade /
                      (distanceToLight * distanceToLight);
        if (lightType == 3u) {
            float coneCosine = dot(normalize(worldPosition - light.positionRange.xyz),
                                   normalize(light.directionType.xyz));
            illuminance *= smoothstep(light.spotCone.y, light.spotCone.x, coneCosine);
        }
    }
    return illuminance * saturate(dot(normal, lightDirection));
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0 {
    uint2 pixel = uint2(input.position.xy);
    float4 albedoSample = g_deferred.albedo.Load(int2(pixel));
    float3 normal = normalize(g_deferred.normal.Load(int2(pixel)).xyz * 2.0 - 1.0);
    float3 worldPosition = g_deferred.worldPosition.Load(int2(pixel)).xyz;

    float3 color = 0.035 * albedoSample.rgb;
    for (uint lightIndex = 0u; lightIndex < g_deferred.frame.lightCount; ++lightIndex)
        color += albedoSample.rgb * evaluateDeferredLight(g_deferred.lights[lightIndex], worldPosition, normal);

    color *= exp2(-g_deferred.frame.exposureEV100);
    color = color / (color + 1.0);
    color = pow(max(color, 0.0), 1.0 / 2.2);
    return float4(color, 1.0);
}
