#include <gtest/gtest.h>

import std;
import Core;
import Material;

using namespace SoulEngine;

namespace {

class MaterialYamlFieldsTest : public testing::Test {
  protected:
    Path m_FilePath = std::filesystem::temp_directory_path() / "soulengine_material_yaml_fields_test.yaml";

    auto TearDown() -> void override {
        std::filesystem::remove(m_FilePath);
    }

    [[nodiscard]] auto WriteDocument(StringView Content) -> std::expected<YamlDocument, ErrorMessage> {
        {
            std::ofstream Output(m_FilePath);
            Output << Content;
        }
        return YamlDocument::Create(m_FilePath);
    }
};

TEST_F(MaterialYamlFieldsTest, AssignsPbrFactorThroughDescriptor) {
    const auto Document = WriteDocument("0.9\n");
    ASSERT_TRUE(Document.has_value());
    MaterialYamlRecord Value     = {};
    const auto*        Roughness = FindField(MaterialYamlRecordFields(), "roughness_factor");
    ASSERT_NE(Roughness, nullptr);
    ASSERT_TRUE(AssignYamlField(*Roughness, &Value, Document->Root()).has_value());
    EXPECT_FLOAT_EQ(Value.RoughnessFactor, 0.9f);
}

TEST_F(MaterialYamlFieldsTest, BridgesEnumFieldThroughString) {
    const auto Document = WriteDocument("additive\n");
    ASSERT_TRUE(Document.has_value());
    MaterialYamlRecord Value = {};
    const auto*        Field = FindField(MaterialYamlRecordFields(), "blend_function");
    ASSERT_NE(Field, nullptr);
    ASSERT_TRUE(AssignYamlField(*Field, &Value, Document->Root()).has_value());
    EXPECT_EQ(Value.BlendFunction, MaterialBlendFunction::Additive);
    EXPECT_EQ(std::get<String>(Field->Get(&Value)), "Additive");
}

TEST_F(MaterialYamlFieldsTest, RejectsUnknownFieldName) {
    EXPECT_EQ(FindField(MaterialYamlRecordFields(), "not_a_field"), nullptr);
}

} // namespace
