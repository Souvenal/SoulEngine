struct FrameData
{
    float time;
};

struct FixedArrayResources
{
    ConstantBuffer<FrameData> frame;
    SamplerState uSampler;
    Texture2D<float4> uFixedTextures[4];
};

ParameterBlock<FixedArrayResources> g_params;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv;
};

[shader("vertex")]
VSOutput VertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.position = float4(0, 0, 0, 1);
    output.uv = float2(0, 0);
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return g_params.uFixedTextures[3].Sample(g_params.uSampler, input.uv);
}
