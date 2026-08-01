import Pbr;

public struct ForwardFrameData {
    public float time;
    public float exposureEV100;
    public uint  lightCount;
};

public struct ForwardViewData {
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
};

public struct ForwardFrameViewResources {
    public ConstantBuffer<ForwardFrameData> frame;
    public ConstantBuffer<ForwardViewData>  view;
    public StructuredBuffer<LightData>       lights;
};

public ParameterBlock<ForwardFrameViewResources> g_forwardFrameView;

public struct ForwardDrawResources {
    public StructuredBuffer<InstanceData> instances;
    public StructuredBuffer<PbrMaterialData> materials;
};

public ParameterBlock<ForwardDrawResources> g_forwardDraw;

public struct ForwardInstanceIndex {
    public uint value;
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
VSOutput vertMain(VSInput input, uniform ForwardInstanceIndex instanceIndex) {
    VSOutput output;
    float4 worldPosition =
        mul(float4(input.inPosition, 1.0), g_forwardDraw.instances[instanceIndex.value].worldTransform);
    output.worldPosition  = worldPosition.xyz;
    output.normal = input.inNormal;
    output.tangent = input.inTangent;
    output.uv = input.inUV;
    output.instanceIndex      = instanceIndex.value;
    output.position       = mul(worldPosition, g_forwardFrameView.view.viewProjection);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET {
    InstanceData instanceData = g_forwardDraw.instances[input.instanceIndex];
    PbrSurfaceMaterial surface = resolvePbrMaterial(
        g_forwardDraw.materials[instanceData.materialIndex], input.uv, input.normal, input.tangent);
    PbrMaterial material = surface.material;
    float3 normal = surface.normal;
    float3 viewDirection = normalize(g_forwardFrameView.view.cameraPosition.xyz - input.worldPosition);
    float3 color = 0.035 * material.baseColor * (1.0 - material.metallic) * surface.occlusion + surface.emissive;
    for (uint lightIndex = 0u; lightIndex < g_forwardFrameView.frame.lightCount; ++lightIndex)
        color += evaluateDirectLight(g_forwardFrameView.lights[lightIndex], material, input.worldPosition, normal, viewDirection);
    color *= exp2(-g_forwardFrameView.frame.exposureEV100);
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0);
}