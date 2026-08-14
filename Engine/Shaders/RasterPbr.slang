import Pbr;
import GBuffer;

public struct RasterFrameData {
    public float time;
    public float exposureEV100;
    public uint  lightCount;
};

public struct RasterViewData {
    public float4x4 viewProjection;
    public float4   cameraPosition;
};

public struct LightData {
    // rgb is unit-luminance linear-sRGB chromaticity; w is lux or candela.
    public float4 colorIntensity;
    // xyz is world position in metres; w is finite-light range in metres.
    public float4 positionRange;
    // xyz is world -Z direction; w is LightType (1 directional, 2 point, 3 spot).
    public float4 directionType;
    // x/y are inner/outer spot cone cosines; z carries shadow metadata.
    public float4 spotCone;
};

public struct InstanceData {
    public float4x4 worldTransform;
    // xyz is the world-space center; w is a conservative world-space radius.
    public float4   boundingSphere;
    public uint     materialIndex;
    public uint     entityId;
};

public struct RasterFrameViewResources {
    public ConstantBuffer<RasterFrameData> frame;
    public ConstantBuffer<RasterViewData>  view;
    public StructuredBuffer<LightData>       lights;
};

public ParameterBlock<RasterFrameViewResources> g_rasterFrameView;

public struct RasterDrawResources {
    public StructuredBuffer<InstanceData> instances;
    public StructuredBuffer<PbrMaterialData> materials;
};

public ParameterBlock<RasterDrawResources> g_rasterDraw;

public struct RasterInstanceIndex {
    public uint value;
    public uint selected;
};

struct VSInput {
    float3 inPosition : POSITION;
    float3 inNormal   : NORMAL;
    float4 inTangent  : TANGENT;
    float2 inUV       : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
    float4 tangent;
    float2 uv;
    nointerpolation uint instanceIndex;
    nointerpolation uint selected;
};

float3 evaluateDirectLight(LightData light, PbrMaterial material, float3 worldPosition, float3 normal, float3 viewDirection) {
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
            float coneCosine = dot(normalize(worldPosition - light.positionRange.xyz), normalize(light.directionType.xyz));
            illuminance *= smoothstep(light.spotCone.y, light.spotCone.x, coneCosine);
        }
    }
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotL <= 0.0)
        return float3(0.0);
    return evaluatePbrBrdf(material, normal, viewDirection, lightDirection) * illuminance * nDotL;
}

[shader("vertex")]
VSOutput vertMain(VSInput input, uniform RasterInstanceIndex instanceIndex) {
    VSOutput output;
    InstanceData instanceData = g_rasterDraw.instances[instanceIndex.value];
    float3 localPosition = input.inPosition;
    if (instanceIndex.selected != 0u)
        localPosition += normalize(input.inNormal) * 0.02;
    float4 worldPosition =
        mul(float4(localPosition, 1.0), instanceData.worldTransform);
    output.worldPosition  = worldPosition.xyz;
    output.normal = input.inNormal;
    output.tangent = input.inTangent;
    output.uv = input.inUV;
    output.instanceIndex = instanceIndex.value;
    output.selected      = instanceIndex.selected;
    output.position      = mul(worldPosition, g_rasterFrameView.view.viewProjection);
    return output;
}

GBufferPixel MakeGBufferOutput(float4 albedo, float3 normal, uint entityId, float3 worldPosition) {
    GBufferPixel output;
    output.albedo = albedo;
    output.normal = encodeGBufferNormal(normal);
    output.entityId = entityId;
    output.worldPosition = float4(worldPosition, 1.0);
    return output;
}

[shader("fragment")]
GBufferPixel fragMain(VSOutput input) {
    InstanceData instanceData = g_rasterDraw.instances[input.instanceIndex];
    if (input.selected != 0u)
        return MakeGBufferOutput(float4(1.0, 0.35, 0.0, 1.0), input.normal, instanceData.entityId, input.worldPosition);
    PbrSurfaceMaterial surface = resolvePbrMaterial(
        g_rasterDraw.materials[instanceData.materialIndex], input.uv, input.normal, input.tangent);
    return MakeGBufferOutput(float4(surface.material.baseColor, 1.0), surface.normal, instanceData.entityId, input.worldPosition);
}
