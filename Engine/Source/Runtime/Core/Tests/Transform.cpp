#include <gtest/gtest.h>

#include <hlsl++.h>

import Core;

using namespace SoulEngine;

TEST(TransformTest, BuildsLocalMatrixFromTranslationRotationAndScale) {
    const Transform Transform{
        .Translation = hlslpp::float3(1.25f, -2.0f, 3.5f),
        .Rotation    = hlslpp::float3(0.0f, 0.0f, 0.0f),
        .Scale       = hlslpp::float3(2.0f, 3.0f, 4.0f),
    };

    const auto Matrix = Transform.GetLocalMatrix();
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[0].x), 2.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[1].y), 3.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[2].z), 4.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[3].x), 1.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[3].y), -2.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[3].z), 3.5f);
}
