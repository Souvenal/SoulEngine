module CullingCompute;

import Common.All;

/// @brief GPU indirect-draw command ABI matching C++ RasterIndirectCommand.
public struct RasterIndirectCommand {
    public uint vertexCount;
    public uint instanceCount;
    public uint firstVertex;
    public uint firstInstance;
};

/// @brief Culling pass I/O contract.
/// Current implementation copies scene instances and indirect commands one-to-one.
/// Future: frustum-culls into viewInstances via viewCounter atomic append and
/// regenerates viewIndirectCommands grouped by geometryID on the GPU.
public struct CullPassResources {
    public StructuredBuffer<InstanceRecord>        sceneInstances;
    public StructuredBuffer<RasterIndirectCommand> sceneIndirectCommands;
    public GeometryRecordTable                   geometryTable;
    public RWStructuredBuffer<InstanceRecord>        viewInstances;
    public RWStructuredBuffer<RasterIndirectCommand> viewIndirectCommands;
    public RWStructuredBuffer<uint>                  viewCounter;
};

public ParameterBlock<CullPassResources> g_cullPass;

public struct CullPushConstants {
    public uint instanceCount;
    public uint commandCount;
};



[shader("compute")]
[numthreads(64, 1, 1)]
void cullMain(uint3 dtid : SV_DispatchThreadID, uniform CullPushConstants push) {
    uint index = dtid.x;
    if (index < push.instanceCount)
        g_cullPass.viewInstances[index] = g_cullPass.sceneInstances[index];
    if (index < push.commandCount)
        g_cullPass.viewIndirectCommands[index] = g_cullPass.sceneIndirectCommands[index];

    if (index == 0) {
        g_cullPass.viewCounter[0] = push.instanceCount;
    }
}
