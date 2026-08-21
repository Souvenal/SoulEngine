#include <gtest/gtest.h>

// Required while Scene exposes entt::registry in its object layout. These tests create
// Scene values and therefore instantiate Scene lifetime operations.
#include <entt/entt.hpp>
#include <hlsl++.h>

import Scene;
import Resource;

using namespace SoulEngine;

namespace {

[[nodiscard]] auto WriteSceneFile(StringView Content) -> Path {
    const auto    FilePath = std::filesystem::temp_directory_path() / "soulengine_scene_test.yaml";
    std::ofstream Output(FilePath);
    Output << Content;
    return FilePath;
}

[[nodiscard]] auto FindNamedEntity(const Scene& Scene, StringView Name) -> entt::entity {
    for (const auto Entity : Scene.GetRegistry().view<NameComponent>()) {
        if (Scene.GetRegistry().get<NameComponent>(Entity).Name == Name)
            return Entity;
    }
    return entt::null;
}

[[nodiscard]] auto CountNamedEntities(const Scene& Scene) -> std::size_t {
    std::size_t Count = 0;
    for (const auto Entity : Scene.GetRegistry().view<NameComponent>()) {
        static_cast<void>(Entity);
        ++Count;
    }
    return Count;
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

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    ASSERT_EQ(Loaded->second.Warnings.size(), 1u);
    auto& Scene = *Loaded->first;

    const auto RootEntity = FindNamedEntity(Scene, "Root");
    const auto ChildEntity = FindNamedEntity(Scene, "Child");
    ASSERT_NE(RootEntity, entt::null);
    ASSERT_NE(ChildEntity, entt::null);
    ASSERT_TRUE(Scene.GetRegistry().all_of<ChildrenComponent>(RootEntity));
    const auto& Children = Scene.GetRegistry().get<ChildrenComponent>(RootEntity).Children;
    ASSERT_EQ(Children.size(), 1u);
    EXPECT_EQ(Children.front(), ChildEntity);

    EXPECT_EQ(Scene.GetRegistry().get<NameComponent>(RootEntity).Name, "Root");
    EXPECT_EQ(Scene.GetRegistry().get<NameComponent>(ChildEntity).Name, "Child");
    EXPECT_FLOAT_EQ(
        static_cast<float>(Scene.GetRegistry().get<TransformComponent>(RootEntity).Translation.x), 1.0f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(Scene.GetRegistry().get<TransformComponent>(ChildEntity).Translation.x), 4.0f);

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

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->second.Warnings.empty());
    auto& Scene = *Loaded->first;

    const auto Snapshot = Scene.BuildSnapshot();
    ASSERT_EQ(Snapshot.Meshes.size(), 1u);
    EXPECT_EQ(Snapshot.Meshes.front().MeshAsset, "teapot.obj");
    EXPECT_EQ(Snapshot.Meshes.front().MaterialId, "gold");

    // Scene material instances are published to the engine-wide MaterialManager on
    // load, with texture paths resolved against the scene Assets root.
    const auto GoldId = MaterialManager::Get().FindMaterialId("gold");
    ASSERT_NE(GoldId, 0u);
    const auto* Gold = MaterialManager::Get().GetMaterial(GoldId);
    ASSERT_NE(Gold, nullptr);
    EXPECT_EQ(Gold->Value.BaseColorTexture,
              (FilePath.parent_path() / "Assets" / "wood.png").lexically_normal().string());
    EXPECT_EQ(Gold->Value.NormalTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_normal.png").lexically_normal().string());
    EXPECT_EQ(Gold->Value.MetallicRoughnessTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_mr.png").lexically_normal().string());
    EXPECT_EQ(Gold->Value.MetallicTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_metallic.png").lexically_normal().string());
    EXPECT_EQ(Gold->Value.RoughnessTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_roughness.png").lexically_normal().string());
    EXPECT_EQ(Gold->Value.OcclusionTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_occlusion.png").lexically_normal().string());
    EXPECT_EQ(Gold->Value.EmissiveTexture,
              (FilePath.parent_path() / "Assets" / "textures" / "wood_emissive.png").lexically_normal().string());
    EXPECT_FLOAT_EQ(static_cast<float>(Gold->Value.Emissive.x), 0.1f);
    EXPECT_FLOAT_EQ(static_cast<float>(Gold->Value.Emissive.y), 0.2f);
    EXPECT_FLOAT_EQ(static_cast<float>(Gold->Value.Emissive.z), 0.3f);
    EXPECT_FLOAT_EQ(static_cast<float>(Gold->Value.BaseColor.x), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Gold->Value.BaseColor.y), 0.71f);
    EXPECT_FLOAT_EQ(Gold->Value.Metallic, 1.0f);
    EXPECT_FLOAT_EQ(Gold->Value.Roughness, 0.18f);
    const auto Cameras        = Scene.GetRegistry().view<CameraComponent>();
    const auto CameraIterator = Cameras.begin();
    ASSERT_NE(CameraIterator, Cameras.end());
    const auto& Camera = Cameras.get<CameraComponent>(*CameraIterator);
    EXPECT_FLOAT_EQ(Camera.FOV, 60.0f);
    EXPECT_FLOAT_EQ(Camera.NearPlane, 0.1f);
    EXPECT_FLOAT_EQ(Camera.FarPlane, 100.0f);

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
    const auto LegacyLoaded = Scene::LoadFromFile(LegacyPath);
    ASSERT_TRUE(LegacyLoaded.has_value()) << LegacyLoaded.error().ToString();
    ASSERT_EQ(LegacyLoaded->second.Warnings.size(), 1u);
    EXPECT_EQ(LegacyLoaded->second.Warnings.front().Path, "entities[1].components.mesh.texture");
    EXPECT_TRUE(LegacyLoaded->first->BuildSnapshot().Meshes.empty());

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

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->second.Warnings.empty());
    auto& Scene = *Loaded->first;
    std::size_t MeshCount = 0;
    for (const auto Entity : Scene.GetRegistry().view<MeshComponent>()) {
        static_cast<void>(Entity);
        ++MeshCount;
    }
    EXPECT_EQ(MeshCount, 1u);
    // A missing scene material instance no longer drops the mesh from the snapshot:
    // the renderer falls back to the mesh-imported material, then the built-in default.
    const auto Snapshot = Scene.BuildSnapshot();
    ASSERT_EQ(Snapshot.Meshes.size(), 1u);
    EXPECT_EQ(Snapshot.Meshes.front().MaterialId, "missing");

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

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    auto& Scene = *Loaded->first;
    ASSERT_EQ(CountNamedEntities(Scene), 2u);
    Scene.Tick(0.0f);
    static_cast<void>(Scene.BuildSnapshot());

    const auto CameraEntity = FindNamedEntity(Scene, "Main Camera");
    const auto ClusterEntity = FindNamedEntity(Scene, "Teapot Cluster");
    ASSERT_NE(CameraEntity, entt::null);
    ASSERT_NE(ClusterEntity, entt::null);
    const auto& CameraTransform = Scene.GetRegistry().get<TransformComponent>(CameraEntity);
    const auto& ClusterTransform = Scene.GetRegistry().get<TransformComponent>(ClusterEntity);

    const auto WorldForward   = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), CameraTransform.WorldTransform);
    const auto Forward        = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
    const auto CameraPosition = hlslpp::float3(
        CameraTransform.WorldTransform[3].x,
        CameraTransform.WorldTransform[3].y,
        CameraTransform.WorldTransform[3].z);
    const auto ClusterPosition = hlslpp::float3(
        ClusterTransform.WorldTransform[3].x,
        ClusterTransform.WorldTransform[3].y,
        ClusterTransform.WorldTransform[3].z);
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

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    auto& Scene = *Loaded->first;

    EXPECT_TRUE(Scene.BuildSnapshot().Views.empty());
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, UsesExplicitRenderViews) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
)");

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    auto& Scene = *Loaded->first;

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
    const auto ValidLoaded = Scene::LoadFromFile(ValidPath);
    ASSERT_TRUE(ValidLoaded.has_value()) << ValidLoaded.error().ToString();
    auto& Scene = *ValidLoaded->first;

    const auto InvalidPath = WriteSceneFile("entities: not-a-sequence\n");
    const auto Loaded      = Scene::LoadFromFile(InvalidPath);
    ASSERT_FALSE(Loaded.has_value());
    EXPECT_EQ(CountNamedEntities(Scene), 1u);

    std::filesystem::remove(ValidPath);
}

TEST(SceneDocument, ReportsYamlSyntaxErrorsWithoutReplacingExistingScene) {
    const auto ValidPath = WriteSceneFile("entities:\n  - name: Valid\n    components:\n      camera: {}\n");
    const auto ValidLoaded = Scene::LoadFromFile(ValidPath);
    ASSERT_TRUE(ValidLoaded.has_value()) << ValidLoaded.error().ToString();
    auto& Scene = *ValidLoaded->first;

    const auto InvalidPath = WriteSceneFile("entities:\n  - components: [\n");
    const auto Loaded      = Scene::LoadFromFile(InvalidPath);

    ASSERT_FALSE(Loaded.has_value());
    EXPECT_TRUE(Loaded.error().ToString().contains("line"));
    EXPECT_TRUE(Loaded.error().ToString().contains("column"));
    EXPECT_EQ(Scene.GetRegistry().get<NameComponent>(
                  FindNamedEntity(Scene, "Valid")).Name,
              "Valid");

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
        const auto Loaded = Scene::LoadFromFile(FilePath);
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
    const auto ValidLoaded = Scene::LoadFromFile(ValidPath);
    ASSERT_TRUE(ValidLoaded.has_value()) << ValidLoaded.error().ToString();
    const auto& ValidScene = *ValidLoaded->first;
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
    const auto InvalidLoaded = Scene::LoadFromFile(InvalidPath);
    ASSERT_TRUE(InvalidLoaded.has_value()) << InvalidLoaded.error().ToString();
    ASSERT_EQ(InvalidLoaded->second.Warnings.size(), 1u);
    EXPECT_EQ(InvalidLoaded->second.Warnings.front().Path, "entities[1].components.light.casts_shadows");

    std::filesystem::remove(ValidPath);
    std::filesystem::remove(InvalidPath);
}

TEST(SceneDocument, AllowsSceneWithoutCamera) {
    const auto FilePath = WriteSceneFile("entities:\n  - name: Root\n");

    const auto Loaded = Scene::LoadFromFile(FilePath);

    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->second.Warnings.empty());
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

    const auto Loaded = Scene::LoadFromFile(FilePath);

    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    auto& Scene = *Loaded->first;
    std::size_t CameraCount = 0;
    for (const auto Entity : Scene.GetRegistry().view<CameraComponent>()) {
        static_cast<void>(Entity);
        ++CameraCount;
    }
    EXPECT_EQ(CameraCount, 2u);
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, BuildsPhysicalLightInfo) {
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

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    auto& Scene = *Loaded->first;
    Scene.Tick(0.0f);
    const auto Snapshot = Scene.BuildSnapshot();

    ASSERT_EQ(Snapshot.Lights.size(), 3u);
    const auto Directional = std::ranges::find_if(
        Snapshot.Lights, [](const LightInfo& Light) -> bool { return Light.Type == LightType::Directional; });
    const auto Point = std::ranges::find_if(
        Snapshot.Lights, [](const LightInfo& Light) -> bool { return Light.Type == LightType::Point; });
    const auto Spot = std::ranges::find_if(
        Snapshot.Lights, [](const LightInfo& Light) -> bool { return Light.Type == LightType::Spot; });
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
