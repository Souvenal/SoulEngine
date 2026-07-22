import Pbr;

public struct FrameData {
    public float  time;
    public float4 directionalLightDirectionIntensity;
    public float4 directionalLightColor;
};

public struct ViewData {
    public float4x4 viewProjection;
    public float4   cameraPosition;
};

public struct ObjectData {
    public float4x4 worldTransform;
};

public struct FrameViewResources {
    public ConstantBuffer<FrameData> frame;
    public ConstantBuffer<ViewData>  view;
};

public ParameterBlock<FrameViewResources> g_forwardFrameView;

public struct ForwardObjectResources {
    public ConstantBuffer<ObjectData> object;
};

public ParameterBlock<ForwardObjectResources> g_forwardObject;

public struct MaterialData {
    public float4 baseColorFactor;
    public float  metallicFactor;
    public float  roughnessFactor;
};

public struct ForwardMaterialResources {
    public ConstantBuffer<MaterialData> material;
};

public ParameterBlock<ForwardMaterialResources> g_forwardMaterial;

struct VSInput {
    float3 inPosition;
    float3 inNormal;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
};

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;
    float4 worldPosition = mul(float4(input.inPosition, 1.0), g_forwardObject.object.worldTransform);
    output.worldPosition  = worldPosition.xyz;
    output.normal         = input.inNormal;
    output.position       = mul(worldPosition, g_forwardFrameView.view.viewProjection);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET {
    PbrMaterial material = makePbrMaterial(g_forwardMaterial.material.baseColorFactor.rgb,
                                           g_forwardMaterial.material.metallicFactor,
                                           g_forwardMaterial.material.roughnessFactor);
    float3 normal = normalize(input.normal);
    float3 lightDirection = normalize(-g_forwardFrameView.frame.directionalLightDirectionIntensity.xyz);
    float3 radiance = g_forwardFrameView.frame.directionalLightColor.rgb *
                      g_forwardFrameView.frame.directionalLightDirectionIntensity.w;
    float nDotL = saturate(dot(normal, lightDirection));
    float3 viewDirection = normalize(g_forwardFrameView.view.cameraPosition.xyz - input.worldPosition);
    float3 ambient = 0.035 * material.baseColor * (1.0 - material.metallic);
    float3 color = ambient + evaluatePbrBrdf(material, normal, viewDirection, lightDirection) * radiance * nDotL;
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, g_forwardMaterial.material.baseColorFactor.a);
}
