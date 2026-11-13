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
CVar<String>  CVarTestOptions{"Test.Options.Mode", "Safe", "test ordered options", {"Safe", "Fast"}};
CVar<Int32>   CVarTestIntOptions{"Test.Options.Level", 1, "test int options", {1, 2, 3}};

enum class TestCVarMode : Uint8 { Safe, Fast };
CVar CVarTestEnum{"Test.Enum.Mode", TestCVarMode::Safe, "test enum options"};

// Rejected registrations: a default outside the declared options, an empty option list, and an unnamed
// enum default are fatal errors recorded at registration; the names stay unregistered and the handles
// serve their declared defaults.
CVar<String> CVarTestInvalidOptionDefault{
    "Test.Options.InvalidDefault", "Unsafe", "test invalid option default", {"Safe", "Fast"}};
CVar<Int32> CVarTestRejectedOptions{"Test.Rejected.Options", 3, "rejected: empty options", std::span<const Int32>{}};
CVar        CVarTestRejectedEnum{"Test.Rejected.Enum", static_cast<TestCVarMode>(99), "rejected: unnamed default"};

// Same name registered twice: the first registration owns the name, the duplicate is a fatal recorded
// error, and the duplicate's handle serves its declared default.
CVar<bool>  CVarTestSharedNameBool{"Test.SharedName", false, "bool of the shared name"};
CVar<Int32> CVarTestSharedNameInt{"Test.SharedName", 7, "int of the shared name"};

// Dedicated to the LoadConfig sweep test so other tests never observe swept values.
CVar<Int32> CVarTestSweepInt{"Test.Sweep.Int", 1, "sweep target"};
CVar<bool>  CVarTestSweepBool{"Test.Sweep.Bool", false, "sweep target"};

CVar<String> CVarTestConcurrentStringA{"Test.Concurrent.StringA", "initial", "concurrent string A"};
CVar<String> CVarTestConcurrentStringB{"Test.Concurrent.StringB", "initial", "concurrent string B"};

/// @brief Read one recorded registration error by substring.
[[nodiscard]] auto HasRegistrationError(StringView Fragment) -> bool {
    return std::ranges::any_of(CVarManager::Get().RegistrationErrors(),
                               [Fragment](const String& Error) -> bool { return Error.contains(Fragment); });
}

TEST(CVarTest, ForEachEntryVisitsEveryRegisteredEntry) {
    std::set<String> Names = {};
    CVarManager::Get().ForEachEntry([&](const StringView Name, const EntryBase& Entry) -> void {
        Names.emplace(String{Name});
        static_cast<void>(Entry);
    });
    EXPECT_TRUE(Names.contains("Test.Int"));
    EXPECT_TRUE(Names.contains("Test.Options.Level"));
    EXPECT_TRUE(Names.contains("Test.Checkbox"));
    EXPECT_TRUE(Names.contains("Test.Enum.Mode"));
    EXPECT_FALSE(Names.contains("Test.DoesNotExist"));
}

TEST(CVarTest, OptionBearersExposeTheirOptionsThroughTheErasedInterface) {
    const EntryBase* StringEntry = nullptr;
    const EntryBase* EnumEntry   = nullptr;
    CVarManager::Get().ForEachEntry([&](const StringView Name, EntryBase& Entry) -> void {
        if (Name == CVarTestOptions.Name())
            StringEntry = &Entry;
        if (Name == CVarTestEnum.Name())
            EnumEntry = &Entry;
    });
    ASSERT_NE(StringEntry, nullptr);
    EXPECT_EQ(StringEntry->OptionNames(), std::vector<String>({"Safe", "Fast"}));

    ASSERT_NE(EnumEntry, nullptr);
    EXPECT_EQ(EnumEntry->OptionNames(), std::vector<String>({"Safe", "Fast"}));
}

TEST(CVarTest, RejectedRegistrationsLeaveNamesUnregisteredAndHandlesInert) {
    ASSERT_FALSE(CVarManager::Get().RegistrationErrors().empty());
    EXPECT_TRUE(HasRegistrationError("Test.Rejected.Options"));
    EXPECT_TRUE(HasRegistrationError("Test.Rejected.Enum"));
    EXPECT_TRUE(HasRegistrationError("Test.Options.InvalidDefault"));
    EXPECT_TRUE(HasRegistrationError("Test.SharedName"));

    EXPECT_FALSE(CVarManager::Get().Find<Int32>("Test.Rejected.Options").has_value());
    EXPECT_FALSE(CVarManager::Get().SetByName<Int32>("Test.Rejected.Options", 5));
    CVarTestRejectedOptions.Set(5);
    EXPECT_FALSE(CVarManager::Get().Find<Int32>("Test.Rejected.Options").has_value());
    EXPECT_EQ(CVarTestRejectedOptions.Get(), 3);

    EXPECT_FALSE(CVarManager::Get().Find<TestCVarMode>("Test.Rejected.Enum").has_value());
    CVarTestRejectedEnum.Set(TestCVarMode::Safe);
    EXPECT_EQ(CVarTestRejectedEnum.Get(), static_cast<TestCVarMode>(99));

    EXPECT_FALSE(CVarManager::Get().Find<String>("Test.Options.InvalidDefault").has_value());
    EXPECT_EQ(CVarTestInvalidOptionDefault.Get(), "Unsafe");
}

TEST(CVarTest, DuplicateNamesKeepFirstRegistrationAndRejectTheRest) {
    EXPECT_EQ(CVarManager::Get().Find<bool>("Test.SharedName"), std::optional{false});
    EXPECT_FALSE(CVarManager::Get().Find<Int32>("Test.SharedName").has_value());

    CVarTestSharedNameInt.Set(9);
    EXPECT_EQ(CVarTestSharedNameInt.Get(), 7);
    EXPECT_EQ(CVarTestSharedNameBool.Get(), false);
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

TEST(CVarTest, OptionsRejectInvalidValuesOnEveryWritePath) {
    CVarManager::Get().ClearRegistrationErrors();
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
    CVarManager::Get().ClearRegistrationErrors();
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

TEST(CVarTest, EnumValuedCVarRoundTripsTypedValues) {
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Safe);

    CVarTestEnum.Set(TestCVarMode::Fast);
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Fast);
    EXPECT_EQ(CVarManager::Get().Find<TestCVarMode>(CVarTestEnum.Name()), std::optional{TestCVarMode::Fast});

    ASSERT_TRUE(CVarManager::Get().SetByName<TestCVarMode>(CVarTestEnum.Name(), TestCVarMode::Safe));
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Safe);

    CVarTestEnum.Set(static_cast<TestCVarMode>(99));
    EXPECT_EQ(CVarTestEnum.Get(), TestCVarMode::Safe);
}

TEST(CVarTest, LoadConfigAppliesMatchingLeavesAndToleratesUnknownKeys) {
    CVarManager::Get().ClearRegistrationErrors();
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
