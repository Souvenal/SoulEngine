#include <gtest/gtest.h>

// Required while Scene exposes entt::registry in its object layout. These tests create
// Scene values and therefore instantiate Scene lifetime operations.
#include <entt/entt.hpp>
#include <hlsl++.h>

import Scene;

using namespace SoulEngine::Core;
using namespace SoulEngine::Scene;

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
entities:
  - components:
      camera:
        fov_degrees: 60.0
        near_plane: 0.1
        far_plane: 100.0
      mesh:
        asset: teapot.obj
)");

    Scene Scene = {};
    const auto Loaded = Scene.LoadFromFile(FilePath);
    ASSERT_TRUE(Loaded.has_value()) << Loaded.error().ToString();
    EXPECT_TRUE(Loaded->Warnings.empty());

    const auto Snapshot = Scene.BuildSnapshot();
    ASSERT_EQ(Snapshot.Renderables.size(), 1u);
    EXPECT_EQ(Snapshot.Renderables.front().MeshAsset, "teapot.obj");

    std::filesystem::remove(FilePath);
}
TEST(SceneDocument, DefaultCameraFacesTeapotCluster) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - name: Main Camera
    transform:
      translation: [1.25, 1.25, 2.0]
      rotation_degrees: [28.0, -32.0, 0.0]
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
TEST(SceneDocument, CursorUpRotatesFirstCameraTowardPositiveWorldY) {
    const auto FilePath = WriteSceneFile(R"(
entities:
  - components:
      camera: {}
)");

    Scene Scene = {};
    ASSERT_TRUE(Scene.LoadFromFile(FilePath).has_value());
    ASSERT_EQ(Scene.GetRoots().size(), 1u);

    Scene.RotateFirstCamera(0.0f, -10.0f);
    const auto* CameraNode = Scene.TryGetSceneNode(Scene.GetRoots().front());

    ASSERT_NE(CameraNode, nullptr);
    EXPECT_LT(static_cast<float>(CameraNode->Transform.RotationDegrees.x), 0.0f);
    std::filesystem::remove(FilePath);
}

TEST(SceneDocument, PreservesExistingSceneAfterStructuralError) {
    const auto ValidPath = WriteSceneFile("entities:\n  - name: Valid\n");
    Scene Scene = {};
    ASSERT_TRUE(Scene.LoadFromFile(ValidPath).has_value());
    ASSERT_EQ(Scene.GetRoots().size(), 1u);

    const auto InvalidPath = WriteSceneFile("entities: not-a-sequence\n");
    const auto Loaded = Scene.LoadFromFile(InvalidPath);
    ASSERT_FALSE(Loaded.has_value());
    EXPECT_EQ(Scene.GetRoots().size(), 1u);

    std::filesystem::remove(ValidPath);
}

} // namespace










