import Bindless;
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

public struct ObjectData {
    public float4x4 worldTransform;
};

public struct ForwardFrameViewResources {
    public ConstantBuffer<ForwardFrameData> frame;
    public ConstantBuffer<ForwardViewData>  view;
};

public ParameterBlock<ForwardFrameViewResources> g_forwardFrameView;

public struct ForwardObjectResources {
    public ConstantBuffer<ObjectData> object;
};

public ParameterBlock<ForwardObjectResources> g_forwardObject;

public struct MaterialData {
    public float4 baseColorFactor;
    public float  metallicFactor;
    public float  roughnessFactor;
    // Index into g_textures.uTextures; negative disables base-color texture sampling.
    public int    baseColorTextureIndex;
};

public struct ForwardMaterialResources {
    public ConstantBuffer<MaterialData> material;
};

public ParameterBlock<ForwardMaterialResources> g_forwardMaterial;

struct VSInput {
    float3 inPosition;
    float3 inNormal;
    float2 inUV;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
    float2 uv;
};

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;
    float4 worldPosition = mul(float4(input.inPosition, 1.0), g_forwardObject.object.worldTransform);
    output.worldPosition  = worldPosition.xyz;
    output.normal         = input.inNormal;
    output.uv             = input.inUV;
    output.position       = mul(worldPosition, g_forwardFrameView.view.viewProjection);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET {
    float3 baseColor = g_forwardMaterial.material.baseColorFactor.rgb;
    if (g_forwardMaterial.material.baseColorTextureIndex >= 0) {
        float3 sampled = g_textures.uTextures[g_forwardMaterial.material.baseColorTextureIndex]
                             .Sample(g_samplers.uSamplerLinear, input.uv)
                             .rgb;
        // Sampled textures are stored UNORM without hardware sRGB decode; convert to linear.
        baseColor *= pow(sampled, 2.2);
    }
    PbrMaterial material = makePbrMaterial(baseColor,
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
