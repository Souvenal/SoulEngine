import Common.All;
import Common.Material;
import Common.Pbr;

module DeferredLighting;

public struct DeferredLightData {
    public float4 colorIntensity;
    public float4 positionRange;
    public float4 directionType;
    public float4 spotCone;
};

public struct DeferredResources {
    public ConstantBuffer<FrameData>         frame;
    public ConstantBuffer<ViewData>          view;
    public StructuredBuffer<MaterialRecord> materials;
    public StructuredBuffer<DeferredLightData> lights;
};

public ParameterBlock<DeferredResources> g_deferred;
public ParameterBlock<GBuffer> g_gbuffer;

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

float3 evaluateDeferredLight(
    DeferredLightData light,
    MaterialSurface material,
    float3 worldPosition,
    float3 normal,
    float3 viewDirection)
{
    uint lightType = uint(light.directionType.w);
    float3 lightDirection = float3(0.0);
    float3 illuminance = float3(0.0);
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
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotL <= 0.0)
        return float3(0.0);
    return evaluatePbrBrdf(material, normal, viewDirection, lightDirection) * illuminance * nDotL;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0 {
    uint2 pixel = uint2(input.position.xy);
    float depth = g_gbuffer.depth.Load(int3(pixel, 0));
    if (depth >= 1.0)
        return float4(0.025, 0.035, 0.055, 1.0);

    float3 worldPosition = reconstructGBufferWorldPosition(
        pixel,
        depth,
        g_deferred.view.viewportSize,
        g_deferred.view.viewProjectionInverse);
    float3 normal = normalize(g_gbuffer.normal.Load(int3(pixel, 0)));
    float3 albedo = g_gbuffer.albedo.Load(int3(pixel, 0)).rgb;
    uint materialId = g_gbuffer.materialId.Load(int3(pixel, 0));
    MaterialRecord materialData = g_deferred.materials[materialId];
    MaterialSurface material = {};
    material.baseColor = albedo;
    material.metallic = saturate(materialData.metallicFactor);
    material.roughness = saturateRoughness(materialData.roughnessFactor);
    float3 viewDirection = normalize(g_deferred.view.cameraPosition.xyz - worldPosition);

    float3 color = 0.035 * albedo;
    for (uint lightIndex = 0u; lightIndex < g_deferred.frame.lightCount; ++lightIndex)
        color += evaluateDeferredLight(
            g_deferred.lights[lightIndex], material, worldPosition, normal, viewDirection);

    color *= exp2(-g_deferred.frame.exposureEV100);
    color = color / (color + 1.0);
    color = pow(max(color, 0.0), 1.0 / 2.2);
    return float4(color, 1.0);
}
