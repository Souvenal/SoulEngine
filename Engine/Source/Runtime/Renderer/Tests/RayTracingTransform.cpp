#include <gtest/gtest.h>
#include <hlsl++.h>

import Renderer;

using namespace SoulEngine;

namespace {

TEST(RayTracingTransform, ConvertsSceneTranslationToInstanceFinalColumn) {
    const auto Matrix = hlslpp::mul(
        hlslpp::float4x4::rotation_y(0.5f), hlslpp::float4x4::translation(hlslpp::float3(1.25f, -2.0f, 3.5f)));
    const auto Transform = ToAccelerationStructureInstanceTransform(Matrix);

    EXPECT_FLOAT_EQ(Transform.M00, static_cast<float>(Matrix[0].x));
    EXPECT_FLOAT_EQ(Transform.M01, static_cast<float>(Matrix[1].x));
    EXPECT_FLOAT_EQ(Transform.M02, static_cast<float>(Matrix[2].x));
    EXPECT_FLOAT_EQ(Transform.M10, static_cast<float>(Matrix[0].y));
    EXPECT_FLOAT_EQ(Transform.M11, static_cast<float>(Matrix[1].y));
    EXPECT_FLOAT_EQ(Transform.M12, static_cast<float>(Matrix[2].y));
    EXPECT_FLOAT_EQ(Transform.M20, static_cast<float>(Matrix[0].z));
    EXPECT_FLOAT_EQ(Transform.M21, static_cast<float>(Matrix[1].z));
    EXPECT_FLOAT_EQ(Transform.M22, static_cast<float>(Matrix[2].z));
    EXPECT_FLOAT_EQ(Transform.M03, 1.25f);
    EXPECT_FLOAT_EQ(Transform.M13, -2.0f);
    EXPECT_FLOAT_EQ(Transform.M23, 3.5f);
}

} // namespace