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
CVar<String>  CVarTestOptions{"Test.Options.Mode", "Safe", "test ordered options", std::vector<String>{"Safe", "Fast"}};
CVar<Int32>   CVarTestIntOptions{"Test.Options.Level", 1, "test int options", std::vector<Int32>{1, 2, 3}};

enum class TestCVarMode : Uint8 { Unknown = 0, Safe, Fast };
CVar<TestCVarMode> CVarTestEnum{
    "Test.Enum.Mode", TestCVarMode::Safe, "test enum options", {TestCVarMode::Safe, TestCVarMode::Fast}};
CVar<TestCVarMode> CVarTestEmptyEnumOptions{
    "Test.Enum.EmptyOptions", TestCVarMode::Unknown, "test empty enum options", std::vector<TestCVarMode>{}};
CVar<TestCVarMode> CVarTestUnnamedEnumOptions{"Test.Enum.UnnamedOptions",
                                              TestCVarMode::Safe,
                                              "test unnamed options",
                                              std::vector<TestCVarMode>{static_cast<TestCVarMode>(99)}};
CVar<TestCVarMode> CVarTestUnnamedEnumDefault{
    "Test.Enum.UnnamedDefault", static_cast<TestCVarMode>(99), "test unnamed default", {TestCVarMode::Safe}};
CVar<String> CVarTestInvalidOptionDefault{
    "Test.Options.InvalidDefault", "Unsafe", "test invalid option default", std::vector<String>{"Safe", "Fast"}};

// Dedicated to the LoadConfig sweep test so other tests never observe swept values.
CVar<Int32> CVarTestSweepInt{"Test.Sweep.Int", 1, "sweep target"};
CVar<bool>  CVarTestSweepBool{"Test.Sweep.Bool", false, "sweep target"};

// Same name registered under two different types — legal: the registry key is (name, type).
CVar<bool>  CVarTestSharedNameBool{"Test.SharedName", false, "bool of the shared name"};
CVar<Int32> CVarTestSharedNameInt{"Test.SharedName", 7, "int of the shared name"};

CVar<String> CVarTestConcurrentStringA{"Test.Concurrent.StringA", "initial", "concurrent string A"};
CVar<String> CVarTestConcurrentStringB{"Test.Concurrent.StringB", "initial", "concurrent string B"};

/// @brief Find one entry of type T by dotted name through the ForEach traversal.
template <CVarStorage T>
[[nodiscard]] auto FindEntry(StringView Name) -> const CVarEntry<T>* {
    const CVarEntry<T>* Found = nullptr;
    CVarManager::Get().ForEach<T>([&](const StringView EntryName, const CVarEntry<T>& Entry) -> void {
        if (EntryName == Name)
            Found = &Entry;
    });
    return Found;
}

TEST(CVarTest, ForEachVisitsEveryRegisteredEntryOfAStorageType) {
    std::set<String> Names = {};
    CVarManager::Get().ForEach<Int32>(
        [&](const StringView Name, const CVarEntry<Int32>&) -> void { Names.emplace(String{Name}); });
    EXPECT_TRUE(Names.contains("Test.Int"));
    EXPECT_TRUE(Names.contains("Test.Options.Level"));
    EXPECT_FALSE(Names.contains("Test.Checkbox"));

    const auto* Entry = FindEntry<Int32>("Test.Int");
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Description, "test int");
    EXPECT_EQ(CVarManager::Get().LoadEntry(*Entry), 42);
    EXPECT_EQ(Entry->Timing, CVarTiming::Runtime);
    EXPECT_TRUE(Entry->Options.empty());
    EXPECT_FALSE(Entry->IsOptionBearing);
}

TEST(CVarTest, EntriesCarryDeclaredOptionsInRegistrationOrder) {
    const auto* StringEntry = FindEntry<String>(CVarTestOptions.Name());
    ASSERT_NE(StringEntry, nullptr);
    EXPECT_TRUE(StringEntry->IsOptionBearing);
    EXPECT_EQ(StringEntry->Options, std::vector<String>({"Safe", "Fast"}));

    const auto* IntEntry = FindEntry<Int32>(CVarTestIntOptions.Name());
    ASSERT_NE(IntEntry, nullptr);
    EXPECT_TRUE(IntEntry->IsOptionBearing);
    EXPECT_EQ(IntEntry->Options, std::vector<Int32>({1, 2, 3}));
}

TEST(CVarTest, OptionRegistrationUsesFirstOptionForInvalidDefault) {
    EXPECT_EQ(CVarTestInvalidOptionDefault.Get(), "Safe");

    const auto* Record = FindEntry<String>(CVarTestInvalidOptionDefault.Name());
    ASSERT_NE(Record, nullptr);
    EXPECT_EQ(CVarManager::Get().LoadEntry(*Record), "Safe");
    EXPECT_EQ(Record->Options, std::vector<String>({"Safe", "Fast"}));
}

TEST(CVarTest, OptionRegistrationNormalizesAnUnnamedEnumDefaultToTheFirstOption) {
    // The default (unnamed enumerator 99) is outside the declared options, so the first option becomes the
    // initial value; the unnamed original is never stored.
    EXPECT_EQ(CVarTestUnnamedEnumDefault.Get(), TestCVarMode::Safe);

    const auto* Record = FindEntry<String>(CVarTestUnnamedEnumDefault.Name());
    ASSERT_NE(Record, nullptr);
    EXPECT_EQ(CVarManager::Get().LoadEntry(*Record), "Safe");
    EXPECT_EQ(Record->Options, std::vector<String>({"Safe"}));
}

TEST(CVarTest, EnumRegistrationsWithoutLegalOptionsRejectEveryWrite) {
    const std::array Names = {CVarTestEmptyEnumOptions.Name(), CVarTestUnnamedEnumOptions.Name()};
    for (const auto Name : Names) {
        EXPECT_FALSE(CVarManager::Get().SetByName<String>(Name, "Unknown"));
        EXPECT_FALSE(CVarManager::Get().SetByName<String>(Name, "Fast"));
        EXPECT_FALSE(CVarManager::Get().SetByName<String>(Name, "Arbitrary"));
    }
    const auto* EmptyRecord = FindEntry<String>(CVarTestEmptyEnumOptions.Name());
    ASSERT_NE(EmptyRecord, nullptr);
    EXPECT_EQ(CVarManager::Get().LoadEntry(*EmptyRecord), "Unknown");
    EXPECT_TRUE(EmptyRecord->Options.empty());

    const auto* UnnamedRecord = FindEntry<String>(CVarTestUnnamedEnumOptions.Name());
    ASSERT_NE(UnnamedRecord, nullptr);
    EXPECT_EQ(CVarManager::Get().LoadEntry(*UnnamedRecord), "Safe");
    EXPECT_TRUE(UnnamedRecord->Options.empty());
}

TEST(CVarTest, ConcurrentStringReadsAndWritesPreserveWholeValues) {
    const String First(256, 'a');
    const String Second(4096, 'b');
    CVarTestConcurrentStringA.Set(First);
    CVarTestConcurrentStringB.Set(First);
    std::atomic<bool>   IsValid    = true;
    std::atomic<bool>   IsFinished = false;
    std::atomic<Uint64> WritesA    = 0;
    std::atomic<Uint64> WritesB    = 0;
    std::barrier        Start(3);
    {
        std::jthread WriterA([&]() -> void {
            Start.arrive_and_wait();
            while (!IsFinished.load()) {
                CVarTestConcurrentStringA.Set(WritesA.load() % 2 == 0 ? First : Second);
                WritesA.fetch_add(1);
            }
        });
        std::jthread WriterB([&]() -> void {
            Start.arrive_and_wait();
            while (!IsFinished.load()) {
                if (!CVarManager::Get().SetByName<String>(CVarTestConcurrentStringB.Name(),
                                                          WritesB.load() % 2 == 0 ? First : Second))
                    IsValid.store(false);
                WritesB.fetch_add(1);
            }
        });
        Start.arrive_and_wait();
        for (int Index = 0; Index < 1000; ++Index) {
            const auto PreviousA = WritesA.load();
            const auto PreviousB = WritesB.load();
            while (WritesA.load() == PreviousA || WritesB.load() == PreviousB)
                std::this_thread::yield();
            const auto HandleValue = CVarTestConcurrentStringA.Get();
            const auto FoundValue  = CVarManager::Get().Find<String>(CVarTestConcurrentStringB.Name());
            if ((HandleValue != First && HandleValue != Second) || !FoundValue ||
                (*FoundValue != First && *FoundValue != Second))
                IsValid.store(false);
            CVarManager::Get().ForEach<String>([&](const StringView Name, const CVarEntry<String>& Entry) -> void {
                if (Name != CVarTestConcurrentStringA.Name() && Name != CVarTestConcurrentStringB.Name())
                    return;
                if (const auto Value = CVarManager::Get().LoadEntry(Entry); Value != First && Value != Second)
                    IsValid.store(false);
            });
        }
        IsFinished.store(true);
    }
    EXPECT_TRUE(IsValid.load());
}

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

TEST(CVarTest, OptionsRejectInvalidValuesOnEveryWritePath) {
    ASSERT_TRUE(CVarManager::Get().SetByName<String>(CVarTestOptions.Name(), "Fast"));
    EXPECT_EQ(CVarTestOptions.Get(), "Fast");

    EXPECT_FALSE(CVarManager::Get().SetByName<String>(CVarTestOptions.Name(), "Unsafe"));
    EXPECT_EQ(CVarTestOptions.Get(), "Fast");

    CVarTestOptions.Set("Unsafe");
    EXPECT_EQ(CVarTestOptions.Get(), "Fast");

    const auto TempDir = std::filesystem::temp_directory_path() / "SoulEngineCVarOptionsTest";
    std::filesystem::create_directories(TempDir);
    const auto ConfigPath = TempDir / "cvar_options_test.toml";
    {
        std::ofstream Out{ConfigPath};
        Out << "[Test.Options]\nMode = \"Unsafe\"\n";
    }
    ASSERT_TRUE(CVarManager::Get().LoadConfig(ConfigPath).has_value());
    EXPECT_EQ(CVarTestOptions.Get(), "Fast");

    {
        std::ofstream Out{ConfigPath};
        Out << "[Test.Options]\nMode = \"Safe\"\n";
    }
    ASSERT_TRUE(CVarManager::Get().LoadConfig(ConfigPath).has_value());
    EXPECT_EQ(CVarTestOptions.Get(), "Safe");

    std::filesystem::remove_all(TempDir);
}

TEST(CVarTest, NumericOptionsRejectInvalidValuesOnEveryWritePath) {
    ASSERT_TRUE(CVarManager::Get().SetByName<Int32>(CVarTestIntOptions.Name(), 2));
    EXPECT_EQ(CVarTestIntOptions.Get(), 2);

    EXPECT_FALSE(CVarManager::Get().SetByName<Int32>(CVarTestIntOptions.Name(), 5));
    EXPECT_EQ(CVarTestIntOptions.Get(), 2);

    CVarTestIntOptions.Set(9);
    EXPECT_EQ(CVarTestIntOptions.Get(), 2);

    const auto TempDir = std::filesystem::temp_directory_path() / "SoulEngineCVarIntOptionsTest";
    std::filesystem::create_directories(TempDir);
    const auto ConfigPath = TempDir / "cvar_int_options_test.toml";
    {
        std::ofstream Out{ConfigPath};
        Out << "[Test.Options]\nLevel = 9\n";
    }
    ASSERT_TRUE(CVarManager::Get().LoadConfig(ConfigPath).has_value());
    EXPECT_EQ(CVarTestIntOptions.Get(), 2);
    std::filesystem::remove_all(TempDir);

    // Restore the first option so test order cannot leak state.
    ASSERT_TRUE(CVarManager::Get().SetByName<Int32>(CVarTestIntOptions.Name(), 1));
}

TEST(CVarTest, EnumCVarReflectsOrderedNamesAndRoundTripsTypedValues) {
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Safe);

    const auto* Record = FindEntry<String>(CVarTestEnum.Name());
    ASSERT_NE(Record, nullptr);
    EXPECT_EQ(CVarManager::Get().LoadEntry(*Record), "Safe");
    EXPECT_EQ(Record->Options, std::vector<String>({"Safe", "Fast"}));

    CVarTestEnum.Set(TestCVarMode::Fast);
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Fast);
    EXPECT_EQ(CVarManager::Get().Find<String>(CVarTestEnum.Name()), std::optional{String{"Fast"}});
    EXPECT_EQ(CVarManager::Get().Find<TestCVarMode>(CVarTestEnum.Name()), std::optional{TestCVarMode::Fast});

    ASSERT_TRUE(CVarManager::Get().SetByName<String>(CVarTestEnum.Name(), "Safe"));
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Safe);

    CVarTestEnum.Set(TestCVarMode::Unknown);
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Safe);

    CVarTestEnum.Set(static_cast<TestCVarMode>(99));
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Safe);
}

TEST(CVarTest, LoadConfigAppliesMatchingLeavesAndToleratesUnknownKeys) {
    const auto TempDir = std::filesystem::temp_directory_path() / "SoulEngineCVarTest";
    std::filesystem::create_directories(TempDir);
    const auto ConfigPath = TempDir / "cvar_test.toml";
    {
        std::ofstream Out{ConfigPath};
        Out << R"toml([Test.Sweep]
Int = 99
Bool = true
Unknown = 5 # no registered CVar: warned, skipped

[Elsewhere] # unknown table: warned, skipped
Key = "value"
)toml";
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
