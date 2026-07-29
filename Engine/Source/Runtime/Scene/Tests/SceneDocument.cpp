#include <gtest/gtest.h>

// Required while Scene exposes entt::registry in its object layout. These tests create
// Scene values and therefore instantiate Scene lifetime operations.
#include <entt/entt.hpp>
#include <hlsl++.h>

import Scene;

using namespace SoulEngine;

namespace {

[[nodiscard]] auto WriteSceneFile(StringView Content) -> Path {
    const auto FilePath = std::filesystem::temp_directory_path() / "soulengine_scene_test.yaml";
    std::ofstream Output(FilePath);
    Output << Content;
    return FilePath;
}

TEST(SceneDocument, BuildsOrderedHierarchyAndSkipsUnknownComponent) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - name: Root
    transform:
      translation: [1.0, 2.0, 3.0]
    components:
      camera: {}
      unknown_component: {}
    children:
      - name: Child
        transform:
          translation: [4.0, 0.0, 0.0]
)");

    Scene Scene = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    ASSERT_EQ(Loaded->Warnings.size(), 1u);
    ASSERT_EQ(Scene.GetRoots().size(), 1u);

    const auto* Root = Scene.TryGetSceneNode(Scene.GetRoots().front());
    ASSERT_NE(Root, nullptr);
    EXPECT_EQ(Root->Name, "Root");
    ASSERT_EQ(Root->Children.size(), 1u);

    const auto* Child = Scene.TryGetSceneNode(Root->Children.front());
    ASSERT_NE(Child, nullptr);
    EXPECT_EQ(Child->Name, "Child");
    EXPECT_FLOAT_EQ(static_cast<float>(Root->Transform.Translation.x), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Child->Transform.Translation.x), 4.0f);

    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, LoadsBuiltInCameraAndMeshComponents) {
    const auto FilePath = WriteSceneFile(R"(
material_instances:
  gold:
    base_color: [1.0, 0.71, 0.22]
    metallic: 1.0
    roughness: 0.18
    base_color_texture: wood.png
    normal_texture: textures/wood_normal.png
    metallic_roughness_texture: textures/wood_mr.png
    metallic_texture: textures/wood_metallic.png
    roughness_texture: textures/wood_roughness.png
    occlusion_texture: textures/wood_occlusion.png
    emissive: [0.1, 0.2, 0.3]
    emissive_texture: textures/wood_emissive.png
entities:
  - components:
      camera:
        fov_degrees: 60.0
        near_plane: 0.1
        far_plane: 100.0
      mesh:
        asset: teapot.obj
        material: gold
)");

    Scene Scene = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->Warnings.empty());

    const auto Snapshot = Scene.BuildSnapshot();
    ASSERT_EQ(Snapshot.Renderables.size(), 1u);
    EXPECT_EQ(Snapshot.Renderables.front().MeshAsset, (FilePath.parent_path() / "Assets" / "teapot.obj").lexically_normal().string());
    EXPECT_EQ(Snapshot.Renderables.front().MaterialId, "gold");
    EXPECT_EQ(Snapshot.Renderables.front().Material.BaseColorTexture,
              (FilePath.parent_path() / "Assets" / "wood.png").lexically_normal().string());
    EXPECT_EQ(Snapshot.Renderables.front().Material.NormalTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_normal.png").lexically_normal().string());
    EXPECT_EQ(Snapshot.Renderables.front().Material.MetallicRoughnessTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_mr.png").lexically_normal().string());
    EXPECT_EQ(Snapshot.Renderables.front().Material.MetallicTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_metallic.png").lexically_normal().string());
    EXPECT_EQ(Snapshot.Renderables.front().Material.RoughnessTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_roughness.png").lexically_normal().string());
    EXPECT_EQ(Snapshot.Renderables.front().Material.OcclusionTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_occlusion.png").lexically_normal().string());
    EXPECT_EQ(Snapshot.Renderables.front().Material.EmissiveTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_emissive.png").lexically_normal().string());
    EXPECT_FLOAT_EQ(static_cast<float>(Snapshot.Renderables.front().Material.Emissive.x), 0.1f);
    EXPECT_FLOAT_EQ(static_cast<float>(Snapshot.Renderables.front().Material.Emissive.y), 0.2f);
    EXPECT_FLOAT_EQ(static_cast<float>(Snapshot.Renderables.front().Material.Emissive.z), 0.3f);
    EXPECT_FLOAT_EQ(static_cast<float>(Snapshot.Renderables.front().Material.BaseColor.x), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Snapshot.Renderables.front().Material.BaseColor.y), 0.71f);
    EXPECT_FLOAT_EQ(Snapshot.Renderables.front().Material.Metallic, 1.0f);
    EXPECT_FLOAT_EQ(Snapshot.Renderables.front().Material.Roughness, 0.18f);
    const auto Cameras = Scene.GetRegistry().view<CameraComponent>();
    const auto CameraIterator = Cameras.begin();
    ASSERT_NE(CameraIterator, Cameras.end());
    const auto& Camera = Cameras.get<CameraComponent>(*CameraIterator);
    EXPECT_FLOAT_EQ(Camera.Settings.FOV, 60.0f);
    EXPECT_FLOAT_EQ(Camera.Settings.NearPlane, 0.1f);
    EXPECT_FLOAT_EQ(Camera.Settings.FarPlane, 100.0f);

    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, SavesMaterialBaseColorTextureAndRejectsLegacyMeshTexture) {
    const auto FilePath = WriteSceneFile(R"(
material_instances:
  wood:
    base_color_texture: textures/wood.png
entities:
  - components:
      camera: {}
  - components:
      mesh:
        asset: teapot.obj
        material: wood
)");

    Scene Scene = {};
    ASSERT_TRUE(Scene.LoadFromFile(FilePath).has_value());
    const auto SavedPath = FilePath.parent_path() / "soulengine_scene_saved_test.yaml";
    ASSERT_TRUE(Scene.SaveToFile(SavedPath).has_value());
    const auto Saved = ReadFile(SavedPath);
    ASSERT_TRUE(Saved.has_value()) << Saved.error().ToString();
    EXPECT_TRUE(Saved->contains("base_color_texture"));
    EXPECT_TRUE(Saved->contains("emissive"));
    EXPECT_FALSE(Saved->contains("\n        texture:"));

    const auto LegacyPath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
  - components:
      mesh:
        asset: teapot.obj
        texture: legacy.png
)");
    SoulEngine::Scene LegacyScene = {};
    const auto LegacyLoaded = LegacyScene.LoadFromFile(LegacyPath);
    ASSERT_TRUE(LegacyLoaded.has_value()) << LegacyLoaded.error().ToString();
    ASSERT_EQ(LegacyLoaded->Warnings.size(), 1u);
    EXPECT_EQ(LegacyLoaded->Warnings.front().Path, "entities[1].components.mesh.texture");
    EXPECT_TRUE(LegacyScene.BuildSnapshot().Renderables.empty());

    std::filesystem::remove(FilePath);
    std::filesystem::remove(SavedPath);
    std::filesystem::remove(LegacyPath);
}

TEST(SceneDocument, SkipsMeshWithUnknownMaterialInstance) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
  - components:
      mesh:
        asset: teapot.obj
        material: missing
)");

    Scene Scene = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    ASSERT_EQ(Loaded->Warnings.size(), 1u);
    EXPECT_EQ(Loaded->Warnings.front().Path, "entities[1].components.mesh");
    EXPECT_TRUE(Scene.BuildSnapshot().Renderables.empty());

    std::filesystem::remove(FilePath);
}
TEST(SceneDocument, DefaultCameraFacesTeapotCluster) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - name: Main Camera
    transform:
      translation: [1.25, 1.25, 2.0]
      rotation: [28.0, -32.0, 0.0]
    components:
      camera: {}
  - name: Teapot Cluster
    transform:
      translation: [0.0, 0.0, 0.0]
)");

    Scene Scene = {};
    ASSERT_TRUE(Scene.LoadFromFile(FilePath).has_value());
    ASSERT_EQ(Scene.GetRoots().size(), 2u);
    static_cast<void>(Scene.BuildSnapshot());

    const auto* CameraNode = Scene.TryGetSceneNode(Scene.GetRoots()[0]);
    const auto* ClusterNode = Scene.TryGetSceneNode(Scene.GetRoots()[1]);
    ASSERT_NE(CameraNode, nullptr);
    ASSERT_NE(ClusterNode, nullptr);

    const auto WorldForward = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f),
                                           CameraNode->Transform.WorldTransform);
    const auto Forward = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
    const auto CameraPosition = hlslpp::float3(CameraNode->Transform.WorldTransform[3].x,
                                                 CameraNode->Transform.WorldTransform[3].y,
                                                 CameraNode->Transform.WorldTransform[3].z);
    const auto ClusterPosition = hlslpp::float3(ClusterNode->Transform.WorldTransform[3].x,
                                                  ClusterNode->Transform.WorldTransform[3].y,
                                                  ClusterNode->Transform.WorldTransform[3].z);
    const auto ToCluster = hlslpp::normalize(ClusterPosition - CameraPosition);

    EXPECT_GT(static_cast<float>(hlslpp::dot(Forward, ToCluster)), 0.99f);

    std::filesystem::remove(FilePath);
}
TEST(SceneDocument, DoesNotCreateViewsFromSceneCamera) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
)");

    Scene Scene = {};
    ASSERT_TRUE(Scene.LoadFromFile(FilePath).has_value());
    ASSERT_EQ(Scene.GetRoots().size(), 1u);

    EXPECT_TRUE(Scene.BuildSnapshot().Views.empty());
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, UsesExplicitRenderViews) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
)");

    Scene Scene = {};
    ASSERT_TRUE(Scene.LoadFromFile(FilePath).has_value());

    const std::array Views{
        RenderViewSnapshot{
            .CameraPosition = hlslpp::float3(4.0f, 5.0f, 6.0f),
        },
    };
    const auto Snapshot = Scene.BuildSnapshot(Views);

    ASSERT_EQ(Snapshot.Views.size(), 1u);
    EXPECT_FLOAT_EQ(static_cast<float>(Snapshot.Views.front().CameraPosition.x), 4.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Snapshot.Views.front().CameraPosition.y), 5.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Snapshot.Views.front().CameraPosition.z), 6.0f);
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, PreservesExistingSceneAfterStructuralError) {
    const auto ValidPath = WriteSceneFile("entities:\n  - name: Valid\n    components:\n      camera: {}\n");
    Scene Scene = {};
    ASSERT_TRUE(Scene.LoadFromFile(ValidPath).has_value());
    ASSERT_EQ(Scene.GetRoots().size(), 1u);

    const auto InvalidPath = WriteSceneFile("entities: not-a-sequence\n");
    const auto Loaded = Scene.LoadFromFile(InvalidPath);
    ASSERT_FALSE(Loaded.has_value());
    EXPECT_EQ(Scene.GetRoots().size(), 1u);

    std::filesystem::remove(ValidPath);
}

TEST(SceneDocument, RejectsSceneWithoutCamera) {
    const auto FilePath = WriteSceneFile("entities:\n  - name: Root\n");

    Scene Scene = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);

    ASSERT_FALSE(Loaded.has_value());
    EXPECT_TRUE(Loaded.error().ToString().contains("exactly one camera component; found 0"));
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, RejectsSceneWithMultipleCameras) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
  - components:
      camera: {}
)");

    Scene Scene = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);

    ASSERT_FALSE(Loaded.has_value());
    EXPECT_TRUE(Loaded.error().ToString().contains("exactly one camera component; found 2"));
    std::filesystem::remove(FilePath);
}

} // namespace
