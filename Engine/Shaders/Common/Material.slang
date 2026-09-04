module Material;

import Common.All;

public struct MaterialSurface {
    public float3 baseColor;
    public float  metallic;
    public float  roughness;
    public float3 normal;
    public float3 emissive;
    public float  occlusion;
};

public bool hasTexture(Texture2D<float4>.Handle handle) {
    uint2 value = (uint2)handle;
    return value.x != 0 || value.y != 0;
}

public struct MaterialRecord {
    public float3 ambient;
    public float3 diffuse;
    public float3 specular;
    public float3 emissive;
    public float3 transparent;
    public float3 reflective;

    public float opacity;
    public float bumpScaling;
    public float shininess;
    public float shininessStrength;
    public float refractionIndex;
    public float reflectivity;
    public uint enableWireframe;
    public uint twoSided;
    public uint blendFunction;
    public uint shadingModel;

    public float4 baseColorFactor;
    public float metallicFactor;
    public float roughnessFactor;
    public float3 sheenColorFactor;
    public float sheenRoughnessFactor;
    public float clearcoatFactor;
    public float clearcoatRoughnessFactor;
    public float transmissionFactor;
    public float volumeThicknessFactor;
    public float volumeAttenuationDistance;
    public float3 volumeAttenuationColor;

    public Texture2D<float4>.Handle baseColorTexture;
    public Texture2D<float4>.Handle normalTexture;
    public Texture2D<float4>.Handle metallicRoughnessTexture;
    public Texture2D<float4>.Handle metallicTexture;
    public Texture2D<float4>.Handle roughnessTexture;
    public Texture2D<float4>.Handle occlusionTexture;
    public Texture2D<float4>.Handle emissiveTexture;

    public MaterialSurface resolveSurface(float2 uv, float3 geometricNormal, float4 tangent, SamplerState linearSampler) {
        float3 baseColor = baseColorFactor.rgb;
        if (hasTexture(baseColorTexture))
            baseColor *= pow(nonuniform(baseColorTexture).SampleLevel(linearSampler, uv, 0.0).rgb, 2.2);

        float metallic = metallicFactor;
        float roughness = roughnessFactor;
        if (hasTexture(metallicRoughnessTexture)) {
            float4 packed = nonuniform(metallicRoughnessTexture).SampleLevel(linearSampler, uv, 0.0);
            roughness *= packed.g;
            metallic *= packed.b;
        }
        if (hasTexture(metallicTexture))
            metallic *= nonuniform(metallicTexture).SampleLevel(linearSampler, uv, 0.0).r;
        if (hasTexture(roughnessTexture))
            roughness *= nonuniform(roughnessTexture).SampleLevel(linearSampler, uv, 0.0).r;

        float3 normal = normalize(geometricNormal);
        if (hasTexture(normalTexture)) {
            float3 sample = nonuniform(normalTexture).SampleLevel(linearSampler, uv, 0.0).xyz * 2.0 - 1.0;
            float3 basisTangent = normalize(tangent.xyz - normal * dot(normal, tangent.xyz));
            float3 bitangent = normalize(cross(normal, basisTangent)) * tangent.w;
            normal = normalize(sample.x * basisTangent + sample.y * bitangent + sample.z * normal);
        }

        float occlusion = hasTexture(occlusionTexture)
            ? nonuniform(occlusionTexture).SampleLevel(linearSampler, uv, 0.0).r
            : 1.0;
        float3 resolvedEmissive = emissive;
        if (hasTexture(emissiveTexture))
            resolvedEmissive *= pow(nonuniform(emissiveTexture).SampleLevel(linearSampler, uv, 0.0).rgb, 2.2);

        MaterialSurface result;
        result.baseColor = baseColor;
        result.metallic = saturate(metallic);
        result.roughness = clamp(roughness, 0.04, 1.0);
        result.normal = normal;
        result.emissive = resolvedEmissive;
        result.occlusion = occlusion;
        return result;
    }
};
