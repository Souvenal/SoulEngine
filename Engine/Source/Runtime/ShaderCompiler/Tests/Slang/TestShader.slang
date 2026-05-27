struct VSInput
{
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv;
};

struct SceneParams
{
    float4 tint;
};

[[vk::binding(0, 0)]] ConstantBuffer<SceneParams> g_scene;
[[vk::binding(0, 1)]] Texture2D<float4>           g_gradient;
[[vk::binding(0, 2)]] SamplerState                g_sampler;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput o;
    o.position = float4(input.position, 1.0);
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return g_gradient.Sample(g_sampler, input.uv) * g_scene.tint;
}
