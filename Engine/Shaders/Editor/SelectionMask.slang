import Common.All;
import Common.Material;

module SelectionMask;

public struct SelectionMaskFrameViewResources {
    public ConstantBuffer<FrameData>       frame;
    public ConstantBuffer<ViewData>        view;
    public SamplerState                    linearSampler;
};

public ParameterBlock<SelectionMaskFrameViewResources> g_selectionMaskFrameView;

public struct SelectionMaskDrawResources {
    public StructuredBuffer<InstanceRecord>   instances;
    public GeometryRecordTable                geometryTable;
    public StructuredBuffer<MaterialRecord>   materials;
};

public ParameterBlock<SelectionMaskDrawResources> g_selectionMaskDraw;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv;
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
    output.uv = vtx.uv;
    output.instanceIndex = instanceIndex;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0 {
    InstanceRecord instanceData = g_selectionMaskDraw.instances[input.instanceIndex];
    MaterialRecord material = g_selectionMaskDraw.materials[instanceData.materialID];
    if (!material.passesAlphaTest(input.uv, g_selectionMaskFrameView.linearSampler))
        discard;
    return float4(1.0, 1.0, 1.0, 1.0);
}
