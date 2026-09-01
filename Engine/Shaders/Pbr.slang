import Material;
import Common;

module Pbr;

public static const float kPbrPi = 3.14159265359;

public struct PbrBsdfSample {
    public float3 direction;
    public float3 value;
    public float  pdf;
};

public float saturateRoughness(float roughness) {
    return clamp(roughness, 0.04, 1.0);
}
public float distributionGGX(float nDotH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(kPbrPi * denominator * denominator, 0.0001);
}

public float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

public float geometrySmith(float nDotV, float nDotL, float roughness) {
    return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
}

public float3 fresnelSchlick(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

public float3 evaluatePbrBrdf(MaterialSurface material, float3 normal, float3 viewDirection, float3 lightDirection) {
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotV <= 0.0 || nDotL <= 0.0)
        return float3(0.0, 0.0, 0.0);

    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.baseColor, material.metallic);
    float3 fresnel = fresnelSchlick(vDotH, f0);
    float distribution = distributionGGX(nDotH, material.roughness);
    float geometry = geometrySmith(nDotV, nDotL, material.roughness);
    float3 specular = distribution * geometry * fresnel / max(4.0 * nDotV * nDotL, 0.0001);
    float3 diffuse = (1.0 - fresnel) * (1.0 - material.metallic) * material.baseColor / kPbrPi;
    return diffuse + specular;
}

public float getPbrSpecularSamplingProbability(MaterialSurface material) {
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.baseColor, material.metallic);
    float luminance = dot(f0, float3(0.2126, 0.7152, 0.0722));
    return clamp(luminance + material.metallic * 0.5, 0.05, 0.95);
}

public float getPbrBsdfPdf(MaterialSurface material, float3 normal, float3 viewDirection, float3 lightDirection) {
    float nDotL = saturate(dot(normal, lightDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    if (nDotL <= 0.0 || nDotV <= 0.0)
        return 0.0;

    float diffusePdf = nDotL / kPbrPi;
    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float halfPdf = distributionGGX(nDotH, material.roughness) * nDotH;
    float specularPdf = halfPdf / max(4.0 * vDotH, 0.0001);
    float specularProbability = getPbrSpecularSamplingProbability(material);
    return lerp(diffusePdf, specularPdf, specularProbability);
}

public void buildPbrBasis(float3 normal, out float3 tangent, out float3 bitangent) {
    float signValue = normal.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (signValue + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(1.0 + signValue * normal.x * normal.x * a, signValue * b, -signValue * normal.x);
    bitangent = float3(b, signValue + normal.y * normal.y * a, -normal.y);
}

public PbrBsdfSample samplePbrBsdf(MaterialSurface material,
                                   float3 normal,
                                   float3 viewDirection,
                                   float lobeSample,
                                   float2 directionSample) {
    PbrBsdfSample result = {};
    float3 tangent;
    float3 bitangent;
    buildPbrBasis(normal, tangent, bitangent);
    float specularProbability = getPbrSpecularSamplingProbability(material);

    if (lobeSample < specularProbability) {
        float alpha = material.roughness * material.roughness;
        float alpha2 = alpha * alpha;
        float phi = 2.0 * kPbrPi * directionSample.x;
        float cosTheta = sqrt((1.0 - directionSample.y) / max(1.0 + (alpha2 - 1.0) * directionSample.y, 0.0001));
        float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
        float3 halfVector = normalize(tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta) + normal * cosTheta);
        result.direction = normalize(reflect(-viewDirection, halfVector));
    } else {
        float radius = sqrt(directionSample.x);
        float phi = 2.0 * kPbrPi * directionSample.y;
        float3 localDirection = float3(radius * cos(phi), radius * sin(phi), sqrt(max(1.0 - directionSample.x, 0.0)));
        result.direction = normalize(tangent * localDirection.x + bitangent * localDirection.y + normal * localDirection.z);
    }

    result.value = evaluatePbrBrdf(material, normal, viewDirection, result.direction);
    result.pdf = getPbrBsdfPdf(material, normal, viewDirection, result.direction);
    return result;
}
