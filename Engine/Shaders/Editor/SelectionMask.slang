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
    public StructuredBuffer<InstanceRecord> instances;
    public StructuredBuffer<MaterialRecord> materials;
};

public ParameterBlock<SelectionMaskDrawResources> g_selectionMaskDraw;

public struct SelectionMaskVertexPullingResources {
    public GeometryRecordTable geometryTable;
};

public ParameterBlock<SelectionMaskVertexPullingResources> g_selectionMaskPulling;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv;
    nointerpolation uint instanceIndex;
};

VSOutput buildSelectionVertexOutput(float3 position, float2 uv, uint instanceIndex) {
    VSOutput output;
    InstanceRecord instanceData = g_selectionMaskDraw.instances[instanceIndex];
    float4 worldPosition = mul(float4(position, 1.0), instanceData.worldTransform);
    output.position = mul(worldPosition, g_selectionMaskFrameView.view.viewProjection);
    output.uv = uv;
    output.instanceIndex = instanceIndex;
    return output;
}

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID, uint startInstance : SV_StartInstanceLocation) {
    uint instanceIndex = instanceID + startInstance;
    InstanceRecord instanceData = g_selectionMaskDraw.instances[instanceIndex];
    VertexInfo vtx = g_selectionMaskPulling.geometryTable.pullVertexAttributes(instanceData.geometryID, vertexID);
    return buildSelectionVertexOutput(vtx.position, vtx.uv, instanceIndex);
}

[shader("vertex")]
VSOutput vertVertexInputMain(
    float3 position : POSITION,
    float2 uv : TEXCOORD,
    uint instanceID : SV_InstanceID,
    uint startInstance : SV_StartInstanceLocation) {
    return buildSelectionVertexOutput(position, uv, instanceID + startInstance);
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0 {
    InstanceRecord instanceData = g_selectionMaskDraw.instances[input.instanceIndex];
    MaterialRecord material = g_selectionMaskDraw.materials[instanceData.materialID];
    if (!material.passesAlphaTest(input.uv, g_selectionMaskFrameView.linearSampler))
        discard;
    return float4(1.0, 1.0, 1.0, 1.0);
}
