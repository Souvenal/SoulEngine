/// @file   CommandSync.cpp
/// @brief  Vulkan synchronization mapping for geometry input and upload accesses.

#include <gtest/gtest.h>

import RHI;
import RHIVulkan.Capability;
import RHIVulkan.Staging;
import RHIVulkan.Types;
import vulkan;
import std;

using namespace SoulEngine;

TEST(VulkanCommandSync, VertexInputMapsToNativeStageAndAccess) {
    const auto Stage = ToVkPipelineStages(RHISyncStage::VertexInput);
    EXPECT_EQ(Stage, vk::PipelineStageFlags2{vk::PipelineStageFlagBits2::eVertexInput});

    const auto VertexAccess = ToVkBufferAccess(RHIBufferUsage::Vertex, RHIAccess::Read);
    EXPECT_EQ(VertexAccess, vk::AccessFlags2{vk::AccessFlagBits2::eVertexAttributeRead});
    const auto IndexAccess = ToVkBufferAccess(RHIBufferUsage::Index, RHIAccess::Read);
    EXPECT_EQ(IndexAccess, vk::AccessFlags2{vk::AccessFlagBits2::eIndexRead});
}

TEST(VulkanCommandSync, UploadAccessKeepsAccelerationStructureReadForGeometry) {
    const bool RtAvailable         = VulkanCapability::Get().IsRayTracingAvailable();
    const auto CheckGeometryUpload = [&](RHIBufferUsage Usage, vk::AccessFlagBits2 DirectAccess) -> void {
        const auto Access = ToVkBufferUploadAccess(Usage);
        EXPECT_TRUE(static_cast<bool>(Access & DirectAccess));
        EXPECT_EQ(static_cast<bool>(Access & vk::AccessFlagBits2::eAccelerationStructureReadKHR), RtAvailable);
    };
    CheckGeometryUpload(RHIBufferUsage::Vertex, vk::AccessFlagBits2::eVertexAttributeRead);
    CheckGeometryUpload(RHIBufferUsage::Index, vk::AccessFlagBits2::eIndexRead);

    const auto StorageAccess = ToVkBufferUploadAccess(RHIBufferUsage::Storage);
    EXPECT_FALSE(static_cast<bool>(StorageAccess & vk::AccessFlagBits2::eAccelerationStructureReadKHR));
}
