struct VSInput
{
    [[vk::location(3)]] float3 pos : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(5)]] float4 col : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv;
};

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput o;
    o.position = float4(input.pos, 1.0);
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return float4(input.uv, 0, 1);
}
