export module Renderer:MaterialResolver;

import Core;
import Material;
import RHI;
import Resource;

export import std;

export namespace SoulEngine {

class PbrMaterialResolver {
  public:
    auto BeginFrame() -> void {
        m_Materials.clear();
        m_TextureTable.clear();
        m_TextureRefs.clear();
        m_TextureIndices.clear();
    }

    auto Clear() -> void {
        BeginFrame();
        m_TextureCache.clear();
    }

    [[nodiscard]] auto Resolve(const PbrMetallicRoughnessMaterial& Material, bool HasUV0, bool HasTangents) -> Uint32 {
        const PbrMaterialTextureIndices Indices{
            .BaseColor         = ResolveTexture(Material.BaseColorTexture, HasUV0),
            .Normal            = ResolveTexture(Material.NormalTexture, HasUV0 && HasTangents),
            .MetallicRoughness = ResolveTexture(Material.MetallicRoughnessTexture, HasUV0),
            .Metallic          = ResolveTexture(Material.MetallicTexture, HasUV0),
            .Roughness         = ResolveTexture(Material.RoughnessTexture, HasUV0),
            .Occlusion         = ResolveTexture(Material.OcclusionTexture, HasUV0),
            .Emissive          = ResolveTexture(Material.EmissiveTexture, HasUV0),
        };
        const auto MaterialIndex = static_cast<Uint32>(m_Materials.size());
        m_Materials.emplace_back(BuildPbrMaterialGpuData(Material, Indices));
        return MaterialIndex;
    }

    [[nodiscard]] auto GetMaterials() const -> std::span<const PbrMaterialGpuData> {
        return m_Materials;
    }

    [[nodiscard]] auto GetTextureRefs() const -> std::span<const RHIRef<RHISampledTexture>> {
        return m_TextureRefs;
    }

    [[nodiscard]] auto BuildTextureArray() const -> RHIResourceArray<RHISampledTexture> {
        RHIResourceArray<RHISampledTexture> Textures = {};
        if (m_TextureTable.empty()) {
            Textures.Set(0, nullptr);
            return Textures;
        }
        for (std::size_t Index = 0; Index < m_TextureTable.size(); ++Index)
            Textures.Set(static_cast<Uint32>(Index), m_TextureTable[Index]);
        return Textures;
    }

  private:
    struct TextureCacheEntry {
        String                    Asset   = {};
        RHIRef<RHISampledTexture> Texture = nullptr;
    };

    [[nodiscard]] auto GetOrRequestTexture(StringView Asset) -> RHIRef<RHISampledTexture>& {
        for (auto& Entry : m_TextureCache) {
            if (Entry.Asset == Asset)
                return Entry.Texture;
        }
        auto& Entry = m_TextureCache.emplace_back(TextureCacheEntry{
            .Asset = String(Asset),
        });
        if (auto Request = SubmitSampledTexturePreparation(
                Asset,
                [this, Asset = String(Asset)](RHIRef<RHISampledTexture> Texture) {
                    for (auto& Pending : m_TextureCache) {
                        if (Pending.Asset == Asset) {
                            Pending.Texture = std::move(Texture);
                            return;
                        }
                    }
                });
            !Request) {
            LogError("Failed to start sampled texture preparation: {}", Request.error().ToString());
        }
        return Entry.Texture;
    }

    [[nodiscard]] auto ResolveTexture(const String& Asset, bool Enabled) -> Int32 {
        if (!Enabled || Asset.empty())
            return -1;
        if (const auto Existing = m_TextureIndices.find(Asset); Existing != m_TextureIndices.end())
            return Existing->second;
        auto  TextureRef = GetOrRequestTexture(Asset);
        auto* Texture    = TextureRef.TryGet();
        if (!Texture || m_TextureTable.size() >= static_cast<std::size_t>(std::numeric_limits<Int32>::max()))
            return -1;
        const auto Index = static_cast<Int32>(m_TextureTable.size());
        m_TextureIndices.emplace(Asset, Index);
        m_TextureTable.emplace_back(Texture);
        m_TextureRefs.emplace_back(std::move(TextureRef));
        return Index;
    }

    std::vector<PbrMaterialGpuData>        m_Materials      = {};
    std::vector<RHISampledTexture*>        m_TextureTable   = {};
    std::vector<RHIRef<RHISampledTexture>> m_TextureRefs    = {};
    std::map<String, Int32, std::less<>>   m_TextureIndices = {};
    std::vector<TextureCacheEntry>         m_TextureCache   = {};
};

} // namespace SoulEngine
