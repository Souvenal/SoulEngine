struct DispatchParams
{
    uint width;
    uint height;
};

[[vk::binding(0, 0)]] ConstantBuffer<DispatchParams>  g_params;
[[vk::binding(0, 0)]] RWStructuredBuffer<float4>       g_output;

groupshared float g_tile[16 * 16];

[numthreads(16, 16, 1)]
[shader("compute")]
void ComputeMain(uint3 dispatchThreadID : SV_DispatchThreadID,
                 uint3 groupThreadID   : SV_GroupThreadID,
                 uint3 groupID         : SV_GroupID)
{
    uint idx = groupThreadID.y * 16 + groupThreadID.x;
    g_tile[idx] = float(dispatchThreadID.x + dispatchThreadID.y);

    GroupMemoryBarrierWithGroupSync();

    float sum = 0.0;
    for (uint i = 0; i < 16 * 16; ++i)
        sum += g_tile[i];

    if (dispatchThreadID.x < g_params.width && dispatchThreadID.y < g_params.height)
    {
        uint2 coord = dispatchThreadID.xy;
        g_output[coord.y * g_params.width + coord.x] = float4(sum / (16.0 * 16.0), 0.0, 0.0, 1.0);
    }
}
