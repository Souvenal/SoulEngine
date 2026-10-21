#include <gtest/gtest.h>
#include <entt/entt.hpp>
#include <hlsl++.h>

import std;
import Core;

using namespace SoulEngine;

namespace {

struct PersistedValues {
    String Text  = {};
    Path   Asset = {};

    auto SetText(String Value) -> void {
        Text = std::move(Value);
    }

    [[nodiscard]] auto GetText() const -> String {
        return Text;
    }
};

class YamlAssignmentTest : public testing::Test {
  protected:
    Path m_FilePath = std::filesystem::temp_directory_path() / "soulengine_yaml_assignment_test.yaml";

    auto SetUp() -> void override {
        entt::meta_factory<PersistedValues>{}
            .data<&PersistedValues::SetText, &PersistedValues::GetText>(entt::hashed_string{"text"}.value())
            .data<&PersistedValues::Asset>(entt::hashed_string{"asset"}.value());
        std::ofstream Output(m_FilePath);
        Output << "text: directional\nasset: meshes/teapot.gltf\ninvalid: [1, 2]\n";
    }

    auto TearDown() -> void override {
        std::filesystem::remove(m_FilePath);
    }
};

TEST(YamlTypeIdentity, UsesExplicitPersistedTypeHashes) {
    EXPECT_EQ(entt::type_hash<String>::value(), entt::hashed_string::value("SoulEngine::String"));
    EXPECT_EQ(entt::type_hash<Path>::value(), entt::hashed_string::value("SoulEngine::Path"));
    EXPECT_EQ(entt::type_id<String>().hash(), entt::type_hash<String>::value());
    EXPECT_EQ(entt::type_id<Path>().hash(), entt::type_hash<Path>::value());
    EXPECT_NE(entt::type_hash<String>::value(), entt::type_hash<Path>::value());
}

TEST_F(YamlAssignmentTest, AssignsStringSetterAndPathAcrossYamlBoundary) {
    const auto Document = YamlDocument::Create(m_FilePath);
    ASSERT_TRUE(Document.has_value());
    auto       Component = entt::meta_any{PersistedValues{}};
    const auto Type      = entt::resolve<PersistedValues>();
    const auto Text      = Type.data(entt::hashed_string{"text"}.value());
    const auto Asset     = Type.data(entt::hashed_string{"asset"}.value());
    ASSERT_TRUE(Text);
    ASSERT_TRUE(Asset);
    ASSERT_EQ(Text.set_arg(0).info().hash(), entt::type_hash<String>::value());
    ASSERT_EQ(Asset.type().info().hash(), entt::type_hash<Path>::value());
    const auto TextNode  = Document->Root()["text"];
    const auto AssetNode = Document->Root()["asset"];
    ASSERT_TRUE(TextNode);
    ASSERT_TRUE(AssetNode);
    const auto TextResult = AssignYamlValue(Text, Component, TextNode->get());
    ASSERT_TRUE(TextResult.has_value());
    const auto AssetResult = AssignYamlValue(Asset, Component, AssetNode->get());
    ASSERT_TRUE(AssetResult.has_value());
    const auto* Values = Component.try_cast<PersistedValues>();
    ASSERT_NE(Values, nullptr);
    EXPECT_EQ(Values->Text, "directional");
    EXPECT_EQ(Values->Asset, Path{"meshes/teapot.gltf"});
}

TEST_F(YamlAssignmentTest, RejectsSequenceWithoutChangingString) {
    const auto Document = YamlDocument::Create(m_FilePath);
    ASSERT_TRUE(Document.has_value());
    auto       Component = entt::meta_any{PersistedValues{.Text = "original"}};
    const auto Field     = entt::resolve<PersistedValues>().data(entt::hashed_string{"text"}.value());
    const auto Invalid   = Document->Root()["invalid"];
    ASSERT_TRUE(Invalid);
    const auto Result = AssignYamlValue(Field, Component, Invalid->get());
    EXPECT_FALSE(Result.has_value());
    const auto* Values = Component.try_cast<PersistedValues>();
    ASSERT_NE(Values, nullptr);
    EXPECT_EQ(Values->Text, "original");
}

} // namespace
