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

TEST(MeshRecord, TraversesNestedAssetNodesAndRepeatedSubMeshes) {
    const auto Translate = [](float X, float Y, float Z) -> hlslpp::float4x4 {
        return hlslpp::float4x4::translation(hlslpp::float3(X, Y, Z));
    };
    const MeshRecord Mesh{
        .SubMeshes = {SubMesh{}, SubMesh{}},
        .Nodes = {
            MeshAssetNode{
                .Name             = "Root",
                .LocalTransform   = Translate(1.0f, 0.0f, 0.0f),
                .SubMeshIndices   = {0},
                .ChildNodeIndices = {1},
            },
            MeshAssetNode{
                .Name             = "Grouping Node",
                .LocalTransform   = Translate(0.0f, 2.0f, 0.0f),
                .ChildNodeIndices = {2},
            },
            MeshAssetNode{
                .Name           = "Repeated Geometry",
                .LocalTransform = Translate(0.0f, 0.0f, 3.0f),
                .SubMeshIndices = {1, 0},
            },
        },
    };

    struct AssetInstance {
        Uint32           SubMeshIndex = 0;
        hlslpp::float4x4 Transform    = hlslpp::float4x4::identity();
    };
    std::vector<AssetInstance> Instances;
    const auto VisitNode = [&Mesh, &Instances](auto&& VisitNode,
                                                Uint32 NodeIndex,
                                                const hlslpp::float4x4& ParentTransform) -> void {
        if (NodeIndex >= Mesh.Nodes.size())
            return;
        const auto& Node      = Mesh.Nodes[NodeIndex];
        const auto  Transform = hlslpp::mul(Node.LocalTransform, ParentTransform);
        for (const auto SubMeshIndex : Node.SubMeshIndices)
            Instances.emplace_back(AssetInstance{.SubMeshIndex = SubMeshIndex, .Transform = Transform});
        for (const auto ChildNodeIndex : Node.ChildNodeIndices)
            VisitNode(VisitNode, ChildNodeIndex, Transform);
    };
    VisitNode(VisitNode, 0, hlslpp::float4x4::identity());

    ASSERT_EQ(Instances.size(), 3u);
    EXPECT_EQ(Instances[0].SubMeshIndex, 0u);
    EXPECT_EQ(Instances[1].SubMeshIndex, 1u);
    EXPECT_EQ(Instances[2].SubMeshIndex, 0u);

    const auto TransformPoint = [](const hlslpp::float4& Point, const hlslpp::float4x4& Transform) -> hlslpp::float4 {
        return hlslpp::mul(Point, Transform);
    };
    const auto Origin         = hlslpp::float4(0.0f, 0.0f, 0.0f, 1.0f);
    const auto RootOrigin     = TransformPoint(Origin, Instances[0].Transform);
    const auto RepeatedOrigin = TransformPoint(Origin, Instances[2].Transform);
    EXPECT_FLOAT_EQ(static_cast<float>(RootOrigin.x), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(RootOrigin.y), 0.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(RootOrigin.z), 0.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(RepeatedOrigin.x), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(RepeatedOrigin.y), 2.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(RepeatedOrigin.z), 3.0f);

    const auto EntityTransform = hlslpp::mul(
        hlslpp::mul(hlslpp::float4x4::scale(hlslpp::float3(2.0f, 3.0f, 1.0f)),
                     hlslpp::float4x4::rotation_z(std::numbers::pi_v<float> * 0.5f)),
        Translate(10.0f, 0.0f, 0.0f));
    const auto Point               = hlslpp::float4(2.0f, -1.0f, 4.0f, 1.0f);
    const auto ExpectedWorldPoint  = TransformPoint(TransformPoint(Point, Instances[2].Transform), EntityTransform);
    const auto ActualWorldPoint    = TransformPoint(Point, hlslpp::mul(Instances[2].Transform, EntityTransform));
    const auto ReversedWorldPoint  = TransformPoint(Point, hlslpp::mul(EntityTransform, Instances[2].Transform));
    EXPECT_FLOAT_EQ(static_cast<float>(ActualWorldPoint.x), static_cast<float>(ExpectedWorldPoint.x));
    EXPECT_FLOAT_EQ(static_cast<float>(ActualWorldPoint.y), static_cast<float>(ExpectedWorldPoint.y));
    EXPECT_FLOAT_EQ(static_cast<float>(ActualWorldPoint.z), static_cast<float>(ExpectedWorldPoint.z));
    EXPECT_NE(static_cast<float>(ActualWorldPoint.x), static_cast<float>(ReversedWorldPoint.x));
}

TEST(SceneMeta, RegistersBuiltInComponentTypesAndFieldsBeforeSceneLoading) {
    const auto Camera = entt::resolve(entt::hashed_string{"camera"}.value());
    ASSERT_TRUE(Camera);
    EXPECT_TRUE(Camera.data(entt::hashed_string{"fov_degrees"}.value()));
    EXPECT_TRUE(Camera.data(entt::hashed_string{"near_plane"}.value()));
    EXPECT_TRUE(Camera.data(entt::hashed_string{"far_plane"}.value()));
    EXPECT_TRUE(Camera.data(entt::hashed_string{"exposure_ev100"}.value()));

    const auto Transform = entt::resolve(entt::hashed_string{"transform"}.value());
    ASSERT_TRUE(Transform);
    EXPECT_TRUE(Transform.data(entt::hashed_string{"translation"}.value()));
    EXPECT_TRUE(Transform.data(entt::hashed_string{"rotation"}.value()));
    EXPECT_TRUE(Transform.data(entt::hashed_string{"scale"}.value()));

    const auto Mesh = entt::resolve(entt::hashed_string{"mesh"}.value());
    ASSERT_TRUE(Mesh);
    EXPECT_TRUE(Mesh.data(entt::hashed_string{"material_override"}.value()));

    const auto Light = entt::resolve(entt::hashed_string{"light"}.value());
    ASSERT_TRUE(Light);
    EXPECT_TRUE(Light.data(entt::hashed_string{"type"}.value()));
    EXPECT_TRUE(Light.data(entt::hashed_string{"casts_shadows"}.value()));
}

TEST(SceneDocument, BuildsOrderedHierarchyAndSkipsUnknownComponent) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - name: Root
    components:
      transform:
        translation: [1.0, 2.0, 3.0]
      camera: {}
      unknown_component: {}
    children:
      - name: Child
        components:
          transform:
            translation: [4.0, 0.0, 0.0]
)");

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    ASSERT_EQ(Loaded->second.Warnings.size(), 1u);
    auto& Scene = *Loaded->first;

    const auto RootEntity  = FindNamedEntity(Scene, "Root");
    const auto ChildEntity = FindNamedEntity(Scene, "Child");
    ASSERT_NE(RootEntity, entt::null);
    ASSERT_NE(ChildEntity, entt::null);
    ASSERT_TRUE(Scene.GetRegistry().all_of<ChildrenComponent>(RootEntity));
    const auto& Children = Scene.GetRegistry().get<ChildrenComponent>(RootEntity).Children;
    ASSERT_EQ(Children.size(), 1u);
    EXPECT_EQ(Children.front(), ChildEntity);

    EXPECT_EQ(Scene.GetRegistry().get<NameComponent>(RootEntity).Name, "Root");
    EXPECT_EQ(Scene.GetRegistry().get<NameComponent>(ChildEntity).Name, "Child");
    EXPECT_FLOAT_EQ(static_cast<float>(Scene.GetRegistry().get<TransformComponent>(RootEntity).Translation.x), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Scene.GetRegistry().get<TransformComponent>(ChildEntity).Translation.x), 4.0f);

    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, LoadsBuiltInCameraAndMeshComponents) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera:
        fov_degrees: 60.0
        near_plane: 0.1
        far_plane: 100.0
      mesh:
        asset: teapot.obj
        material_override: materials/gold.yaml
)");

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->second.Warnings.empty());
    auto& Scene = *Loaded->first;

    const auto Snapshot = Scene.BuildSnapshot();
    // Mesh resources are loaded by MeshSystem::OnUpdate once an RHI device exists.
    // This document test validates the authoring component before runtime loading.
    EXPECT_TRUE(Snapshot.Instances.empty());
    const auto MeshEntities = Scene.GetRegistry().view<MeshComponent>();
    ASSERT_NE(MeshEntities.begin(), MeshEntities.end());
    EXPECT_EQ(MeshEntities.get<MeshComponent>(*MeshEntities.begin()).Asset, Path{"teapot.obj"});

    EXPECT_EQ(Scene.GetRegistry().get<MeshComponent>(*Scene.GetRegistry().view<MeshComponent>().begin()).MaterialOverridePath,
              Path{"materials/gold.yaml"});
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
    const auto LegacyPath   = WriteSceneFile(R"(
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
    EXPECT_EQ(LegacyLoaded->second.Warnings.front().Location, "line 8, column 18");
    EXPECT_TRUE(LegacyLoaded->first->BuildSnapshot().Instances.empty());

    std::filesystem::remove(LegacyPath);
}

TEST(SceneDocument, PreservesMeshWithMaterialOverridePath) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
  - components:
      mesh:
        asset: teapot.obj
        material_override: materials/missing.yaml
)");

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->second.Warnings.empty());
    auto&       Scene     = *Loaded->first;
    std::size_t MeshCount = 0;
    for (const auto Entity : Scene.GetRegistry().view<MeshComponent>()) {
        static_cast<void>(Entity);
        ++MeshCount;
    }
    EXPECT_EQ(MeshCount, 1u);
    EXPECT_EQ(Scene.GetRegistry().get<MeshComponent>(*Scene.GetRegistry().view<MeshComponent>().begin()).MaterialOverridePath,
              Path{"materials/missing.yaml"});
    const auto Snapshot = Scene.BuildSnapshot();
    EXPECT_TRUE(Snapshot.Instances.empty());

    std::filesystem::remove(FilePath);
}
TEST(SceneDocument, DefaultCameraFacesTeapotCluster) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - name: Main Camera
    components:
      transform:
        translation: [1.25, 1.25, 2.0]
        rotation: [28.0, -32.0, 0.0]
      camera: {}
  - name: Teapot Cluster
    components:
      transform:
        translation: [0.0, 0.0, 0.0]
)");

    const auto Loaded = Scene::LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    auto& Scene = *Loaded->first;
    ASSERT_EQ(CountNamedEntities(Scene), 2u);
    Scene.Tick(0.0f);
    static_cast<void>(Scene.BuildSnapshot());

    const auto CameraEntity  = FindNamedEntity(Scene, "Main Camera");
    const auto ClusterEntity = FindNamedEntity(Scene, "Teapot Cluster");
    ASSERT_NE(CameraEntity, entt::null);
    ASSERT_NE(ClusterEntity, entt::null);
    const auto& CameraTransform  = Scene.GetRegistry().get<TransformComponent>(CameraEntity);
    const auto& ClusterTransform = Scene.GetRegistry().get<TransformComponent>(ClusterEntity);

    const auto WorldForward   = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), CameraTransform.WorldTransform);
    const auto Forward        = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
    const auto CameraPosition = hlslpp::float3(
        CameraTransform.WorldTransform[3].x, CameraTransform.WorldTransform[3].y, CameraTransform.WorldTransform[3].z);
    const auto ClusterPosition = hlslpp::float3(ClusterTransform.WorldTransform[3].x,
                                                ClusterTransform.WorldTransform[3].y,
                                                ClusterTransform.WorldTransform[3].z);
    const auto ToCluster       = hlslpp::normalize(ClusterPosition - CameraPosition);

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

TEST(SceneDocument, PreservesExistingSceneAfterStructuralError) {
    const auto ValidPath   = WriteSceneFile("entities:\n  - name: Valid\n    components:\n      camera: {}\n");
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
    const auto ValidPath   = WriteSceneFile("entities:\n  - name: Valid\n    components:\n      camera: {}\n");
    const auto ValidLoaded = Scene::LoadFromFile(ValidPath);
    ASSERT_TRUE(ValidLoaded.has_value()) << ValidLoaded.error().ToString();
    auto& Scene = *ValidLoaded->first;

    const auto InvalidPath = WriteSceneFile("entities:\n  - components: [\n");
    const auto Loaded      = Scene::LoadFromFile(InvalidPath);

    ASSERT_FALSE(Loaded.has_value());
    EXPECT_TRUE(Loaded.error().ToString().contains("line"));
    EXPECT_TRUE(Loaded.error().ToString().contains("column"));
    EXPECT_EQ(Scene.GetRegistry().get<NameComponent>(FindNamedEntity(Scene, "Valid")).Name, "Valid");

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
        const auto Loaded   = Scene::LoadFromFile(FilePath);
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
    EXPECT_EQ(InvalidLoaded->second.Warnings.front().Location, "line 12, column 24");

    std::filesystem::remove(ValidPath);
    std::filesystem::remove(InvalidPath);
}

TEST(SceneDocument, AllowsSceneWithoutCamera) {
    const auto FilePath = WriteSceneFile("entities:\n  - name: Root\n");

    const auto Loaded = Scene::LoadFromFile(FilePath);

    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->second.Warnings.empty());
    const auto Entity = FindNamedEntity(*Loaded->first, "Root");
    ASSERT_NE(Entity, entt::null);
    const auto& Transform = Loaded->first->GetRegistry().get<TransformComponent>(Entity);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Translation.x), 0.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Translation.y), 0.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Translation.z), 0.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Scale.x), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Scale.y), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Scale.z), 1.0f);
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, PreservesDefaultTransformWhenTransformComponentIsInvalid) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - name: Root
    components:
      transform:
        translation: [1.0, 2.0]
)");

    const auto Loaded = Scene::LoadFromFile(FilePath);

    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    ASSERT_EQ(Loaded->second.Warnings.size(), 1u);
    EXPECT_EQ(Loaded->second.Warnings.front().Location, "line 6, column 22");
    const auto Entity = FindNamedEntity(*Loaded->first, "Root");
    ASSERT_NE(Entity, entt::null);
    const auto& Transform = Loaded->first->GetRegistry().get<TransformComponent>(Entity);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Translation.x), 0.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Translation.y), 0.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Transform.Translation.z), 0.0f);
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
    auto&       Scene       = *Loaded->first;
    std::size_t CameraCount = 0;
    for (const auto Entity : Scene.GetRegistry().view<CameraComponent>()) {
        static_cast<void>(Entity);
        ++CameraCount;
    }
    EXPECT_EQ(CameraCount, 2u);
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, BuildsPhysicalLightRecord) {
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
    components:
      transform:
        translation: [2.0, 3.0, 4.0]
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
        Snapshot.Lights, [](const LightRecord& Light) -> bool { return Light.Type == LightType::Directional; });
    const auto Point = std::ranges::find_if(
        Snapshot.Lights, [](const LightRecord& Light) -> bool { return Light.Type == LightType::Point; });
    const auto Spot = std::ranges::find_if(
        Snapshot.Lights, [](const LightRecord& Light) -> bool { return Light.Type == LightType::Spot; });
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
