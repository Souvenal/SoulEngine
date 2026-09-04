import Common.All;

module SelectionFilter;

/// @brief GPU indirect-draw command ABI matching the raster renderer.
public struct RasterIndirectCommand {
    public uint vertexCount;
    public uint instanceCount;
    public uint firstVertex;
    public uint firstInstance;
};

public struct SelectionFilterResources {
    public StructuredBuffer<InstanceRecord> sceneInstances;
    public StructuredBuffer<RasterIndirectCommand> sceneIndirectCommands;
    public RWStructuredBuffer<InstanceRecord> selectedInstances;
    public RWStructuredBuffer<RasterIndirectCommand> selectedIndirectCommands;
    public RWStructuredBuffer<uint> commandCounters;
};

public ParameterBlock<SelectionFilterResources> g_selectionFilter;

public struct SelectionFilterPushConstants {
    public uint selectedEntityId;
};

/// @brief Filters one geometry group's instances for the selected entity.
[shader("compute")]
[numthreads(64, 1, 1)]
void filterMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uniform SelectionFilterPushConstants push) {
    uint commandIndex = groupId.x;
    uint lane = threadId.x;
    RasterIndirectCommand sourceCommand =
        g_selectionFilter.sceneIndirectCommands[commandIndex];

    for (uint localIndex = lane; localIndex < sourceCommand.instanceCount; localIndex += 64) {
        uint sourceIndex = sourceCommand.firstInstance + localIndex;
        InstanceRecord instanceData = g_selectionFilter.sceneInstances[sourceIndex];
        if (instanceData.entityID != push.selectedEntityId) {
            continue;
        }

        uint outputLocalIndex = 0;
        InterlockedAdd(g_selectionFilter.commandCounters[commandIndex], 1, outputLocalIndex);
        g_selectionFilter.selectedInstances[sourceCommand.firstInstance + outputLocalIndex] = instanceData;
    }

    GroupMemoryBarrierWithGroupSync();
    if (lane == 0) {
        RasterIndirectCommand selectedCommand = sourceCommand;
        selectedCommand.instanceCount = g_selectionFilter.commandCounters[commandIndex];
        g_selectionFilter.selectedIndirectCommands[commandIndex] = selectedCommand;
    }
}
