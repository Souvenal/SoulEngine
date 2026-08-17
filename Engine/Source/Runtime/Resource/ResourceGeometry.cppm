/// @file   ResourceGeometry.cppm
/// @brief  Centralized geometry record management for GPU-driven rendering.
///
/// Replaces the old SubMesh + MeshGroup hierarchy with a unified GeometryRecord
/// system that directly imports from Assimp and provides span-based access.

module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <hlsl++.h>

export module Resource:Geometry;

export import Core;
export import RHI;
export import std;

export namespace SoulEngine {

/// @brief GPU-ready geometry record (CPU side).
///
/// Holds buffer refs for command recording and metadata for shader binding.
/// Replaces the old SubMesh + MeshGroup hierarchy.
struct GeometryRecord {
    String Name = {};  ///< Optional: record name for debugging

    // GPU buffer refs (for render command recording)
    RHIRef<RHIVertexBuffer> PositionBuffer = nullptr;
    RHIRef<RHIVertexBuffer> NormalBuffer   = nullptr;
    RHIRef<RHIVertexBuffer> TangentBuffer  = nullptr;
    RHIRef<RHIVertexBuffer> TexCoordBuffer = nullptr;
    RHIRef<RHIIndexBuffer>  IndexBuffer    = nullptr;

    // Metadata
    Uint32 IndexCount  = 0;
    Uint32 MaterialId  = 0;
    bool   HasUV0      = false;
    bool   HasTangents = false;

    // CPU-side vertex data (for GPU upload and collision/LOD)
    std::vector<hlslpp::interop::float3> Positions;
    std::vector<hlslpp::interop::float3> Normals;
    std::vector<hlslpp::interop::float4> Tangents;
    std::vector<hlslpp::interop::float2> UVs;
    std::vector<Uint32>                  Indices;
};

/// @brief Centralized geometry record manager.
///
/// Manages all geometry records imported from Assimp meshes.
/// Provides span-based access for renderer consumption.
/// Thread-safe for concurrent access.
class GeometryManager final : public Singleton<GeometryManager> {
    friend class Singleton<GeometryManager>;

public:
    /// @brief Register a geometry record from an Assimp mesh.
    ///
    /// If a mesh with the same file path already exists, the new record is appended.
    /// If import fails, logs a warning and continues.
    auto RegisterMesh(const aiMesh* aiMesh, const Path& meshDirectory, StringView meshFilePath) -> void
    {
        if (!aiMesh || !aiMesh->mNumVertices || !aiMesh->HasFaces()) {
            LogWarning("GeometryManager::RegisterMesh: invalid aiMesh");
            return;
        }

        auto recordResult = ImportMesh(aiMesh, meshDirectory);
        if (!recordResult) {
            LogWarning("Failed to import mesh '{}': {}", meshFilePath, recordResult.error().ToString());
            return;
        }

        std::scoped_lock lock(m_mutex);
        m_records[String(meshFilePath)].push_back(std::move(*recordResult));
    }

    /// @brief Register all meshes from an Assimp scene.
    ///
    /// Convenience method that registers all meshes in the scene.
    /// All meshes are stored under the same file path key.
    auto RegisterScene(const aiScene* scene, const Path& meshDirectory, StringView meshFilePath) -> void
    {
        if (!scene) {
            LogWarning("GeometryManager::RegisterScene: null aiScene");
            return;
        }

        for (Uint32 i = 0; i < scene->mNumMeshes; ++i) {
            RegisterMesh(scene->mMeshes[i], meshDirectory, meshFilePath);
        }
    }

    /// @brief Find geometry records by mesh file path.
    ///
    /// Returns a reference to the internal vector.
    /// The reference is valid as long as GeometryManager exists and the mesh is not cleared.
    [[nodiscard]] auto FindGeometryRecords(const String& meshFilePath) -> std::vector<GeometryRecord>
    {
        std::scoped_lock lock(m_mutex);
        
        return m_records[meshFilePath];
    }

    /// @brief Get global geometry ID for a specific record.
    ///
    /// Returns the index in the global geometry array for uploading to GPU.
    [[nodiscard]] auto GetGeometryID(StringView meshFilePath, size_t recordIndex) const -> Uint32
    {
        std::scoped_lock lock(m_mutex);
        
        // Count all records before this mesh
        Uint32 id = 0;
        for (const auto& [path, records] : m_records) {
            if (path == meshFilePath) {
                return id + static_cast<Uint32>(recordIndex);
            }
            id += static_cast<Uint32>(records.size());
        }
        return 0;  // Not found
    }

    /// @brief Get all geometry records as a flat array for GPU upload.
    [[nodiscard]] auto GetAllGeometryRecordsFlat() const -> std::vector<const GeometryRecord*>
    {
        std::scoped_lock lock(m_mutex);
        
        std::vector<const GeometryRecord*> result;
        for (const auto& [path, records] : m_records) {
            for (const auto& record : records) {
                result.push_back(&record);
            }
        }
        return result;
    }

    /// @brief Clear all geometry records.
    auto Clear() -> void
    {
        std::scoped_lock lock(m_mutex);
        m_records.clear();
    }

private:
    GeometryManager() = default;

    /// @brief Import a single Assimp mesh into a geometry record.
    ///
    /// Extracts CPU data and requests GPU buffers asynchronously.
    [[nodiscard]] auto ImportMesh(const aiMesh* aiMesh, const Path& meshDirectory)
        -> std::expected<GeometryRecord, ErrorMessage>
    {
        if (!aiMesh) {
            return std::unexpected(ErrorMessage("Null aiMesh"));
        }

        // Check if mesh has valid geometry
        const bool hasUV0 = aiMesh->mTextureCoords[0] != nullptr;
        const bool hasTangents = aiMesh->mTangents != nullptr && aiMesh->mBitangents != nullptr;

        // Prepare CPU-side data for GPU upload
        std::vector<hlslpp::interop::float3> positions;
        std::vector<hlslpp::interop::float3> normals;
        std::vector<hlslpp::interop::float4> tangents;
        std::vector<hlslpp::interop::float2> uvs;
        std::vector<Uint32> indices;

        positions.reserve(aiMesh->mNumVertices);
        normals.reserve(aiMesh->mNumVertices);
        if (hasTangents) tangents.reserve(aiMesh->mNumVertices);
        if (hasUV0) uvs.reserve(aiMesh->mNumVertices);

        // Extract vertex data
        for (Uint32 v = 0; v < aiMesh->mNumVertices; ++v) {
            const auto& pos = aiMesh->mVertices[v];
            positions.push_back(hlslpp::interop::float3{hlslpp::float3{pos.x, pos.y, pos.z}});

            const auto normal = aiMesh->mNormals ? aiMesh->mNormals[v] : aiVector3D{0, 1, 0};
            normals.push_back(hlslpp::interop::float3{hlslpp::float3{normal.x, normal.y, normal.z}});

            if (hasTangents) {
                const auto& tangent = aiMesh->mTangents[v];
                float handedness = 1.0f;
                if (aiMesh->mBitangents) {
                    const auto cross = normal ^ tangent;
                    handedness = (cross * aiMesh->mBitangents[v]) < 0.0f ? -1.0f : 1.0f;
                }
                tangents.push_back(hlslpp::interop::float4{hlslpp::float4{tangent.x, tangent.y, tangent.z, handedness}});
            }

            if (hasUV0) {
                const auto& uv = aiMesh->mTextureCoords[0][v];
                uvs.push_back(hlslpp::interop::float2{hlslpp::float2{uv.x, uv.y}});
            }
        }

        // Extract index data
        indices.reserve(aiMesh->mNumFaces * 3);
        for (Uint32 f = 0; f < aiMesh->mNumFaces; ++f) {
            const auto& face = aiMesh->mFaces[f];
            if (face.mNumIndices != 3) {
                return std::unexpected(ErrorMessage("Non-triangle face detected"));
            }
            indices.insert(indices.end(), face.mIndices, face.mIndices + 3);
        }

        // Request GPU buffers asynchronously (only when data exists)
        auto& device = RHIRenderDevice::Get();
        
        auto positionBuffer = device.CreateVertexBuffer({
            .Data = std::as_bytes(std::span{positions}),
            .VertexCount = positions.size(),
            .Stride = sizeof(hlslpp::interop::float3)
        });
        if (!positionBuffer) {
            return std::unexpected(positionBuffer.error().Append("Failed to create position buffer"));
        }

        auto normalBuffer = device.CreateVertexBuffer({
            .Data = std::as_bytes(std::span{normals}),
            .VertexCount = normals.size(),
            .Stride = sizeof(hlslpp::interop::float3)
        });
        if (!normalBuffer) {
            return std::unexpected(normalBuffer.error().Append("Failed to create normal buffer"));
        }

        // Only create tangent buffer if data exists
        RHIRef<RHIVertexBuffer> tangentBuffer = nullptr;
        if (hasTangents && !tangents.empty()) {
            auto tangentResult = device.CreateVertexBuffer({
                .Data = std::as_bytes(std::span{tangents}),
                .VertexCount = tangents.size(),
                .Stride = sizeof(hlslpp::interop::float4)
            });
            if (!tangentResult) {
                return std::unexpected(tangentResult.error().Append("Failed to create tangent buffer"));
            }
            tangentBuffer = std::move(*tangentResult);
        }

        // Only create texCoord buffer if data exists
        RHIRef<RHIVertexBuffer> texCoordBuffer = nullptr;
        if (hasUV0 && !uvs.empty()) {
            auto texCoordResult = device.CreateVertexBuffer({
                .Data = std::as_bytes(std::span{uvs}),
                .VertexCount = uvs.size(),
                .Stride = sizeof(hlslpp::interop::float2)
            });
            if (!texCoordResult) {
                return std::unexpected(texCoordResult.error().Append("Failed to create texCoord buffer"));
            }
            texCoordBuffer = std::move(*texCoordResult);
        }

        auto indexBuffer = device.CreateIndexBuffer({
            .Data = std::as_bytes(std::span{indices}),
            .IndexCount = indices.size()
        });
        if (!indexBuffer) {
            return std::unexpected(indexBuffer.error().Append("Failed to create index buffer"));
        }

        // Create GeometryRecord with CPU data and GPU buffer refs
        GeometryRecord record{
            .Name = String(aiMesh->mName.C_Str()),
            .PositionBuffer = std::move(*positionBuffer),
            .NormalBuffer = std::move(*normalBuffer),
            .TangentBuffer = std::move(tangentBuffer),
            .IndexBuffer = std::move(*indexBuffer),
            .IndexCount = static_cast<Uint32>(indices.size()),
            .MaterialId = aiMesh->mMaterialIndex,
            .HasUV0 = hasUV0,
            .HasTangents = hasTangents,
            .Positions = std::move(positions),
            .Normals = std::move(normals),
            .Tangents = std::move(tangents),
            .UVs = std::move(uvs),
            .Indices = std::move(indices),
        };
        
        // Set optional texCoord buffer
        if (texCoordBuffer) {
            record.TexCoordBuffer = std::move(texCoordBuffer);
        }
        
        return record;
    }

    mutable std::mutex m_mutex;
    std::unordered_map<String, std::vector<GeometryRecord>> m_records;  ///< Mesh file path -> geometry records
};

} // namespace SoulEngine
