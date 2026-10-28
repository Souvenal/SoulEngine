import Common.All;

module VertexPullingLocalIndexConsumer;

public struct VertexPullingConsumerResources {
    public GeometryRecordTable geometryTable;
}

public ParameterBlock<VertexPullingConsumerResources> g_vertexPullingConsumer;

[shader("vertex")]
float4 vertMain(uint vertexID : SV_VertexID) : SV_Position {
    VertexInfo vertex = g_vertexPullingConsumer.geometryTable.pullVertexAttributes(0, vertexID);
    return float4(vertex.position, 1.0);
}

[shader("fragment")]
float4 fragMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
