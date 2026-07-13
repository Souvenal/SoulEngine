struct PositionOnly
{
    float3 position : POSITION;
};

struct FullInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv0      : TEXCOORD0;
    float2 uv1      : TEXCOORD1;
    uint4  boneIDs  : BLENDINDICES;
    float4 weights  : BLENDWEIGHT;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv;
};

struct SceneParams
{
    float4x4 viewProj;
};

[[vk::binding(0, 0)]] ConstantBuffer<SceneParams> g_scene;

[shader("vertex")]
VSOutput VertexMain_PositionOnly(PositionOnly input)
{
    VSOutput o;
    o.position = float4(input.position, 1.0);
    o.uv = float2(0, 0);
    return o;
}

[shader("vertex")]
VSOutput VertexMain_Full(FullInput input)
{
    VSOutput o;
    o.position = float4(input.position, 1.0);
    o.uv = input.uv0;
    return o;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return float4(input.uv, 0, 1);
}
