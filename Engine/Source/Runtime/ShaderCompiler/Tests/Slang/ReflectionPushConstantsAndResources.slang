struct PushData
{
    float4 color;
    float  intensity;
};

[[vk::binding(0, 0)]] ConstantBuffer<float4> g_cb;
[[vk::binding(1, 0)]] Texture2D<float4>      g_tex;
[[vk::binding(2, 0)]] SamplerState           g_sam;

struct VSOutput
{
    float4 position : SV_Position;
    float4 color;
};

[shader("vertex")]
VSOutput VertexMain(uint vid : SV_VertexID, uniform PushData push)
{
    VSOutput o;
    o.position = float4(0, 0, 0, 1) + push.color * g_cb[0];
    o.color = g_tex.Sample(g_sam, float2(0, 0));
    return o;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return input.color;
}
