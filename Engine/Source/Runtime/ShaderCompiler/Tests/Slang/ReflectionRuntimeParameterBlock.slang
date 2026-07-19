struct FrameData
{
    float time;
};

struct ViewData
{
    float4x4 viewProjection;
};

struct FrameViewResources
{
    ConstantBuffer<FrameData> frame;
    ConstantBuffer<ViewData> view;
};

ParameterBlock<FrameViewResources> g_frameView;

struct SamplerResources
{
    SamplerState uSamplerLinear;
    SamplerState uSamplerAniso;
};

ParameterBlock<SamplerResources> g_samplers;

struct TextureResources
{
    Texture2D<float4> uTextures[];
};

ParameterBlock<TextureResources> g_textures;

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
    float pulse = 0.5 + 0.5 * sin(g_frameView.frame.time);
    return g_textures.uTextures[0].Sample(g_samplers.uSamplerLinear, input.uv) * pulse;
}
