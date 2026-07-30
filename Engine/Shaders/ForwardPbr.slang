import Pbr;

public struct ForwardFrameData {
    public float  time;
    public float4 directionalLightDirectionIntensity;
    public float4 directionalLightColor;
};

public struct ForwardViewData {
    public float4x4 viewProjection;
    public float4   cameraPosition;
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
    float3 lightDirection = normalize(-g_forwardFrameView.frame.directionalLightDirectionIntensity.xyz);
    float3 radiance = g_forwardFrameView.frame.directionalLightColor.rgb *
                      g_forwardFrameView.frame.directionalLightDirectionIntensity.w;
    float nDotL = saturate(dot(normal, lightDirection));
    float3 viewDirection = normalize(g_forwardFrameView.view.cameraPosition.xyz - input.worldPosition);
    float3 ambient = 0.035 * material.baseColor * (1.0 - material.metallic) * surface.occlusion;
    float3 color = ambient + surface.emissive + evaluatePbrBrdf(material, normal, viewDirection, lightDirection) * radiance * nDotL;
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0);
}
