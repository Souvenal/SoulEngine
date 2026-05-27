struct Light
{
    float3 direction;
    float  intensity;
    float3 color;
    float  _pad0;
};

struct Material
{
    float3 albedo;
    float  roughness;
    float3 emissive;
    float  metalness;
};

struct SceneData
{
    float4x4 viewProj;
    float4x4 invView;
    float3   cameraPos;
    uint     lightCount;
};

[[vk::binding(0, 0)]] ConstantBuffer<SceneData>   g_scene;
[[vk::binding(0, 1)]] StructuredBuffer<Light>     g_lights;
[[vk::binding(1, 1)]] Texture2D<float4>           g_albedoMap;
[[vk::binding(2, 1)]] Texture2D<float4>           g_normalMap;
[[vk::binding(3, 1)]] Texture2D<float4>           g_roughnessMap;
[[vk::binding(0, 2)]] SamplerState                g_texSampler;
[[vk::binding(0, 0)]] RWTexture2D<float4>         g_renderTarget;

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPos;
    float3 worldNormal;
    float2 uv;
};

float3 evaluateLight(Light light, float3 pos, float3 normal, Material mat)
{
    float3 L = normalize(light.direction);
    float NdotL = max(dot(normal, L), 0.0);
    float3 diffuse = mat.albedo * light.color * light.intensity * NdotL;
    return diffuse;
}

[shader("fragment")]
float4 FragmentMain(PSInput input) : SV_Target
{
    Material mat;
    mat.albedo = g_albedoMap.Sample(g_texSampler, input.uv).rgb;
    mat.roughness = g_roughnessMap.Sample(g_texSampler, input.uv).r;
    mat.metalness = 0.0;
    mat.emissive = float3(0.0, 0.0, 0.0);

    float3 N = normalize(input.worldNormal);
    float3 result = float3(0.0, 0.0, 0.0);

    for (uint i = 0; i < g_scene.lightCount; ++i)
        result += evaluateLight(g_lights[i], input.worldPos, N, mat);

    g_renderTarget[uint2(input.position.xy)] = float4(result, 1.0);
    return float4(result, 1.0);
}
