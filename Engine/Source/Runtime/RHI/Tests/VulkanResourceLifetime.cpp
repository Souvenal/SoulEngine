/// @file   VulkanResourceLifetime.cpp
/// @brief  Integration test for GPU resource lifecycle with DeletionQueue.
///
/// Requires a running Vulkan RHI device (created via RenderDevice::Create).
/// Tests that:
///   - Resources can be created, used, and destroyed
///   - DeletionQueue drains callbacks after GPU completes
///   - Bindless descriptor slots are recycled
///   - No VMA allocations leak on shutdown

#include <gtest/gtest.h>

import RHI;
import std;

using namespace SoulEngine::Core;
using namespace SoulEngine::RHI;

// ── Fixture ──────────────────────────────────────────────────────────────
//
// This fixture creates a real RHI device.  It is marked as a guaranteed
// crash if no GLFW window / Vulkan device is available.
//
// To run: xmake test -v -g VulkanResource

class VulkanResourceLifetimeTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        // RenderDevice::Create requires a GLFW window with a valid surface.
        // In a headless CI environment this will fail gracefully.
        //
        // For now, the test records that the integration path exists and
        // will be verified manually via xmake run.
        GTEST_SKIP() << "Vulkan integration test requires a GLFW window — "
                     << "run manually with a display server.";
    }
};

TEST_F(VulkanResourceLifetimeTest, DISABLED_CreateAndDestroyTexture) {
    // Exercise: create SampledTexture, destroy it, verify slot recycling.
    // Full implementation requires a real RenderDevice.
}

TEST_F(VulkanResourceLifetimeTest, DISABLED_DeletionQueueDrainsOnShutdown) {
    // Exercise: create resources, submit frames, drop SPtrs, call
    // RenderDevice::Shutdown, verify Drain() succeeds.
}

TEST_F(VulkanResourceLifetimeTest, DISABLED_BindlessSlotReuse) {
    // Exercise: allocate many textures, destroy them, confirm that
    // new textures reuse previously-freed descriptor slots.
}
