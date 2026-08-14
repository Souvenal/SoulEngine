#include <gtest/gtest.h>

// Required while Scene exposes entt::registry in its object layout. These tests create
// Scene values and therefore instantiate Scene lifetime operations.
#include <entt/entt.hpp>
#include <hlsl++.h>

import Scene;

using namespace SoulEngine;

namespace {

[[nodiscard]] auto WriteSceneFile(StringView Content) -> Path {
    const auto    FilePath = std::filesystem::temp_directory_path() / "soulengine_scene_test.yaml";
    std::ofstream Output(FilePath);
    Output << Content;
    return FilePath;
}

TEST(SceneMeta, RegistersBuiltInComponentTypesAndFieldsBeforeSceneLoading) {
    const auto Camera = entt::resolve(entt::hashed_string{"camera"}.value());
    ASSERT_TRUE(Camera);
    EXPECT_TRUE(Camera.data(entt::hashed_string{"fov_degrees"}.value()));
    EXPECT_TRUE(Camera.data(entt::hashed_string{"near_plane"}.value()));
    EXPECT_TRUE(Camera.data(entt::hashed_string{"far_plane"}.value()));
    EXPECT_TRUE(Camera.data(entt::hashed_string{"exposure_ev100"}.value()));

    const auto Mesh = entt::resolve(entt::hashed_string{"mesh"}.value());
    ASSERT_TRUE(Mesh);
    EXPECT_TRUE(Mesh.data(entt::hashed_string{"asset"}.value()));
    EXPECT_TRUE(Mesh.data(entt::hashed_string{"material"}.value()));

    const auto Light = entt::resolve(entt::hashed_string{"light"}.value());
    ASSERT_TRUE(Light);
    EXPECT_TRUE(Light.data(entt::hashed_string{"type"}.value()));
    EXPECT_TRUE(Light.data(entt::hashed_string{"casts_shadows"}.value()));
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

    Scene      Scene  = {};
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
    EXPECT_FLOAT_EQ(static_cast<float>(Root->LocalTransform.Translation.x), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Child->LocalTransform.Translation.x), 4.0f);

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

    Scene      Scene  = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->Warnings.empty());

    const auto Snapshot = Scene.BuildSnapshot();
    ASSERT_EQ(Snapshot.Renderables.size(), 1u);
    EXPECT_EQ(Snapshot.Renderables.front().MeshAsset,
              (FilePath.parent_path() / "Assets" / "teapot.obj").lexically_normal().string());
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
    const auto Cameras        = Scene.GetRegistry().view<CameraComponent>();
    const auto CameraIterator = Cameras.begin();
    ASSERT_NE(CameraIterator, Cameras.end());
    const auto& Camera = Cameras.get<CameraComponent>(*CameraIterator);
    EXPECT_FLOAT_EQ(Camera.Settings.FOV, 60.0f);
    EXPECT_FLOAT_EQ(Camera.Settings.NearPlane, 0.1f);
    EXPECT_FLOAT_EQ(Camera.Settings.FarPlane, 100.0f);

    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, RejectsLegacyMeshTextureField) {
    const auto        LegacyPath   = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
  - components:
      mesh:
        asset: teapot.obj
        texture: legacy.png
)");
    SoulEngine::Scene LegacyScene  = {};
    const auto        LegacyLoaded = LegacyScene.LoadFromFile(LegacyPath);
    ASSERT_TRUE(LegacyLoaded.has_value()) << LegacyLoaded.error().ToString();
    ASSERT_EQ(LegacyLoaded->Warnings.size(), 1u);
    EXPECT_EQ(LegacyLoaded->Warnings.front().Path, "entities[1].components.mesh.texture");
    EXPECT_TRUE(LegacyScene.BuildSnapshot().Renderables.empty());

    std::filesystem::remove(LegacyPath);
}

TEST(SceneDocument, PreservesMeshWithUnknownMaterialInstance) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
  - components:
      mesh:
        asset: teapot.obj
        material: missing
)");

    Scene      Scene  = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->Warnings.empty());
    std::size_t MeshCount = 0;
    for (const auto Entity : Scene.GetRegistry().view<MeshComponent>()) {
        static_cast<void>(Entity);
        ++MeshCount;
    }
    EXPECT_EQ(MeshCount, 1u);
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

    const auto* CameraNode  = Scene.TryGetSceneNode(Scene.GetRoots()[0]);
    const auto* ClusterNode = Scene.TryGetSceneNode(Scene.GetRoots()[1]);
    ASSERT_NE(CameraNode, nullptr);
    ASSERT_NE(ClusterNode, nullptr);

    const auto WorldForward   = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), CameraNode->WorldTransform);
    const auto Forward        = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
    const auto CameraPosition = hlslpp::float3(
        CameraNode->WorldTransform[3].x, CameraNode->WorldTransform[3].y, CameraNode->WorldTransform[3].z);
    const auto ClusterPosition = hlslpp::float3(
        ClusterNode->WorldTransform[3].x, ClusterNode->WorldTransform[3].y, ClusterNode->WorldTransform[3].z);
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
    Scene      Scene     = {};
    ASSERT_TRUE(Scene.LoadFromFile(ValidPath).has_value());
    ASSERT_EQ(Scene.GetRoots().size(), 1u);

    const auto InvalidPath = WriteSceneFile("entities: not-a-sequence\n");
    const auto Loaded      = Scene.LoadFromFile(InvalidPath);
    ASSERT_FALSE(Loaded.has_value());
    EXPECT_EQ(Scene.GetRoots().size(), 1u);

    std::filesystem::remove(ValidPath);
}

TEST(SceneDocument, ReportsYamlSyntaxErrorsWithoutReplacingExistingScene) {
    const auto ValidPath = WriteSceneFile("entities:\n  - name: Valid\n    components:\n      camera: {}\n");
    Scene      Scene     = {};
    ASSERT_TRUE(Scene.LoadFromFile(ValidPath).has_value());

    const auto InvalidPath = WriteSceneFile("entities:\n  - components: [\n");
    const auto Loaded      = Scene.LoadFromFile(InvalidPath);

    ASSERT_FALSE(Loaded.has_value());
    EXPECT_TRUE(Loaded.error().ToString().contains("line"));
    EXPECT_TRUE(Loaded.error().ToString().contains("column"));
    ASSERT_EQ(Scene.GetRoots().size(), 1u);
    EXPECT_EQ(Scene.TryGetSceneNode(Scene.GetRoots().front())->Name, "Valid");

    std::filesystem::remove(ValidPath);
}

TEST(SceneDocument, RejectsUnsupportedYamlFeaturesAndDuplicateKeys) {
    const std::array InvalidDocuments{
        R"(
entities:
  - &camera
    components:
      camera: {}
)",
        R"(
entities:
  - &camera
    components:
      camera: {}
  - *camera
)",
        R"(
!!map
entities:
  - components:
      camera: {}
)",
        R"(
%YAML 1.2
---
entities:
  - components:
      camera: {}
)",
        R"(
entities:
  - components:
      camera: {}
---
entities:
  - components:
      camera: {}
)",
        R"(
entities:
  - name: First
    name: Second
    components:
      camera: {}
)",
    };

    for (const auto Document : InvalidDocuments) {
        const auto FilePath = WriteSceneFile(Document);
        Scene      Scene    = {};
        const auto Loaded   = Scene.LoadFromFile(FilePath);
        EXPECT_FALSE(Loaded.has_value()) << Loaded.error().ToString();
        std::filesystem::remove(FilePath);
    }
}

TEST(SceneDocument, UsesYaml12CoreScalarRules) {
    const auto ValidPath   = WriteSceneFile(R"(
entities:
  - components:
      camera:
        fov_degrees: +6.0e1
  - components:
      light:
        type: directional
        color_r: 1.0
        color_g: 1.0
        color_b: 1.0
        intensity: 1.0e5
        casts_shadows: TRUE
)");
    Scene      ValidScene  = {};
    const auto ValidLoaded = ValidScene.LoadFromFile(ValidPath);
    ASSERT_TRUE(ValidLoaded.has_value()) << ValidLoaded.error().ToString();
    const auto  Lights     = ValidScene.GetRegistry().view<LightComponent>();
    std::size_t LightCount = 0;
    for (const auto Entity : Lights) {
        EXPECT_TRUE(Lights.get<LightComponent>(Entity).CastsShadows);
        ++LightCount;
    }
    EXPECT_EQ(LightCount, 1u);

    const auto InvalidPath   = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
  - components:
      light:
        type: directional
        color_r: 1.0
        color_g: 1.0
        color_b: 1.0
        intensity: 1.0
        casts_shadows: yes
)");
    Scene      InvalidScene  = {};
    const auto InvalidLoaded = InvalidScene.LoadFromFile(InvalidPath);
    ASSERT_TRUE(InvalidLoaded.has_value()) << InvalidLoaded.error().ToString();
    ASSERT_EQ(InvalidLoaded->Warnings.size(), 1u);
    EXPECT_EQ(InvalidLoaded->Warnings.front().Path, "entities[1].components.light.casts_shadows");

    std::filesystem::remove(ValidPath);
    std::filesystem::remove(InvalidPath);
}

TEST(SceneDocument, AllowsSceneWithoutCamera) {
    const auto FilePath = WriteSceneFile("entities:\n  - name: Root\n");

    Scene      Scene  = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);

    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->Warnings.empty());
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, AllowsSceneWithMultipleCameras) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
  - components:
      camera: {}
)");

    Scene      Scene  = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);

    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    std::size_t CameraCount = 0;
    for (const auto Entity : Scene.GetRegistry().view<CameraComponent>()) {
        static_cast<void>(Entity);
        ++CameraCount;
    }
    EXPECT_EQ(CameraCount, 2u);
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, BuildsPhysicalLightSnapshots) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera:
        exposure_ev100: 12.0
  - name: Sun
    components:
      light:
        type: directional
        color_r: 1.0
        color_g: 1.0
        color_b: 1.0
        intensity: 100000.0
  - name: Lamp
    transform:
      translation: [2.0, 3.0, 4.0]
    components:
      light:
        type: point
        color_r: 1.0
        color_g: 1.0
        color_b: 1.0
        intensity: 600.0
        range_meters: 8.0
  - name: Spot
    components:
      light:
        type: spot
        color_r: 1.0
        color_g: 1.0
        color_b: 1.0
        intensity: 400.0
        range_meters: 12.0
        inner_cone_angle_degrees: 15.0
        outer_cone_angle_degrees: 25.0
)");

    Scene Scene = {};
    ASSERT_TRUE(Scene.LoadFromFile(FilePath).has_value());
    const auto Snapshot = Scene.BuildSnapshot();

    ASSERT_EQ(Snapshot.Lights.size(), 3u);
    const auto Directional = std::ranges::find_if(
        Snapshot.Lights, [](const LightSnapshot& Light) -> bool { return Light.Type == LightType::Directional; });
    const auto Point = std::ranges::find_if(
        Snapshot.Lights, [](const LightSnapshot& Light) -> bool { return Light.Type == LightType::Point; });
    const auto Spot = std::ranges::find_if(
        Snapshot.Lights, [](const LightSnapshot& Light) -> bool { return Light.Type == LightType::Spot; });
    ASSERT_NE(Directional, Snapshot.Lights.end());
    ASSERT_NE(Point, Snapshot.Lights.end());
    ASSERT_NE(Spot, Snapshot.Lights.end());
    EXPECT_FLOAT_EQ(Directional->Intensity, 100000.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Point->Position.x), 2.0f);
    EXPECT_FLOAT_EQ(Point->RangeMeters, 8.0f);
    EXPECT_GT(Spot->InnerConeCosine, Spot->OuterConeCosine);

    std::filesystem::remove(FilePath);
}
} // namespace
