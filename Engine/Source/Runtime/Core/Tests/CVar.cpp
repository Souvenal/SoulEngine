/// @file   CVar.cpp
/// @brief  Tests for the CVar registration/access/TOML-sweep machinery.

#include <gtest/gtest.h>

import Core;
import std;

using namespace SoulEngine;

// Namespace-scope registration: these self-register with CVarManager at static-init time, before any TEST
// runs.
CVar<bool>    CVarTestCheckbox{"Test.Checkbox", false, "test bool"};
CVar<Int32>   CVarTestInt{"Test.Int", 42, "test int"};
CVar<Float32> CVarTestFloat{"Test.Float", 1.5F, "test float"};
CVar<String>  CVarTestString{"Test.String", "hello", "test string"};

// Dedicated to the LoadConfig sweep test so other tests never observe swept values.
CVar<Int32> CVarTestSweepInt{"Test.Sweep.Int", 1, "sweep target"};
CVar<bool>  CVarTestSweepBool{"Test.Sweep.Bool", false, "sweep target"};

// Same name registered under two different types — legal: the registry key is (name, type).
CVar<bool>  CVarTestSharedNameBool{"Test.SharedName", false, "bool of the shared name"};
CVar<Int32> CVarTestSharedNameInt{"Test.SharedName", 7, "int of the shared name"};

TEST(CVarTest, HandleReturnsRegisteredDefault) {
    EXPECT_FALSE(CVarTestCheckbox.Get());
    EXPECT_EQ(CVarTestInt.Get(), 42);
    EXPECT_FLOAT_EQ(CVarTestFloat.Get(), 1.5F);
    EXPECT_EQ(CVarTestString.Get(), "hello");
}

TEST(CVarTest, FindByNameReturnsCurrentValue) {
    const auto Value = CVarManager::Get().Find<Int32>("Test.Int");
    ASSERT_TRUE(Value.has_value());
    EXPECT_EQ(*Value, 42);
}

TEST(CVarTest, FindUnregisteredNameReturnsNullopt) {
    EXPECT_FALSE(CVarManager::Get().Find<Int32>("Test.DoesNotExist").has_value());
    EXPECT_FALSE(CVarManager::Get().Find<String>("Test.DoesNotExist").has_value());
}

TEST(CVarTest, FindTypeMismatchReturnsNullopt) {
    // "Test.Int" exists but is an Int32, not a bool/String.
    EXPECT_FALSE(CVarManager::Get().Find<bool>("Test.Int").has_value());
    EXPECT_FALSE(CVarManager::Get().Find<String>("Test.Int").has_value());
}

TEST(CVarTest, SetByNameRoundTripsThroughHandle) {
    ASSERT_TRUE(CVarManager::Get().SetByName<Int32>("Test.Int", 7));
    EXPECT_EQ(CVarTestInt.Get(), 7);
    EXPECT_EQ(CVarManager::Get().Find<Int32>("Test.Int"), std::optional{7});

    // Restore the default so test order cannot leak state.
    CVarTestInt.Set(42);
    EXPECT_EQ(CVarManager::Get().Find<Int32>("Test.Int"), std::optional{42});
}

TEST(CVarTest, SetByNameRejectsUnknownAndMismatchedNames) {
    EXPECT_FALSE(CVarManager::Get().SetByName<Int32>("Test.DoesNotExist", 1));
    EXPECT_FALSE(CVarManager::Get().SetByName<bool>("Test.Int", true));
}

TEST(CVarTest, HandleSetIsVisibleThroughFind) {
    CVarTestString.Set("world");
    EXPECT_EQ(CVarManager::Get().Find<String>("Test.String"), std::optional{String{"world"}});
    CVarTestString.Set("hello");
}

TEST(CVarTest, SameNameDifferentTypesCoexistIndependently) {
    EXPECT_EQ(CVarManager::Get().Find<bool>("Test.SharedName"), std::optional{false});
    EXPECT_EQ(CVarManager::Get().Find<Int32>("Test.SharedName"), std::optional{7});

    CVarTestSharedNameInt.Set(9);
    EXPECT_FALSE(CVarTestSharedNameBool.Get());
    EXPECT_EQ(CVarTestSharedNameInt.Get(), 9);
    CVarTestSharedNameInt.Set(7);
}

TEST(CVarTest, LoadConfigAppliesMatchingLeavesAndToleratesUnknownKeys) {
    const auto TempDir = std::filesystem::temp_directory_path() / "SoulEngineCVarTest";
    std::filesystem::create_directories(TempDir);
    const auto ConfigPath = TempDir / "cvar_test.toml";
    {
        std::ofstream Out{ConfigPath};
        Out << "[Test.Sweep]\n"
               "Int = 99\n"
               "Bool = true\n"
               "Unknown = 5\n" // no registered CVar: warned, skipped
               "\n"
               "[Elsewhere]\n" // unknown table: warned, skipped
               "Key = \"value\"\n";
    }

    const auto Result = CVarManager::Get().LoadConfig(ConfigPath);
    ASSERT_TRUE(Result.has_value());

    EXPECT_EQ(CVarTestSweepInt.Get(), 99);
    EXPECT_TRUE(CVarTestSweepBool.Get());
    // Unknown keys must not have created anything.
    EXPECT_FALSE(CVarManager::Get().Find<Int32>("Test.Sweep.Unknown").has_value());
    EXPECT_FALSE(CVarManager::Get().Find<String>("Elsewhere.Key").has_value());

    std::filesystem::remove_all(TempDir);
}

TEST(CVarTest, LoadConfigReportsParseErrors) {
    const auto Result = CVarManager::Get().LoadConfig("definitely/not/a/real/file.toml");
    EXPECT_FALSE(Result.has_value());
}
