#include <cstddef>
#include <gtest/gtest.h>

import Core;
import RHI;
import Renderer.Common;
import Renderer.Raster.RasterRenderer;
import Scene;
import std;

using namespace SoulEngine;

static_assert(sizeof(RasterIndirectCommand) == sizeof(Uint32) * 5);
static_assert(offsetof(RasterIndirectCommand, IndexCount) == 0);
static_assert(offsetof(RasterIndirectCommand, InstanceCount) == 4);
static_assert(offsetof(RasterIndirectCommand, FirstIndex) == 8);
static_assert(offsetof(RasterIndirectCommand, VertexOffset) == 12);
static_assert(offsetof(RasterIndirectCommand, FirstInstance) == 16);
static_assert(std::same_as<decltype(RasterIndirectCommand::VertexOffset), Int32>);

TEST(RasterDrawGroups, BuildsIndexedCommandsAndPreservesGeometrySlots) {
    SceneGpuData DrawData;
    DrawData.GeometryIndexCounts = {6, 9, 12};
    DrawData.InstanceRecords     = {
        InstanceRecord::GpuData{.GeometryID = 0},
        InstanceRecord::GpuData{.GeometryID = 0},
        InstanceRecord::GpuData{.GeometryID = 1},
        InstanceRecord::GpuData{.GeometryID = 1},
        InstanceRecord::GpuData{.GeometryID = 1},
        InstanceRecord::GpuData{.GeometryID = 2},
    };

    const auto Groups = BuildRasterDrawGroups(DrawData);
    ASSERT_EQ(Groups.size(), 3u);

    constexpr std::array<Uint32, 3> ExpectedGeometryIDs{0, 1, 2};
    constexpr std::array<Uint32, 3> ExpectedIndexCounts{6, 9, 12};
    constexpr std::array<Uint32, 3> ExpectedInstanceCounts{2, 3, 1};
    constexpr std::array<Uint32, 3> ExpectedFirstInstances{0, 2, 5};
    for (std::size_t I = 0; I < Groups.size(); ++I) {
        EXPECT_EQ(Groups[I].GeometryID, ExpectedGeometryIDs[I]);
        EXPECT_EQ(Groups[I].Command.IndexCount, ExpectedIndexCounts[I]);
        EXPECT_EQ(Groups[I].Command.InstanceCount, ExpectedInstanceCounts[I]);
        EXPECT_EQ(Groups[I].Command.FirstIndex, 0u);
        EXPECT_EQ(Groups[I].Command.VertexOffset, 0);
        EXPECT_EQ(Groups[I].Command.FirstInstance, ExpectedFirstInstances[I]);
    }

    const auto Commands = BuildRasterIndirectCommands(Groups);
    ASSERT_EQ(Commands.size(), Groups.size());
    for (std::size_t I = 0; I < Commands.size(); ++I) {
        EXPECT_EQ(Commands[I].IndexCount, ExpectedIndexCounts[I]);
        EXPECT_EQ(Commands[I].InstanceCount, ExpectedInstanceCounts[I]);
        EXPECT_EQ(Commands[I].FirstInstance, ExpectedFirstInstances[I]);
    }
}

class MockRasterBuffer final : public RHIBuffer {
  public:
    MockRasterBuffer(StringView Name, RHIBufferUsage Usage)
        : RHIBuffer(String(Name), 64, Usage, RHIMemoryUsage::CpuToGpu, RHILifetime::Persistent) {}
};

[[nodiscard]] auto MakeRasterBuffer(StringView Name, RHIBufferUsage Usage) -> RHIRef<RHIBuffer> {
    auto Result    = RHIRef<RHIBuffer>::Create();
    auto Published = Result.Publish(std::make_unique<MockRasterBuffer>(Name, Usage), RHIRefState::Ready);
    EXPECT_TRUE(Published.has_value());
    return Result;
}

TEST(RasterGeometrySubmission, ReadsRuntimeCVarAndDefaultsInvalidValues) {
    constexpr StringView CVarName{"Render.Raster.GeometrySubmissionPath"};
    const auto           Original = CVarManager::Get().Find<String>(CVarName);
    ASSERT_TRUE(Original.has_value());
    struct RestoreCVar {
        String Value;
        ~RestoreCVar() {
            CVarManager::Get().SetByName<String>("Render.Raster.GeometrySubmissionPath", Value);
        }
    } Restore{*Original};

    EXPECT_EQ(GetRasterGeometrySubmissionPath(), RasterGeometrySubmissionPath::VertexPulling);
    ASSERT_TRUE(CVarManager::Get().SetByName<String>(CVarName, "VertexInput"));
    EXPECT_EQ(GetRasterGeometrySubmissionPath(), RasterGeometrySubmissionPath::VertexInput);
    ASSERT_TRUE(CVarManager::Get().SetByName<String>(CVarName, "InvalidPath"));
    EXPECT_EQ(GetRasterGeometrySubmissionPath(), RasterGeometrySubmissionPath::VertexPulling);
}

TEST(RasterDrawGroups, SceneGpuDataKeepsGeometryRefsAlignedWithCanonicalInstanceOrder) {
    RHIDeferredDeletionQueue DeletionQueue;
    GDeferredDeletionQueue = &DeletionQueue;
    {
        auto First             = GeometryHandle{std::make_shared<GeometryRecord>()};
        First->IndexCount      = 6;
        First->PositionBuffer  = MakeRasterBuffer("FirstPosition", RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
        First->NormalBuffer    = MakeRasterBuffer("FirstNormal", RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
        First->TangentBuffer   = MakeRasterBuffer("FirstTangent", RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
        First->TexCoordBuffer  = MakeRasterBuffer("FirstTexCoord", RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
        First->IndexBuffer     = MakeRasterBuffer("FirstIndex", RHIBufferUsage::Index | RHIBufferUsage::Storage);
        auto Second            = GeometryHandle{std::make_shared<GeometryRecord>()};
        Second->IndexCount     = 9;
        Second->PositionBuffer = MakeRasterBuffer("SecondPosition", RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
        Second->NormalBuffer   = MakeRasterBuffer("SecondNormal", RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
        Second->IndexBuffer    = MakeRasterBuffer("SecondIndex", RHIBufferUsage::Index | RHIBufferUsage::Storage);

        const std::array Instances{
            InstanceRecord{.EntityId = 10, .Geometry = Second},
            InstanceRecord{.EntityId = 11, .Geometry = First},
            InstanceRecord{.EntityId = 12, .Geometry = Second},
            InstanceRecord{.EntityId = 13, .Geometry = First},
        };
        const std::unordered_map<Uint64, Uint32> TextureIndices;
        const auto                               DrawData = SceneGpuData::Create(Instances, {}, TextureIndices);

        ASSERT_EQ(DrawData.InstanceRecords.size(), 4u);
        EXPECT_EQ(DrawData.InstanceRecords[0].EntityID, 10u);
        EXPECT_EQ(DrawData.InstanceRecords[1].EntityID, 12u);
        EXPECT_EQ(DrawData.InstanceRecords[2].EntityID, 11u);
        EXPECT_EQ(DrawData.InstanceRecords[3].EntityID, 13u);
        ASSERT_EQ(DrawData.GeometryBuffers.size(), 2u);
        EXPECT_EQ(DrawData.GeometryBuffers[0].IndexBuffer.TryGet(), Second->IndexBuffer.TryGet());
        EXPECT_EQ(DrawData.GeometryBuffers[1].IndexBuffer.TryGet(), First->IndexBuffer.TryGet());
        EXPECT_EQ(DrawData.GeometryBuffers[0].PositionBuffer.TryGet(), Second->PositionBuffer.TryGet());
        EXPECT_EQ(DrawData.GeometryBuffers[1].PositionBuffer.TryGet(), First->PositionBuffer.TryGet());
        EXPECT_EQ(DrawData.GeometryBuffers[1].TangentBuffer.TryGet(), First->TangentBuffer.TryGet());
        EXPECT_EQ(DrawData.GeometryBuffers[1].TexCoordBuffer.TryGet(), First->TexCoordBuffer.TryGet());
        EXPECT_FALSE(DrawData.GeometryBuffers[0].TangentBuffer);
        EXPECT_FALSE(DrawData.GeometryBuffers[0].TexCoordBuffer);

        const auto Groups = BuildRasterDrawGroups(DrawData);
        ASSERT_EQ(Groups.size(), 2u);
        EXPECT_EQ(Groups[0].GeometryID, 0u);
        EXPECT_EQ(Groups[0].Command.IndexCount, 9u);
        EXPECT_EQ(Groups[0].Command.InstanceCount, 2u);
        EXPECT_EQ(Groups[0].Command.FirstInstance, 0u);
        EXPECT_EQ(Groups[1].GeometryID, 1u);
        EXPECT_EQ(Groups[1].Command.IndexCount, 6u);
        EXPECT_EQ(Groups[1].Command.InstanceCount, 2u);
        EXPECT_EQ(Groups[1].Command.FirstInstance, 2u);
    }
    DeletionQueue.Drain();
    GDeferredDeletionQueue = nullptr;
}
