struct SceneData
{
    float4 color;
};

[[vk::binding(3, 2)]] ConstantBuffer<SceneData> g_scene;

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput VertexMain(uint vid : SV_VertexID)
{
    VSOutput o;
    o.position = float4(0, 0, 0, 1);
    return o;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return g_scene.color;
}
