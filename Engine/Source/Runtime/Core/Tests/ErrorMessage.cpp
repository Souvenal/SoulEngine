/// @file   ErrorMessage.cpp
/// @brief  Tests for structured error message rendering.

#include <gtest/gtest.h>

import Core;
import std;

using namespace SoulEngine::Core;

TEST(ErrorMessageTest, LeafErrorRendersAsIs) {
    const ErrorMessage Error{"Leaf failure"};

    EXPECT_TRUE(Error.ToString().starts_with("Leaf failure"));
}

TEST(ErrorMessageTest, AppendedContextRendersOutermostFirst) {
    const ErrorMessage Error = ErrorMessage{"Slang compile failed"}
                                   .Append("Vertex shader compilation failed")
                                   .Append("Application init failed");

    constexpr std::string_view Expected = "Application init failed\n"
                                          "  Caused by: Vertex shader compilation failed\n"
                                          "    Caused by: Slang compile failed";

    EXPECT_TRUE(Error.ToString().starts_with(Expected));
}

TEST(ErrorMessageTest, FormattedMessagesRenderSafely) {
    const ErrorMessage Error{Format("Failed to load '{}'", "Shader.slang")};

    EXPECT_TRUE(Error.ToString().starts_with("Failed to load 'Shader.slang'"));
}
