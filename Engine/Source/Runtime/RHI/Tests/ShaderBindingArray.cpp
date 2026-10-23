/// @file   ShaderBindingArray.cpp
/// @brief  Tests for shader-side resource array binding (RHI validation layer).
///
/// Vulkan descriptor semantics are device-bound and not covered here (same rationale as D14); these tests pin
/// the request validation contract: path resolution, scalar/array API separation, duplicate and
/// out-of-capacity indices, single-set batches, and mixed scalar+array set completeness.

#include <gtest/gtest.h>

import RHI;
import Shader;
import std;
import Core;

using namespace SoulEngine;

class MockRenderTarget final : public RHITexture {
  public:
    MockRenderTarget()
        : RHITexture("Test/ArrayTarget",
                     RHITextureDesc{.Width       = 4,
                                    .Height      = 4,
                                    .MipLevels   = 3,
                                    .Format      = RHIFormat::R8G8B8A8_UNORM,
                                    .Usage       = RHITextureUsage::Sampled,
                                    .MemoryUsage = RHIMemoryUsage::GpuOnly}) {}
};

class MockTransientConstantBuffer final : public RHIBuffer {
  public:
    MockTransientConstantBuffer()
        : RHIBuffer("Test/FrameData", 256, RHIBufferUsage::Constant, RHIMemoryUsage::CpuToGpu, RHILifetime::Transient) {
    }
};

/// One sampled view of a single mip at one array index.
[[nodiscard]] auto MakeTexElement(const RHIRef<RHITexture>& Tex, Uint32 Mip, Uint32 Index) -> RHIArrayBindingElement {
    return {
        .View = {.Texture = Tex, .Usage = RHITextureUsage::Sampled, .Range = {.BaseMipLevel = Mip, .MipLevelCount = 1}},
        .ArrayIndex = Index};
}

/// One storage view of a single mip at one array index.
[[nodiscard]] auto MakeStorageTexElement(const RHIRef<RHITexture>& Tex, Uint32 Mip, Uint32 Index)
    -> RHIArrayBindingElement {
    return {
        .View = {.Texture = Tex, .Usage = RHITextureUsage::Storage, .Range = {.BaseMipLevel = Mip, .MipLevelCount = 1}},
        .ArrayIndex = Index};
}

class MockBindingSet final : public RHIShaderBindingSet {
  public:
    explicit MockBindingSet(const RHIShaderBindingSetDesc& Desc) : RHIShaderBindingSet("Test/BindingSet", Desc) {}

    /// @brief Simulates backend layout creation.
    ///
    /// Fills the effective array capacity a VulkanShaderBindingSet would compute.
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
};

auto MakeRenderTarget() -> RHIRef<RHITexture> {
    auto Ref       = RHIRef<RHITexture>::Create();
    auto Published = Ref.Publish(UPtr<RHITexture>{std::make_unique<MockRenderTarget>()}, RHIRefState::Ready);
    EXPECT_TRUE(Published.has_value());
    return Ref;
}

auto MakeConstantBuffer() -> RHIRef<RHIBuffer> {
    auto Ref       = RHIRef<RHIBuffer>::Create();
    auto Published = Ref.Publish(UPtr<RHIBuffer>{std::make_unique<MockTransientConstantBuffer>()}, RHIRefState::Ready);
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
            .Type          = ShaderResourceType::Texture,
            .Access        = ShaderBindingAccess::Read,
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
            .Type          = ShaderResourceType::Texture,
            .Access        = ShaderBindingAccess::Read,
            .ArrayCount    = 4,
        },
        ShaderBinding{
            .ParameterPath = "g_extra.texArray",
            .Set           = 1,
            .BindingIndex  = 0,
            .Type          = ShaderResourceType::Texture,
            .Access        = ShaderBindingAccess::ReadWrite,
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

TEST_F(ShaderBindingArrayTest, BindsElementsAtExplicitIndices) {
    auto Set          = MakeArraySet();
    auto Tex          = MakeRenderTarget();
    auto Frame        = MakeConstantBuffer();
    auto ScalarResult = Set->BindResources(std::array{
        RHIShaderBindingRequest{"g_params.frame", RHIBufferView{.Buffer = Frame, .Usage = RHIBufferUsage::Constant}},
    });
    ASSERT_TRUE(ScalarResult.has_value()) << ScalarResult.error().ToString();
    // Gap at index 1 stays unbound (partially bound is legal).
    auto Elements = std::array{
        MakeTexElement(Tex, 0, 2),
        MakeTexElement(Tex, 1, 0),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
        },
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto* Slot = Set->GetSlot("g_params.texArray");
    ASSERT_NE(Slot, nullptr);
    ASSERT_EQ(Slot->ArrayElements.size(), 2U);
    EXPECT_EQ(Slot->ArrayElements[0].ArrayIndex, 2U);
    EXPECT_EQ(Slot->ArrayElements[0].View.Usage, RHITextureUsage::Sampled);
    EXPECT_FALSE(Set->HasUnboundBindings());
}

TEST_F(ShaderBindingArrayTest, RejectsDuplicateIndex) {
    auto Set      = MakeArraySet();
    auto Tex      = MakeRenderTarget();
    auto Elements = std::array{
        MakeTexElement(Tex, 0, 1),
        MakeTexElement(Tex, 0, 1),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsIndexBeyondCapacity) {
    auto Set      = MakeArraySet();
    auto Tex      = MakeRenderTarget();
    auto Elements = std::array{
        MakeTexElement(Tex, 0, 4),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsUnknownPathAndScalarPath) {
    auto Set      = MakeArraySet();
    auto Tex      = MakeRenderTarget();
    auto Elements = std::array{
        MakeTexElement(Tex, 0, 0),
    };
    EXPECT_FALSE(Set->BindArrayResources(std::array{
                                             RHIShaderArrayBindingRequest{
                                                 .ParameterPath = "g_params.missing",
                                                 .Elements      = Elements,
                                             },
                                         })
                     .has_value());
    EXPECT_FALSE(Set->BindArrayResources(std::array{
                                             RHIShaderArrayBindingRequest{
                                                 .ParameterPath = "g_params.frame",
                                                 .Elements      = Elements,
                                             },
                                         })
                     .has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsElementWithoutReadyResource) {
    auto Set      = MakeArraySet();
    auto Elements = std::array{
        RHIArrayBindingElement{},
    };
    Elements[0].ArrayIndex = 0;
    auto Result            = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsBatchSpanningMultipleSets) {
    auto Set      = MakeTwoSetArrayBindingSet();
    auto Tex      = MakeRenderTarget();
    auto Elements = std::array{
        MakeTexElement(Tex, 0, 0),
    };
    auto StorageElements = std::array{
        MakeStorageTexElement(Tex, 0, 0),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
        },
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_extra.texArray",
            .Elements      = StorageElements,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, RejectsElementTypeMismatch) {
    // Sampled views bound to a storage-texture array slot: the slot's reflected type is checked against the
    // type derived from the views.
    auto Set      = MakeTwoSetArrayBindingSet();
    auto Tex      = MakeRenderTarget();
    auto Elements = std::array{
        MakeTexElement(Tex, 0, 0),
    };
    auto Result = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_extra.texArray",
            .Elements      = Elements,
        },
    });
    EXPECT_FALSE(Result.has_value());
}

TEST_F(ShaderBindingArrayTest, ScalarBatchCoexistsWithArraySlots) {
    auto Set      = MakeArraySet();
    auto Tex      = MakeRenderTarget();
    auto Frame    = MakeConstantBuffer();
    auto Elements = std::array{
        MakeTexElement(Tex, 0, 0),
        MakeTexElement(Tex, 0, 1),
        MakeTexElement(Tex, 0, 2),
        MakeTexElement(Tex, 0, 3),
    };
    // The scalar batch covers only the scalar slot even though the set has two bindings (the array slot
    // completes via BindArrayResources).
    auto ScalarResult = Set->BindResources(std::array{
        RHIShaderBindingRequest{"g_params.frame", RHIBufferView{.Buffer = Frame, .Usage = RHIBufferUsage::Constant}},
    });
    ASSERT_TRUE(ScalarResult.has_value()) << ScalarResult.error().ToString();
    EXPECT_TRUE(Set->HasUnboundBindings());

    auto ArrayResult = Set->BindArrayResources(std::array{
        RHIShaderArrayBindingRequest{
            .ParameterPath = "g_params.texArray",
            .Elements      = Elements,
        },
    });
    ASSERT_TRUE(ArrayResult.has_value()) << ArrayResult.error().ToString();
    EXPECT_FALSE(Set->HasUnboundBindings());
}

TEST_F(ShaderBindingArrayTest, ScalarBatchRejectsArraySlot) {
    auto Set    = MakeArraySet();
    auto Tex    = MakeRenderTarget();
    // Scalar API must refuse array slots and point callers at BindArrayResources.
    auto Result = Set->BindResources(std::array{
        RHIShaderBindingRequest{"g_params.texArray", RHITextureView{.Texture = Tex, .Usage = RHITextureUsage::Sampled}},
    });
    EXPECT_FALSE(Result.has_value());
}
