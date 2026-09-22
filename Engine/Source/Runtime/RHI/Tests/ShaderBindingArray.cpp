/// @file   ShaderBindingArray.cpp
/// @brief  Tests for shader-side resource array binding (WT2, RHI validation layer).
///
/// Vulkan descriptor semantics are device-bound and not covered here (same
/// rationale as D14); these tests pin the request validation contract:
/// path resolution, scalar/array API separation, duplicate and out-of-capacity
/// indices, single-set batches, and mixed scalar+array set completeness.

#include <gtest/gtest.h>

import RHI;
import Shader;
import std;

using namespace SoulEngine;

namespace {

class MockRenderTarget final : public RHIRenderTarget {
  public:
    MockRenderTarget() : RHIRenderTarget("Test/ArrayTarget") {}
    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return 4;
    }
    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return 4;
    }
    [[nodiscard]] auto GetFormat() const -> RHIFormat override {
        return RHIFormat::R8G8B8A8_UNORM;
    }
    [[nodiscard]] auto GetUsage() const -> RHITextureUsage override {
        return RHITextureUsage::ShaderResource;
    }
    [[nodiscard]] auto GetMipLevelCount() const -> Uint32 override {
        return 3;
    }
};

class MockTransientConstantBuffer final : public RHITransientConstantBuffer {
  public:
    MockTransientConstantBuffer() : RHITransientConstantBuffer("Test/FrameData", 256) {}
};

class MockBindingSet final : public RHIShaderBindingSet {
  public:
    explicit MockBindingSet(const RHIShaderBindingSetDesc& Desc)
        : RHIShaderBindingSet("Test/BindingSet", Desc) {}

    /// Simulates backend layout creation: fills the effective array capacity
    /// a VulkanShaderBindingSet would compute (WT2 step 4b).
    auto ConfigureArrayCapacity(StringView Path, Uint32 Capacity) -> void {
        for (auto& SetBindings : m_BindingsBySet)
            for (auto& Binding : SetBindings)
                if (Binding.Info.ParameterPath == Path)
                    Binding.ArrayCapacity = Capacity;
    }

    [[nodiscard]] auto GetSlot(StringView Path) -> Slot* {
        for (auto& SetBindings : m_BindingsBySet)
            for (auto& Binding : SetBindings)
                if (Binding.Info.ParameterPath == Path)
                    return &Binding;
        return nullptr;
    }

  protected:
    [[nodiscard]] auto OnResourcesBound(Uint32) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto OnBindlessResourceBound() -> std::expected<void, ErrorMessage> override {
        return {};
    }
};

auto MakeRenderTarget() -> RHIRef<RHIRenderTarget> {
    auto Ref = RHIRef<RHIRenderTarget>::Create();
    auto Published =
        Ref.Publish(UPtr<RHIRenderTarget>{std::make_unique<MockRenderTarget>()}, RHIRefState::Ready);
    EXPECT_TRUE(Published.has_value());
    return Ref;
}

auto MakeConstantBuffer() -> RHIRef<RHITransientConstantBuffer> {
    auto Ref = RHIRef<RHITransientConstantBuffer>::Create();
    auto Published = Ref.Publish(
        UPtr<RHITransientConstantBuffer>{std::make_unique<MockTransientConstantBuffer>()}, RHIRefState::Ready);
    EXPECT_TRUE(Published.has_value());
    return Ref;
}

auto MakeArraySet() -> std::unique_ptr<MockBindingSet> {
    ShaderReflection Reflection;
    Reflection.Bindings = {
        ShaderBinding{
            .ParameterPath = "g_params.frame",
            .Set           = 0,
            .BindingIndex  = 0,
            .Type          = ShaderResourceType::ConstantBuffer,
            .ArrayCount    = 1,
        },
        ShaderBinding{
            .ParameterPath = "g_params.texArray",
            .Set           = 0,
            .BindingIndex  = 1,
            .Type          = ShaderResourceType::SampledTexture,
            .ArrayCount    = 4,
        },
    };
    auto Set = std::make_unique<MockBindingSet>(RHIShaderBindingSetDesc{
        .Reflection = std::move(Reflection),
        .Stages     = ShaderStage::Compute,
    });
    Set->ConfigureArrayCapacity("g_params.texArray", 4);
    return Set;
}

auto MakeTwoSetArrayBindingSet() -> std::unique_ptr<MockBindingSet> {
    ShaderReflection Reflection;
    Reflection.Bindings = {
        ShaderBinding{
            .ParameterPath = "g_params.texArray",
            .Set           = 0,
            .BindingIndex  = 0,
            .Type          = ShaderResourceType::SampledTexture,
            .ArrayCount    = 4,
        },
        ShaderBinding{
            .ParameterPath = "g_extra.texArray",
            .Set           = 1,
            .BindingIndex  = 0,
            .Type          = ShaderResourceType::StorageTexture,
            .ArrayCount    = kShaderReflectionArrayUnboundedSize,
        },
    };
    auto Set = std::make_unique<MockBindingSet>(RHIShaderBindingSetDesc{
        .Reflection = std::move(Reflection),
        .Stages     = ShaderStage::Compute,
    });
    Set->ConfigureArrayCapacity("g_params.texArray", 4);
    Set->ConfigureArrayCapacity("g_extra.texArray", 4096);
    return Set;
}

class ShaderBindingArrayTest : public ::testing::Test {
  protected:
    ShaderBindingArrayTest() {
        GDeferredDeletionQueue = &m_DeletionQueue;
    }
    ~ShaderBindingArrayTest() override {
        GDeferredDeletionQueue = nullptr;
    }

    RHIDeferredDeletionQueue m_DeletionQueue;
};

} // namespace

TEST_F(ShaderBindingArrayTest, BindsElementsAtExplicitIndices) {
    auto   Set   = MakeArraySet();
    auto   Tex   = MakeRenderTarget();
    auto   Frame = MakeConstantBuffer();
    auto   ScalarResult = Set->BindResources(std::array{
        RHIShaderBindingRequest{"g_params.frame", Frame, true},
    });
    ASSERT_TRUE(ScalarResult.has_value()) << ScalarResult.error().ToString();
    // Gap at index 1 stays unbound (partially bound is legal).
    auto   Elements = std::array{
        MakeRenderTargetArrayElement(Tex, 0, 2),
        MakeRenderTargetArrayElement(Tex, 1, 0),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
            .Type          = ShaderResourceType::SampledTexture,
            .IsReadOnly    = true,
        },
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto* Slot = Set->GetSlot("g_params.texArray");
    ASSERT_NE(Slot, nullptr);
    ASSERT_EQ(Slot->ArrayElements.size(), 2U);
    EXPECT_EQ(Slot->ArrayElements[0].ArrayIndex, 2U);
    EXPECT_TRUE(Slot->IsReadOnly);
    EXPECT_FALSE(Set->HasUnboundBindings());
}

TEST_F(ShaderBindingArrayTest, RejectsDuplicateIndex) {
    auto Set     = MakeArraySet();
    auto Tex     = MakeRenderTarget();
    auto Elements = std::array{
        MakeRenderTargetArrayElement(Tex, 0, 1),
        MakeRenderTargetArrayElement(Tex, 0, 1),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
            .Type          = ShaderResourceType::SampledTexture,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsIndexBeyondCapacity) {
    auto Set     = MakeArraySet();
    auto Tex     = MakeRenderTarget();
    auto Elements = std::array{
        MakeRenderTargetArrayElement(Tex, 0, 4),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
            .Type          = ShaderResourceType::SampledTexture,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsUnknownPathAndScalarPath) {
    auto Set     = MakeArraySet();
    auto Tex     = MakeRenderTarget();
    auto Elements = std::array{
        MakeRenderTargetArrayElement(Tex, 0, 0),
    };
    EXPECT_FALSE(Set->BindArrayResources(std::array{
                     RHIShaderArrayBindingRequest{
                         .ParameterPath = "g_params.missing",
                         .Elements      = Elements,
                         .Type          = ShaderResourceType::SampledTexture,
                     },
                 })
                     .has_value());
    EXPECT_FALSE(Set->BindArrayResources(std::array{
                     RHIShaderArrayBindingRequest{
                         .ParameterPath = "g_params.frame",
                         .Elements      = Elements,
                         .Type          = ShaderResourceType::ConstantBuffer,
                     },
                 })
                     .has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsElementWithoutReadyResource) {
    auto Set = MakeArraySet();
    auto Elements = std::array{
        RHIArrayBindingElement{},
    };
    Elements[0].ArrayIndex = 0;
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
            .Type          = ShaderResourceType::SampledTexture,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsBatchSpanningMultipleSets) {
    auto Set     = MakeTwoSetArrayBindingSet();
    auto Tex     = MakeRenderTarget();
    auto Elements = std::array{
        MakeRenderTargetArrayElement(Tex, 0, 0),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
            .Type          = ShaderResourceType::SampledTexture,
        },
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_extra.texArray",
            .Elements      = Elements,
            .Type          = ShaderResourceType::StorageTexture,
            .IsReadOnly    = false,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, ScalarBatchCoexistsWithArraySlots) {
    auto Set  = MakeArraySet();
    auto Tex  = MakeRenderTarget();
    auto Frame = MakeConstantBuffer();
    auto Elements = std::array{
        MakeRenderTargetArrayElement(Tex, 0, 0),
        MakeRenderTargetArrayElement(Tex, 0, 1),
        MakeRenderTargetArrayElement(Tex, 0, 2),
        MakeRenderTargetArrayElement(Tex, 0, 3),
    };
    // The scalar batch covers only the scalar slot even though the set has two
    // bindings (the array slot completes via BindArrayResources).
    auto ScalarResult = Set->BindResources(std::array{
        RHIShaderBindingRequest{"g_params.frame", Frame, true},
    });
    ASSERT_TRUE(ScalarResult.has_value()) << ScalarResult.error().ToString();
    EXPECT_TRUE(Set->HasUnboundBindings());

    auto ArrayResult = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
            .Type          = ShaderResourceType::SampledTexture,
        },
    });
    ASSERT_TRUE(ArrayResult.has_value()) << ArrayResult.error().ToString();
    EXPECT_FALSE(Set->HasUnboundBindings());
}

TEST_F(ShaderBindingArrayTest, ScalarBatchRejectsArraySlot) {
    auto Set = MakeArraySet();
    auto Tex = MakeRenderTarget();
    // Scalar API must refuse array slots and point callers at BindArrayResources.
    auto Result = Set->BindResources(std::array{
        RHIShaderBindingRequest{"g_params.texArray", Tex, true},
    });
    EXPECT_FALSE(Result.has_value());
}
