implementing Common;

/// @brief GPU instance ABI matching C++ InstanceRecord::GpuData.
public struct InstanceRecord {
    public float4x4 worldTransform;
    public uint     materialID;
    public uint     entityID;
    public uint     geometryID;
};

/// @brief Shared normal transformation for row-vector world transforms.
public float getTransformOrientation(float3x3 transform) {
    float determinant = dot(transform[0], cross(transform[1], transform[2]));
    return determinant < 0.0 ? -1.0 : 1.0;
}

public float3 transformNormalToWorld(float3 normal, float3x3 worldTransform) {
    float3 cofactor0 = cross(worldTransform[1], worldTransform[2]);
    float3 cofactor1 = cross(worldTransform[2], worldTransform[0]);
    float3 cofactor2 = cross(worldTransform[0], worldTransform[1]);
    float3 transformed = normal.x * cofactor0 + normal.y * cofactor1 + normal.z * cofactor2;
    return normalize(transformed * getTransformOrientation(worldTransform));
}
