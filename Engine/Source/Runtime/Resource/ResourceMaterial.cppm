module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <hlsl++.h>

export module Resource:Material;

import magic_enum;
import std;

export import Core;
import Material;
import RHI;
import :Texture;

export namespace SoulEngine {

/// One registered material instance: a stable ID plus its value.
struct MaterialEntry {
    Uint32      Id    = 0;
    String      Name  = {};
    PbrMaterial Value = {};
};

/// 材质导入相关函数

/// 从 aiMaterial 中读取颜色值
[[nodiscard]] auto
ReadColor(const aiMaterial& Material, const char* Key, Uint32 Type, Uint32 Index, hlslpp::float3 Fallback)
    -> hlslpp::float3 {
    aiColor4D Value = {};
    if (Material.Get(Key, Type, Index, Value) != AI_SUCCESS)
        return Fallback;
    return hlslpp::float3{Value.r, Value.g, Value.b};
}

/// 从 aiMaterial 中读取浮点值
[[nodiscard]] auto ReadFloat(const aiMaterial& Material, const char* Key, Uint32 Type, Uint32 Index, float Fallback)
    -> float {
    float Value = Fallback;
    return Material.Get(Key, Type, Index, Value) == AI_SUCCESS ? Value : Fallback;
}

/// 从 aiMaterial 中获取外部纹理路径
[[nodiscard]] auto GetExternalTexturePath(const aiMaterial& Material, aiTextureType Type, const Path& MeshDirectory)
    -> String {
    const auto TextureCount = Material.GetTextureCount(Type);
    if (TextureCount == 0)
        return {};
    if (TextureCount != 1) {
        LogWarning("Assimp material {} texture disabled: layered texture stacks are not supported",
                   magic_enum::enum_name(Type));
        return {};
    }

    aiString         TexturePath = {};
    aiTextureMapping Mapping     = aiTextureMapping_UV;
    Uint32           UVIndex     = 0;
    if (Material.GetTexture(Type, 0, &TexturePath, &Mapping, &UVIndex) != AI_SUCCESS) {
        LogWarning("Assimp material {} texture disabled: failed to read texture binding", magic_enum::enum_name(Type));
        return {};
    }
    if (Mapping != aiTextureMapping_UV || UVIndex != 0) {
        LogWarning("Assimp material {} texture disabled: only UV0 mapping is supported", magic_enum::enum_name(Type));
        return {};
    }

    const Path SourcePath = TexturePath.C_Str();
    if (TexturePath.length == 0 || TexturePath.C_Str()[0] == '*') {
        LogWarning("Assimp material {} texture disabled: embedded textures are not supported",
                   magic_enum::enum_name(Type));
        return {};
    }
    if (SourcePath.is_absolute()) {
        LogWarning("Assimp material {} texture disabled: absolute paths are not supported",
                   magic_enum::enum_name(Type));
        return {};
    }

    const auto Normalized = (MeshDirectory / SourcePath).lexically_normal();
    if (Normalized.empty()) {
        LogWarning("Assimp material {} texture disabled: path could not be normalized", magic_enum::enum_name(Type));
        return {};
    }
    return Normalized.string();
}

/// 从 aiMaterial 导入 PbrMaterial
[[nodiscard]] auto ImportMaterial(const aiMaterial* AiMaterial, const Path& MeshDirectory) -> PbrMaterial {
    PbrMaterial Material = {};
    if (!AiMaterial)
        return Material;

    aiColor4D BaseColor = {};
    if (AiMaterial->Get(AI_MATKEY_BASE_COLOR, BaseColor) == AI_SUCCESS) {
        Material.BaseColor = hlslpp::float3{BaseColor.r, BaseColor.g, BaseColor.b};
    } else {
        Material.BaseColor = ReadColor(*AiMaterial, AI_MATKEY_COLOR_DIFFUSE, Material.BaseColor);
    }
    Material.Emissive  = ReadColor(*AiMaterial, AI_MATKEY_COLOR_EMISSIVE, hlslpp::float3{0.0f, 0.0f, 0.0f});
    Material.Metallic  = std::clamp(ReadFloat(*AiMaterial, AI_MATKEY_METALLIC_FACTOR, 0.0f), 0.0f, 1.0f);
    Material.Roughness = std::clamp(ReadFloat(*AiMaterial, AI_MATKEY_ROUGHNESS_FACTOR, Material.Roughness), 0.0f, 1.0f);

    Material.BaseColorTexture = GetExternalTexturePath(*AiMaterial, aiTextureType_BASE_COLOR, MeshDirectory);
    if (Material.BaseColorTexture.empty())
        Material.BaseColorTexture = GetExternalTexturePath(*AiMaterial, aiTextureType_DIFFUSE, MeshDirectory);
    Material.NormalTexture = GetExternalTexturePath(*AiMaterial, aiTextureType_NORMAL_CAMERA, MeshDirectory);
    if (Material.NormalTexture.empty())
        Material.NormalTexture = GetExternalTexturePath(*AiMaterial, aiTextureType_NORMALS, MeshDirectory);
    Material.MetallicTexture  = GetExternalTexturePath(*AiMaterial, aiTextureType_METALNESS, MeshDirectory);
    Material.RoughnessTexture = GetExternalTexturePath(*AiMaterial, aiTextureType_DIFFUSE_ROUGHNESS, MeshDirectory);
    Material.OcclusionTexture = GetExternalTexturePath(*AiMaterial, aiTextureType_AMBIENT_OCCLUSION, MeshDirectory);
    Material.EmissiveTexture  = GetExternalTexturePath(*AiMaterial, aiTextureType_EMISSION_COLOR, MeshDirectory);
    if (Material.EmissiveTexture.empty())
        Material.EmissiveTexture = GetExternalTexturePath(*AiMaterial, aiTextureType_EMISSIVE, MeshDirectory);

    if (!Material.MetallicTexture.empty() && Material.MetallicTexture == Material.RoughnessTexture) {
        Material.MetallicRoughnessTexture = Material.MetallicTexture;
        Material.MetallicTexture          = {};
        Material.RoughnessTexture         = {};
    }
    return Material;
}

/// Engine-wide, renderer-neutral material instance registry.
///
/// Manages material entries, texture array (bindless), and GPU material records.
/// Scene loading and mesh import register through RegisterMaterial().
class MaterialManager final : public Singleton<MaterialManager> {
    friend class Singleton<MaterialManager>;

  public:
    /// Name of the built-in default material, always registered at ID 0.
    static constexpr StringView DefaultMaterialName = "DefaultMaterial";

    /// Register or update one material instance. New IDs append; an existing ID
    /// is overwritten in place.
    [[nodiscard]] auto RegisterMaterial(String Name, const PbrMaterial& Material) -> Uint32 {
        std::scoped_lock Lock(m_Mutex);

        // Check if material with same name already exists
        for (auto& Entry : m_Entries) {
            if (Entry.Name == Name) {
                Entry.Value         = Material;
                m_Records[Entry.Id] = BuildMaterialRecord(Material);
                return Entry.Id;
            }
        }

        // Generate new ID
        const Uint32 NewId = static_cast<Uint32>(m_Entries.size());

        m_Entries.emplace_back(MaterialEntry{
            .Id    = NewId,
            .Name  = std::move(Name),
            .Value = Material,
        });
        m_Records.emplace_back(BuildMaterialRecord(Material));
        return NewId;
    }

    /// Register all materials from an Assimp scene.
    [[nodiscard]] auto RegisterMaterialsFromScene(const aiScene* Scene, const Path& MeshDirectory)
        -> std::vector<Uint32> {
        std::vector<Uint32> Result;
        if (!Scene)
            return Result;

        Result.reserve(Scene->mNumMaterials);
        for (Uint32 MaterialIndex = 0; MaterialIndex < Scene->mNumMaterials; ++MaterialIndex) {
            const auto* AiMaterial   = Scene->mMaterials[MaterialIndex];
            aiString    MaterialName = {};
            const bool  HasName =
                AiMaterial && AiMaterial->Get(AI_MATKEY_NAME, MaterialName) == AI_SUCCESS && MaterialName.length != 0;

            String      Name     = HasName ? String(MaterialName.C_Str()) : Format("Material{}", MaterialIndex);
            PbrMaterial Material = ImportMaterial(AiMaterial, MeshDirectory);

            Result.emplace_back(RegisterMaterial(std::move(Name), Material));
        }
        return Result;
    }

    /// Find a material ID by name. Returns 0 (DefaultMaterial) if not found.
    [[nodiscard]] auto FindMaterialId(StringView Name) -> Uint32 {
        std::scoped_lock Lock(m_Mutex);
        for (const auto& Entry : m_Entries) {
            if (Entry.Name == Name) {
                return Entry.Id;
            }
        }
        return 0;
    }

    [[nodiscard]] auto FindMaterialIdOptional(StringView Name) -> std::optional<Uint32> {
        std::scoped_lock Lock(m_Mutex);
        for (const auto& Entry : m_Entries) {
            if (Entry.Name == Name)
                return Entry.Id;
        }
        return std::nullopt;
    }

    /// Get a material by ID.
    [[nodiscard]] auto GetMaterial(Uint32 Id) -> const MaterialEntry* {
        std::scoped_lock Lock(m_Mutex);
        for (const auto& Entry : m_Entries) {
            if (Entry.Id == Id) {
                return &Entry;
            }
        }
        return nullptr;
    }

    /// Get all materials.
    [[nodiscard]] auto GetMaterials() -> std::vector<MaterialEntry> {
        std::scoped_lock Lock(m_Mutex);
        return m_Entries;
    }

    /// Get all material records for GPU upload.
    [[nodiscard]] auto GetMaterialRecords() -> std::span<const MaterialRecord> {
        std::scoped_lock Lock(m_Mutex);
        return m_Records;
    }

    /// Get material textures (bindless array) for shader binding.
    /// Returns a reference to the texture array.
    /// If the array is empty, returns a static empty array with a nullptr placeholder.
    [[nodiscard]] auto GetMaterialTextures() -> const RHIResourceArray<RHISampledTexture>& {
        std::scoped_lock Lock(m_Mutex);
        if (m_TextureArray.GetSize() == 0) {
            // Return a static array with a nullptr placeholder to avoid "missing non-empty resource array" errors
            static RHIResourceArray<RHISampledTexture> EmptyArray = [] {
                RHIResourceArray<RHISampledTexture> Arr;
                Arr.Set(0, nullptr);
                return Arr;
            }();
            return EmptyArray;
        }
        return m_TextureArray;
    }

    /// Built-in default PBR material value (used when no instance resolves).
    [[nodiscard]] static auto GetDefaultMaterial() -> const PbrMaterial& {
        static const PbrMaterial Default = {};
        return Default;
    }

    /// Drop all registered instances except the built-in Default. Scene
    /// replacement calls this before registering the new scene's instances.
    /// Test/shutdown helper too.
    auto Clear() -> void {
        std::scoped_lock Lock(m_Mutex);
        m_Entries.resize(1);
        m_Records.resize(1);
        m_TextureArray = {};
        m_TextureIndices.clear();
    }

  private:
    MaterialManager() {
        // Reserve the Default material at ID 0 so it is always available as a fallback.
        m_Entries.emplace_back(MaterialEntry{
            .Id    = 0,
            .Name  = String(DefaultMaterialName),
            .Value = PbrMaterial{},
        });
        m_Records.emplace_back(BuildMaterialRecord(PbrMaterial{}));
    }

    /// Build a MaterialRecord from PbrMaterial.
    [[nodiscard]] auto BuildMaterialRecord(const PbrMaterial& Material) -> MaterialRecord {
        MaterialRecord Record;
        Record.BaseColorFactor = hlslpp::interop::float4{
            hlslpp::float4{Material.BaseColor.x, Material.BaseColor.y, Material.BaseColor.z, 1.0f}};
        Record.EmissiveFactor =
            hlslpp::interop::float3{hlslpp::float3{Material.Emissive.x, Material.Emissive.y, Material.Emissive.z}};
        Record.MetallicFactor  = Material.Metallic;
        Record.RoughnessFactor = Material.Roughness;
        Record.OcclusionFactor = 1.0f;

        Record.BaseColorTexture         = ResolveTextureIndex(Material.BaseColorTexture);
        Record.NormalTexture            = ResolveTextureIndex(Material.NormalTexture);
        Record.MetallicRoughnessTexture = ResolveTextureIndex(Material.MetallicRoughnessTexture);
        Record.MetallicTexture          = ResolveTextureIndex(Material.MetallicTexture);
        Record.RoughnessTexture         = ResolveTextureIndex(Material.RoughnessTexture);
        Record.OcclusionTexture         = ResolveTextureIndex(Material.OcclusionTexture);
        Record.EmissiveTexture          = ResolveTextureIndex(Material.EmissiveTexture);

        return Record;
    }

    /// Resolve a texture path to a bindless texture array index.
    /// Returns -1 if path is empty or texture fails to load.
    [[nodiscard]] auto ResolveTextureIndex(const String& Path) -> Int32 {
        if (Path.empty())
            return -1;

        // Check cache
        if (auto It = m_TextureIndices.find(Path); It != m_TextureIndices.end())
            return It->second;

        // Load texture
        auto Result = RequestSampledTexture(SampledTextureRequest{.TexturePath = Path});
        if (!Result) {
            LogError("Failed to load texture '{}': {}", Path, Result.error().ToString());
            return -1;
        }

        // Add to texture array
        const Int32 Index = static_cast<Int32>(m_TextureArray.GetSize());
        m_TextureArray.Set(Index, Result->TryGet());
        m_TextureIndices[Path] = Index;

        return Index;
    }

    std::mutex                           m_Mutex          = {};
    std::vector<MaterialEntry>           m_Entries        = {};
    std::vector<MaterialRecord>          m_Records        = {};
    RHIResourceArray<RHISampledTexture>  m_TextureArray   = {};
    std::map<String, Int32, std::less<>> m_TextureIndices = {};
};

} // namespace SoulEngine
