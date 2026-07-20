public struct FrameData {
    public float  time;
    public float4 directionalLightDirectionIntensity;
    public float4 directionalLightColor;
};

public struct ViewData {
    public float4x4 viewProjection;
    public float4   cameraPosition;
};

public struct FrameViewResources {
    public ConstantBuffer<FrameData> frame;
    public ConstantBuffer<ViewData>  view;
};

public ParameterBlock<FrameViewResources> g_forwardFrameView;

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

static const float kPi = 3.14159265359;

float distributionGGX(float3 normal, float3 halfVector, float roughness) {
    float alpha  = roughness * roughness;
    float alpha2 = alpha * alpha;
    float nDotH  = saturate(dot(normal, halfVector));
    float denom  = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(kPi * denom * denom, 0.0001);
}

float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness) {
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
}

float3 fresnelSchlick(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;
    output.worldPosition = input.inPosition;
    output.normal        = input.inNormal;
    output.position      = mul(float4(input.inPosition, 1.0), g_forwardFrameView.view.viewProjection);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET {
    float3 baseColor = g_forwardMaterial.material.baseColorFactor.rgb;
    float metallic = saturate(g_forwardMaterial.material.metallicFactor);
    float roughness = clamp(g_forwardMaterial.material.roughnessFactor, 0.04, 1.0);
    float3 normal = normalize(input.normal);

    float3 lightDirection = normalize(-g_forwardFrameView.frame.directionalLightDirectionIntensity.xyz);
    float3 radiance = g_forwardFrameView.frame.directionalLightColor.rgb *
                      g_forwardFrameView.frame.directionalLightDirectionIntensity.w;
    float nDotL = saturate(dot(normal, lightDirection));
    float3 viewDirection = normalize(g_forwardFrameView.view.cameraPosition.xyz - input.worldPosition);
    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotV = saturate(dot(normal, viewDirection));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
    float distribution = distributionGGX(normal, halfVector, roughness);
    float geometry = geometrySmith(normal, viewDirection, lightDirection, roughness);
    float3 fresnel = fresnelSchlick(saturate(dot(halfVector, viewDirection)), f0);
    float3 specular = distribution * geometry * fresnel / max(4.0 * nDotV * nDotL, 0.0001);
    float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * baseColor / kPi;

    float3 ambient = 0.035 * baseColor * (1.0 - metallic);
    float3 color = ambient + (diffuse + specular) * radiance * nDotL;
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, g_forwardMaterial.material.baseColorFactor.a);
}
