implementing Common;

/// @brief GPU geometry ABI matching C++ GeometryRecord::GpuData.
public struct GeometryRecord {
    public float4  boundingSphere;
    public uint64_t positionAddress;
    public uint64_t normalAddress;
    public uint64_t tangentAddress;
    public uint64_t texCoordAddress;
    public uint64_t indexAddress;
};

/// @brief Vertex attributes pulled from a geometry record.
public struct VertexInfo {
    public float3 position;
    public float3 normal;
    public float4 tangent;
    public float2 uv;
};

/// @brief Local-space bounding sphere deconstructed from GeometryRecord::boundingSphere.
public struct BoundingSphere {
    public float3 center;
    public float  radius;
};

/// @brief OOP facade over the per-pass geometry record table.
/// Fully encapsulates the GeometryRecord ABI: callers pull decoded vertex
/// data or bounding spheres by record index and never see raw GPU addresses.
/// Replaces the former free-function vertex-pulling helpers.
public struct GeometryRecordTable {
    private StructuredBuffer<GeometryRecord> records;

    /// @brief Pull one vertex. vertexIndex is a primitive-vertex index and is
    /// always resolved through the record's index buffer.
    public VertexInfo pullVertex(uint recordIndex, uint vertexIndex) {
        GeometryRecord geometry = records[recordIndex];
        VertexInfo result;
        uint localVertexIndex = loadUint32(geometry.indexAddress + uint64_t(vertexIndex) * uint64_t(4));
        result.position = loadFloat3(geometry.positionAddress + uint64_t(localVertexIndex) * uint64_t(12));
        result.normal   = loadFloat3(geometry.normalAddress + uint64_t(localVertexIndex) * uint64_t(12));
        result.tangent  = float4(1.0, 0.0, 0.0, 1.0);
        if (geometry.tangentAddress != 0)
            result.tangent = ((float4*)geometry.tangentAddress)[localVertexIndex];
        result.uv = float2(0.0, 0.0);
        if (geometry.texCoordAddress != 0)
            result.uv = ((float2*)geometry.texCoordAddress)[localVertexIndex];
        return result;
    }

    /// @brief Pull the local-space bounding sphere of one record.
    public BoundingSphere pullBoundingSphere(uint recordIndex) {
        float4 bounds = records[recordIndex].boundingSphere;
        BoundingSphere result;
        result.center = bounds.xyz;
        result.radius = bounds.w;
        return result;
    }

    private static float3 loadFloat3(uint64_t address) {
        float* values = (float*)address;
        return float3(values[0], values[1], values[2]);
    }

    private static uint loadUint32(uint64_t address) {
        return ((uint*)address)[0];
    }
};
