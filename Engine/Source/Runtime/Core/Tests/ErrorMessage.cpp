/// @file   ErrorMessage.cpp
/// @brief  Tests for structured error message rendering.

#include <gtest/gtest.h>

import Core;
import std;

using namespace SoulEngine;

TEST(ErrorMessageTest, LeafErrorRendersAsIs) {
    const ErrorMessage Error{"Leaf failure"};

    EXPECT_TRUE(Error.ToString().starts_with("Leaf failure\n[at "));
    EXPECT_TRUE(Error.ToString().contains(__FILE__));
}

TEST(ErrorMessageTest, AppendedContextRendersOutermostFirst) {
    const ErrorMessage Error = ErrorMessage{"Slang compile failed"}
                                   .Append("Vertex shader compilation failed")
                                   .Append("Application init failed");

    const String Rendered = Error.ToString();

    // Outermost first, then progressively indented causes; every layer's source location sits on a following
    // line at that layer's indentation.
    EXPECT_TRUE(Rendered.starts_with("Application init failed\n[at "));
    EXPECT_TRUE(Rendered.contains("\n  Caused by: Vertex shader compilation failed\n  [at "));
    EXPECT_TRUE(Rendered.contains("\n    Caused by: Slang compile failed\n    [at "));
}

TEST(ErrorMessageTest, FormattedMessagesRenderSafely) {
    const ErrorMessage Error{Format("Failed to load '{}'", "Shader.slang")};

    EXPECT_TRUE(Error.ToString().starts_with("Failed to load 'Shader.slang'\n[at "));
}
