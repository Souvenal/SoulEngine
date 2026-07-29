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

public struct InstanceData {
    public float4x4 worldTransform;
    // xyz is the world-space center; w is a conservative world-space radius.
    public float4   boundingSphere;
    public uint     materialIndex;
};

public struct ForwardMaterialData {
    public float4 baseColorFactor;
    public float4 emissiveFactor;
    public float metallicFactor;
    public float roughnessFactor;
    // Index into g_textures.uTextures; negative disables base-color texture sampling.
    public int baseColorTextureIndex;
    public int normalTextureIndex;
    public int metallicRoughnessTextureIndex;
    public int metallicTextureIndex;
    public int roughnessTextureIndex;
    public int occlusionTextureIndex;
    public int emissiveTextureIndex;
};

public struct ForwardFrameViewResources {
    public ConstantBuffer<ForwardFrameData> frame;
    public ConstantBuffer<ForwardViewData>  view;
};

public ParameterBlock<ForwardFrameViewResources> g_forwardFrameView;

public struct ForwardDrawResources {
    public StructuredBuffer<InstanceData> instances;
    public StructuredBuffer<ForwardMaterialData> materials;
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
    ForwardMaterialData materialData = g_forwardDraw.materials[instanceData.materialIndex];
    float3 baseColor = materialData.baseColorFactor.rgb;
    if (materialData.baseColorTextureIndex >= 0) {
        float3 sampled = g_textures.uTextures[materialData.baseColorTextureIndex]
                             .Sample(g_samplers.uSamplerLinear, input.uv).rgb;
        // Sampled textures are stored UNORM without hardware sRGB decode; convert to linear.
        baseColor *= pow(sampled, 2.2);
    }

    float metallic = materialData.metallicFactor;
    float roughness = materialData.roughnessFactor;
    if (materialData.metallicRoughnessTextureIndex >= 0) {
        float4 packed = g_textures.uTextures[materialData.metallicRoughnessTextureIndex]
                            .Sample(g_samplers.uSamplerLinear, input.uv);
        roughness *= packed.g;
        metallic *= packed.b;
    }
    if (materialData.metallicTextureIndex >= 0)
        metallic *= g_textures.uTextures[materialData.metallicTextureIndex]
                        .Sample(g_samplers.uSamplerLinear, input.uv).r;
    if (materialData.roughnessTextureIndex >= 0)
        roughness *= g_textures.uTextures[materialData.roughnessTextureIndex]
                        .Sample(g_samplers.uSamplerLinear, input.uv).r;

    float3 normal = normalize(input.normal);
    if (materialData.normalTextureIndex >= 0) {
        float3 normalSample = g_textures.uTextures[materialData.normalTextureIndex]
                                  .Sample(g_samplers.uSamplerLinear, input.uv).xyz * 2.0 - 1.0;
        float3 tangent = normalize(input.tangent.xyz);
        float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
        normal = normalize(normalSample.x * tangent + normalSample.y * bitangent + normalSample.z * normal);
    }

    float occlusion = 1.0;
    if (materialData.occlusionTextureIndex >= 0)
        occlusion = g_textures.uTextures[materialData.occlusionTextureIndex]
                        .Sample(g_samplers.uSamplerLinear, input.uv).r;
    float3 emissive = materialData.emissiveFactor.rgb;
    if (materialData.emissiveTextureIndex >= 0) {
        float3 sampled = g_textures.uTextures[materialData.emissiveTextureIndex]
                             .Sample(g_samplers.uSamplerLinear, input.uv).rgb;
        emissive *= pow(sampled, 2.2);
    }

    PbrMaterial material = makePbrMaterial(baseColor, metallic, roughness);
    float3 lightDirection = normalize(-g_forwardFrameView.frame.directionalLightDirectionIntensity.xyz);
    float3 radiance = g_forwardFrameView.frame.directionalLightColor.rgb *
                      g_forwardFrameView.frame.directionalLightDirectionIntensity.w;
    float nDotL = saturate(dot(normal, lightDirection));
    float3 viewDirection = normalize(g_forwardFrameView.view.cameraPosition.xyz - input.worldPosition);
    float3 ambient = 0.035 * material.baseColor * (1.0 - material.metallic) * occlusion;
    float3 color = ambient + emissive + evaluatePbrBrdf(material, normal, viewDirection, lightDirection) * radiance * nDotL;
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, materialData.baseColorFactor.a);
}
