#include <gtest/gtest.h>
#include <entt/entt.hpp>
#include <hlsl++.h>

import std;
import Core;
import Scene;

using namespace SoulEngine;

TEST(LightYaml, AssignsRegisteredTypeThroughYamlImplementation) {
    const auto FilePath = std::filesystem::temp_directory_path() / "soulengine_light_yaml_test.yaml";
    {
        std::ofstream Output(FilePath);
        Output << "directional\n";
    }
    const auto Document = YamlDocument::Create(FilePath);
    std::filesystem::remove(FilePath);
    ASSERT_TRUE(Document.has_value());
    const auto* Field = FindField(LightComponentFields(), "type");
    ASSERT_NE(Field, nullptr);
    LightComponent Component = {};
    const auto     Result    = AssignYamlField(*Field, &Component, Document->Root());
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ(Component.Type, LightType::Directional);
}

TEST(LightYaml, LoadsDirectionalLightWithoutComponentWarnings) {
    const auto FilePath = std::filesystem::temp_directory_path() / "soulengine_light_scene_test.yaml";
    {
        std::ofstream Output(FilePath);
        Output << "entities:\n  - name: Sun\n    components:\n      light:\n"
                  "        type: directional\n        intensity: 100000.0\n";
    }
    const auto Loaded = Scene::LoadFromFile(FilePath);
    std::filesystem::remove(FilePath);
    ASSERT_TRUE(Loaded.has_value());
    ASSERT_TRUE(Loaded->second.Warnings.empty());
    const auto Lights = Loaded->first->GetRegistry().view<LightComponent>();
    ASSERT_NE(Lights.begin(), Lights.end());
    const auto& Light = Lights.get<LightComponent>(*Lights.begin());
    EXPECT_EQ(Light.Type, LightType::Directional);
    EXPECT_FLOAT_EQ(Light.Intensity, 100000.0f);
}
