struct FrameData
{
    float time;
};

struct InnerResources
{
    ConstantBuffer<FrameData> cb;
};

struct OuterResources
{
    ParameterBlock<InnerResources> inner;
};

ParameterBlock<OuterResources> g_outer;

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput VertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.position = float4(0, 0, 0, 1);
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return float4(g_outer.inner.cb.time, 0, 0, 1);
}
