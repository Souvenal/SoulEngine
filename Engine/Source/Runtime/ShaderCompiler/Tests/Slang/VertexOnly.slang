struct SkyInput
{
    float3 position : POSITION;
};

struct SkyOutput
{
    float4 position : SV_Position;
    float3 worldDir;
};

[[vk::binding(0, 0)]] ConstantBuffer<float4x4> g_viewProj;

[shader("vertex")]
SkyOutput VertexMain(SkyInput input)
{
    SkyOutput o;
    float4 pos = mul(g_viewProj, float4(input.position, 1.0));
    pos.z = pos.w;
    o.position = pos;
    o.worldDir = input.position;
    return o;
}
