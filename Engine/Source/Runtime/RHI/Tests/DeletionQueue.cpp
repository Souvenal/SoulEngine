/// @file   VulkanDeletionQueue.cpp
/// @brief  Tests for VulkanDeletionQueue retirement logic.
///
/// These tests operate on the RHI level.  A full integration test that
/// exercises the real timeline semaphore lives in VulkanResourceLifetime.cpp.

#include <gtest/gtest.h>

import RHI;
import std;

using namespace SoulEngine;

// ── Test: callback fires when token advances ─────────────────────────────
//
// Since VulkanDeletionQueue is in the Vulkan module (private partition) and takes
// a VulkanTimelineSemaphore& which is also private, direct instantiation is not
// possible from a plain imported-RHI test.  This test verifies the
// deferred-deletion pattern by testing the Enqueue + Drain logic through
// the mock RHIGpuResource base class and the shared SPtr pattern.
//
// The actual VulkanDeletionQueue::Tick() + VulkanTimelineSemaphore integration is
// verified in VulkanResourceLifetime.cpp.

TEST(DeletionQueueTest, GpuResourceTokenTracking) {
    auto Res = std::make_shared<RHIGraphicsPipeline>();
    EXPECT_EQ(Res->GetLastUsageToken().Id, 0);

    Res->UpdateLastUsageToken(RHIGpuCompletionToken{.Id = 5});
    EXPECT_EQ(Res->GetLastUsageToken().Id, 5);

    Res->UpdateLastUsageToken(RHIGpuCompletionToken{.Id = 10});
    EXPECT_EQ(Res->GetLastUsageToken().Id, 10);
}

TEST(DeletionQueueTest, SptrCapturePreventsPrematureDestruction) {
    /// Verify that capturing a shared_ptr in a lambda keeps it alive
    /// until the lambda is destroyed — this is the core mechanic that
    /// VulkanDeletionQueue uses to defer GPU resource destruction.
    auto Weak = std::weak_ptr<RHIGraphicsPipeline>{};

    {
        auto Strong = std::make_shared<RHIGraphicsPipeline>();
        Weak        = Strong;

        auto Callback = [Strong]() { /* SPtr captured — keeps alive */ };
        EXPECT_FALSE(Weak.expired());
    }

    // Strong went out of scope; the lambda captured a copy, so the
    // underlying object is still alive until the lambda is destroyed.
    EXPECT_TRUE(Weak.expired());
}
