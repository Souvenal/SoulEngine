#include <gtest/gtest.h>
#include <assimp/GltfMaterial.h>

import Material;

using namespace SoulEngine;

namespace {

class ScopedTemporaryFile final {
  public:
    explicit ScopedTemporaryFile(Path FilePath) : m_FilePath(std::move(FilePath)) {}

    ~ScopedTemporaryFile() {
        std::error_code Error = {};
        std::filesystem::remove(m_FilePath, Error);
    }

  private:
    Path m_FilePath = {};
};

[[nodiscard]] auto MakeTemporaryMaterialFilePath(std::error_code& Error) -> Path {
    static std::atomic<Uint32> NextFileId = 0;
    const auto Directory = std::filesystem::temp_directory_path(Error);
    if (Error)
        return {};
    return Directory / Format("soulengine_material_alpha_{}.yaml", NextFileId.fetch_add(1, std::memory_order_relaxed));
}

class MockSampledTexture final : public RHISampledTexture {
  public:
    explicit MockSampledTexture(String Name) : RHISampledTexture(std::move(Name)) {}

    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return 1;
    }

    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return 1;
    }
};

class MaterialGpuDataTest : public testing::Test {
  protected:
    auto SetUp() -> void override {
        GDeferredDeletionQueue = &m_DeletionQueue;
    }

    auto TearDown() -> void override {
        m_DeletionQueue.Drain();
        GDeferredDeletionQueue = nullptr;
    }

    [[nodiscard]] auto CreateTexture(StringView Name) -> RHIRef<RHISampledTexture> {
        auto Texture = RHIRef<RHISampledTexture>::Create();
        if (auto Published = Texture.Publish(std::make_unique<MockSampledTexture>(String(Name)), RHIRefState::Ready);
            !Published)
            return {};
        return Texture;
    }

    [[nodiscard]] auto CreateTextureData(RHIRef<RHISampledTexture> Texture) -> TextureDataHandle {
        return TextureDataHandle{std::make_shared<TextureData>(TextureData{
            .Texture = std::move(Texture),
        })};
    }

  private:
    RHIDeferredDeletionQueue m_DeletionQueue = {};
};

} // namespace

TEST(MaterialRecord, PreservesFlatTextureTypeOrder) {
    const MaterialRecord Material{
        .Textures = {
            TextureRecord{.Type = TextureType::BaseColor},
            TextureRecord{.Type = TextureType::Normals},
            TextureRecord{.Type = TextureType::BaseColor},
            TextureRecord{.Type = TextureType::Metalness},
        },
    };

    ASSERT_EQ(Material.Textures.size(), 4U);
    EXPECT_EQ(Material.Textures[0].Type, TextureType::BaseColor);
    EXPECT_EQ(Material.Textures[1].Type, TextureType::Normals);
    EXPECT_EQ(Material.Textures[2].Type, TextureType::BaseColor);
    EXPECT_EQ(Material.Textures[3].Type, TextureType::Metalness);
}

TEST(MaterialAssimpLoader, ImportsGltfAlphaModeAndCutoff) {
    struct AlphaModeCase {
        StringView        SourceValue;
        MaterialAlphaMode ExpectedMode;
        Float32           Cutoff;
    };
    constexpr std::array AlphaModeCases{
        AlphaModeCase{"OPAQUE", MaterialAlphaMode::Opaque, 0.25f},
        AlphaModeCase{"MASK", MaterialAlphaMode::Mask, 0.50f},
        AlphaModeCase{"BLEND", MaterialAlphaMode::Blend, 0.75f},
    };

    for (const auto& TestCase : AlphaModeCases) {
        aiMaterial Source = {};
        const aiString AlphaMode{std::string{TestCase.SourceValue}};
        ASSERT_EQ(Source.AddProperty(&AlphaMode, AI_MATKEY_GLTF_ALPHAMODE), AI_SUCCESS);
        ASSERT_EQ(Source.AddProperty(&TestCase.Cutoff, 1, AI_MATKEY_GLTF_ALPHACUTOFF), AI_SUCCESS);

        const auto Material = MaterialAssimpLoader{}(&Source, Path{"Assets/Test.gltf"});
        ASSERT_NE(Material, nullptr);
        EXPECT_EQ(Material->AlphaMode, TestCase.ExpectedMode);
        EXPECT_FLOAT_EQ(Material->AlphaCutoff, TestCase.Cutoff);
    }
}

TEST(MaterialAssimpLoader, DefaultsGltfAlphaModeAndCutoff) {
    const aiMaterial Source = {};

    const auto Material = MaterialAssimpLoader{}(&Source, Path{"Assets/Test.gltf"});

    ASSERT_NE(Material, nullptr);
    EXPECT_EQ(Material->AlphaMode, MaterialAlphaMode::Opaque);
    EXPECT_FLOAT_EQ(Material->AlphaCutoff, 0.5f);
}

TEST(MaterialYamlLoader, AuthorsGltfAlphaModeAndCutoff) {
    std::error_code Error = {};
    const auto FilePath = MakeTemporaryMaterialFilePath(Error);
    ASSERT_FALSE(Error) << Error.message();
    ScopedTemporaryFile Cleanup{FilePath};
    {
        std::ofstream Output(FilePath);
        ASSERT_TRUE(Output);
        Output << R"(
material:
  alpha_mode: bLeNd
  alpha_cutoff: 0.37
)";
    }

    const auto Material = MaterialYamlLoader{}(FilePath, FilePath.parent_path());

    ASSERT_NE(Material, nullptr);
    EXPECT_EQ(Material->AlphaMode, MaterialAlphaMode::Blend);
    EXPECT_FLOAT_EQ(Material->AlphaCutoff, 0.37f);
}

TEST_F(MaterialGpuDataTest, GltfMetallicRoughnessTakesPrecedence) {
    auto GltfTexture      = CreateTexture("Test/GltfMetallicRoughness");
    auto MetallicTexture  = CreateTexture("Test/Metalness");
    auto RoughnessTexture = CreateTexture("Test/Roughness");
    ASSERT_TRUE(GltfTexture);
    ASSERT_TRUE(MetallicTexture);
    ASSERT_TRUE(RoughnessTexture);

    RHIRefArray<RHISampledTexture> TextureArray;
    ASSERT_TRUE(TextureArray.Append(GltfTexture).has_value());
    ASSERT_TRUE(TextureArray.Append(MetallicTexture).has_value());
    ASSERT_TRUE(TextureArray.Append(RoughnessTexture).has_value());

    const MaterialRecord Material{
        .Textures = {
            TextureRecord{
                .Type = TextureType::GltfMetallicRoughness,
                .Texture = CreateTextureData(GltfTexture),
            },
            TextureRecord{
                .Type = TextureType::Metalness,
                .Texture = CreateTextureData(MetallicTexture),
            },
            TextureRecord{
                .Type = TextureType::DiffuseRoughness,
                .Texture = CreateTextureData(RoughnessTexture),
            },
        },
    };

    const auto GpuData = Material.BuildGpuData(TextureArray);
    const auto GltfHandle = std::array<Uint32, 2>{RHIRefArray<RHISampledTexture>::FirstResourceSlot, 0U};
    const auto NullHandle = std::array<Uint32, 2>{};
    EXPECT_EQ(GpuData.MetallicRoughnessTexture, GltfHandle);
    EXPECT_EQ(GpuData.MetallicTexture, NullHandle);
    EXPECT_EQ(GpuData.RoughnessTexture, NullHandle);
}

TEST_F(MaterialGpuDataTest, SharedMetalnessAndRoughnessUseCombinedHandle) {
    auto SharedTexture = CreateTexture("Test/MetallicRoughness");
    ASSERT_TRUE(SharedTexture);

    RHIRefArray<RHISampledTexture> TextureArray;
    ASSERT_TRUE(TextureArray.Append(SharedTexture).has_value());
    const auto SharedTextureData = CreateTextureData(SharedTexture);
    const MaterialRecord Material{
        .Textures = {
            TextureRecord{
                .Type = TextureType::Metalness,
                .Texture = SharedTextureData,
            },
            TextureRecord{
                .Type = TextureType::DiffuseRoughness,
                .Texture = SharedTextureData,
            },
        },
    };

    const auto GpuData = Material.BuildGpuData(TextureArray);
    const auto CombinedHandle = std::array<Uint32, 2>{RHIRefArray<RHISampledTexture>::FirstResourceSlot, 0U};
    const auto NullHandle = std::array<Uint32, 2>{};
    EXPECT_EQ(GpuData.MetallicRoughnessTexture, CombinedHandle);
    EXPECT_EQ(GpuData.MetallicTexture, NullHandle);
    EXPECT_EQ(GpuData.RoughnessTexture, NullHandle);
}

TEST_F(MaterialGpuDataTest, SeparateMetalnessAndRoughnessUseSeparateHandles) {
    auto MetallicTexture  = CreateTexture("Test/Metalness");
    auto RoughnessTexture = CreateTexture("Test/Roughness");
    ASSERT_TRUE(MetallicTexture);
    ASSERT_TRUE(RoughnessTexture);

    RHIRefArray<RHISampledTexture> TextureArray;
    ASSERT_TRUE(TextureArray.Append(MetallicTexture).has_value());
    ASSERT_TRUE(TextureArray.Append(RoughnessTexture).has_value());
    const MaterialRecord Material{
        .Textures = {
            TextureRecord{
                .Type = TextureType::Metalness,
                .Texture = CreateTextureData(MetallicTexture),
            },
            TextureRecord{
                .Type = TextureType::DiffuseRoughness,
                .Texture = CreateTextureData(RoughnessTexture),
            },
        },
    };

    const auto GpuData = Material.BuildGpuData(TextureArray);
    const auto MetallicHandle = std::array<Uint32, 2>{RHIRefArray<RHISampledTexture>::FirstResourceSlot, 0U};
    const auto RoughnessHandle = std::array<Uint32, 2>{RHIRefArray<RHISampledTexture>::FirstResourceSlot + 1U, 0U};
    const auto NullHandle = std::array<Uint32, 2>{};
    EXPECT_EQ(GpuData.MetallicRoughnessTexture, NullHandle);
    EXPECT_EQ(GpuData.MetallicTexture, MetallicHandle);
    EXPECT_EQ(GpuData.RoughnessTexture, RoughnessHandle);
}
