struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput VertexMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOutput o;
    o.position = float4(0, 0, 0, 1);
    return o;
}

[shader("fragment")]
float4 FragmentMain() : SV_Target
{
    return float4(1, 0, 0, 1);
}
