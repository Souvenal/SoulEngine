#include <gtest/gtest.h>

import std;
import Core;

using namespace SoulEngine;

struct PersistedValues {
    String Text  = {};
    Path   Asset = {};

    auto SetText(String Value) -> void {
        Text = std::move(Value);
    }

    [[nodiscard]] auto GetText() const -> String {
        return Text;
    }

    static constexpr std::array Fields = {
        Field<&PersistedValues::SetText, &PersistedValues::GetText>("text"),
        Field<&PersistedValues::Asset>("asset"),
    };
};

class YamlAssignmentTest : public testing::Test {
  protected:
    Path m_FilePath = std::filesystem::temp_directory_path() / "soulengine_yaml_assignment_test.yaml";

    auto SetUp() -> void override {
        std::ofstream Output(m_FilePath);
        Output << "text: directional\nasset: meshes/teapot.gltf\ninvalid: [1, 2]\n";
    }

    auto TearDown() -> void override {
        std::filesystem::remove(m_FilePath);
    }
};

TEST_F(YamlAssignmentTest, AssignsStringSetterAndPathAcrossYamlBoundary) {
    const auto Document = YamlDocument::Create(m_FilePath);
    ASSERT_TRUE(Document.has_value());
    PersistedValues Values = {};
    const auto      Fields = FieldTable{PersistedValues::Fields};
    const auto*     Text   = FindField(Fields, "text");
    const auto*     Asset  = FindField(Fields, "asset");
    ASSERT_NE(Text, nullptr);
    ASSERT_NE(Asset, nullptr);
    const auto TextNode  = Document->Root()["text"];
    const auto AssetNode = Document->Root()["asset"];
    ASSERT_TRUE(TextNode);
    ASSERT_TRUE(AssetNode);
    EXPECT_TRUE(AssignYamlField(*Text, &Values, TextNode->get()).has_value());
    EXPECT_TRUE(AssignYamlField(*Asset, &Values, AssetNode->get()).has_value());
    EXPECT_EQ(Values.Text, "directional");
    EXPECT_EQ(Values.Asset, Path{"meshes/teapot.gltf"});
}

TEST_F(YamlAssignmentTest, RejectsSequenceWithoutChangingString) {
    const auto Document = YamlDocument::Create(m_FilePath);
    ASSERT_TRUE(Document.has_value());
    PersistedValues Values{.Text = "original"};
    const auto*     Field = FindField(PersistedValues::Fields, "text");
    ASSERT_NE(Field, nullptr);
    const auto Invalid = Document->Root()["invalid"];
    ASSERT_TRUE(Invalid);
    EXPECT_FALSE(AssignYamlField(*Field, &Values, Invalid->get()).has_value());
    EXPECT_EQ(Values.Text, "original");
}
