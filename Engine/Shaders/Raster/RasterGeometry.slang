import Common.All;
import Common.Material;

module RasterGeometry;

public struct RasterFrameViewResources {
    public ConstantBuffer<FrameData>       frame;
    public ConstantBuffer<ViewData>        view;
    public StructuredBuffer<float4>        lights;
    public SamplerState                    linearSampler;
};

public ParameterBlock<RasterFrameViewResources> g_rasterFrameView;

public struct RasterDrawResources {
    public StructuredBuffer<InstanceRecord>   instances;
    public GeometryRecordTable                geometryTable;
    public StructuredBuffer<MaterialRecord>   materials;
};

public ParameterBlock<RasterDrawResources> g_rasterDraw;

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
    float4 tangent;
    float2 uv;
    nointerpolation uint instanceIndex;
};

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID, uint startInstance : SV_StartInstanceLocation) {
    VSOutput output;
    uint instanceIndex = instanceID + startInstance;
    InstanceRecord instanceData = g_rasterDraw.instances[instanceIndex];
    VertexInfo vtx = g_rasterDraw.geometryTable.pullVertex(instanceData.geometryID, vertexID);
    float4 worldPosition = mul(float4(vtx.position, 1.0), instanceData.worldTransform);
    float3x3 worldTransform = (float3x3)instanceData.worldTransform;
    output.worldPosition = worldPosition.xyz;
    output.normal = transformNormalToWorld(vtx.normal, worldTransform);
    output.tangent = float4(
        normalize(mul(vtx.tangent.xyz, worldTransform)),
        vtx.tangent.w * getTransformOrientation(worldTransform));
    output.uv = vtx.uv;
    output.instanceIndex = instanceIndex;
    output.position = mul(worldPosition, g_rasterFrameView.view.viewProjection);
    return output;
}

[shader("fragment")]
GBufferPixel fragMain(VSOutput input) {
    InstanceRecord instanceData = g_rasterDraw.instances[input.instanceIndex];
    MaterialSurface surface = g_rasterDraw.materials[instanceData.materialID].resolveSurface(
        input.uv,
        input.normal,
        input.tangent,
        g_rasterFrameView.linearSampler);
    return GBufferPixel(
        float4(surface.baseColor, 1.0),
        surface.normal,
        instanceData.materialID,
        instanceData.entityID);
}
