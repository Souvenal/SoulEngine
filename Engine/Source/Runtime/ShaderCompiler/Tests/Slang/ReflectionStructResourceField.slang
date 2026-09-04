struct Record
{
    float4 bounds;
};

// Resource facade mirroring the runtime GeometryRecordTable pattern: the
// descriptor lives on a private struct member, callers use accessor methods.
struct RecordTable
{
    private StructuredBuffer<Record> records;

    float4 pullBounds(uint index)
    {
        return records[index].bounds;
    }
};

struct DrawResources
{
    StructuredBuffer<float4> instances;
    RecordTable              geometryTable;
    StructuredBuffer<float4> materials;
};

ParameterBlock<DrawResources> g_draw;

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput VertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.position = g_draw.instances[vertexID] + g_draw.geometryTable.pullBounds(vertexID) +
        g_draw.materials[vertexID];
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    return input.position;
}
