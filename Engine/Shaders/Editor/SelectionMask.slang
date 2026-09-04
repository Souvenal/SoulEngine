import Common.All;

module SelectionMask;

public struct SelectionMaskFrameViewResources {
    public ConstantBuffer<FrameData>       frame;
    public ConstantBuffer<ViewData>        view;
};

public ParameterBlock<SelectionMaskFrameViewResources> g_selectionMaskFrameView;

public struct SelectionMaskDrawResources {
    public StructuredBuffer<InstanceRecord>   instances;
    public GeometryRecordTable                geometryTable;
};

public ParameterBlock<SelectionMaskDrawResources> g_selectionMaskDraw;

struct VSOutput {
    float4 position : SV_Position;
    nointerpolation uint instanceIndex;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID, uint startInstance : SV_StartInstanceLocation) {
    VSOutput output;
    uint instanceIndex = instanceID + startInstance;
    InstanceRecord instanceData = g_selectionMaskDraw.instances[instanceIndex];
    VertexInfo vtx = g_selectionMaskDraw.geometryTable.pullVertex(instanceData.geometryID, vertexID);
    float4 worldPosition = mul(float4(vtx.position, 1.0), instanceData.worldTransform);
    output.position = mul(worldPosition, g_selectionMaskFrameView.view.viewProjection);
    output.instanceIndex = instanceIndex;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
