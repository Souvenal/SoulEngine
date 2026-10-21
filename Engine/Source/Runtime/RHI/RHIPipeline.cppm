module;

// Clang 22 named-modules bug: importing a module that re-exports std (Core) breaks concept lookup via the
// re-export chain; a textual <concepts> in the GMF restores visibility. Safe to drop once clang fixes concept
// re-export.
#include <concepts>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module RHI.Pipeline;

import Core;
export import RHI.Types;
import RHI.Ref;
import Shader;
import std;

namespace SoulEngine {

export enum class RHIPipelineType : Uint8 {
    Unknown = 0,
    Graphics,
    Compute,
    RayTracing,
};

/// @brief Reflected shader interface used to construct a binding set.
export struct RHIShaderBindingSetDesc {
    ShaderReflection Reflection = {};
    /// Bitmask of every stage in the shader program.
    ShaderStage      Stages     = ShaderStage::Unknown;
};

/// @brief One bound resource at the RHI level: always a view.
///
/// Sampler and TLAS views are degenerate (identity only), so every alternative retains its payload through
/// the embedded RHIRef.
export using RHIBoundResource =
    std::variant<std::monostate, RHITextureView, RHIBufferView, RHISamplerView, RHITlasView>;

export struct RHIShaderBindingRequest {
    StringView       ParameterPath = {};
    RHIBoundResource Bound         = {};

    RHIShaderBindingRequest() = default;

    /// Explicit view forms.
    RHIShaderBindingRequest(StringView ParameterPathIn, RHITextureView View)
        : ParameterPath(ParameterPathIn), Bound(View) {}
    RHIShaderBindingRequest(StringView ParameterPathIn, RHIBufferView View)
        : ParameterPath(ParameterPathIn), Bound(View) {}
    RHIShaderBindingRequest(StringView ParameterPathIn, RHISamplerView View)
        : ParameterPath(ParameterPathIn), Bound(View) {}
    RHIShaderBindingRequest(StringView ParameterPathIn, RHITlasView View)
        : ParameterPath(ParameterPathIn), Bound(View) {}
};

/// @brief One element of a shader resource array binding.
///
/// Texture views only today; buffers/samplers extend here when arrays of them arrive.
export struct RHIArrayBindingElement {
    RHITextureView View       = {};
    Uint32         ArrayIndex = 0;
};

/// @brief Request binding a shader-side resource array (fixed or unbounded).
/// Elements may address arbitrary array indices; gaps stay unbound (partially-bound descriptor sets make that
/// legal).
export struct RHIShaderArrayBindingRequest {
    StringView                              ParameterPath = {};
    std::span<const RHIArrayBindingElement> Elements      = {};
};

/// @brief Backend-owned descriptor resources for one reflected shader layout.
export class RHIShaderBindingSet : public RHIObject {
  public:
    /// @brief One reflected binding slot: the shader-side contract plus the
    /// bound resource.
    struct Slot {
        ShaderBinding                       Info                 = {};
        /// @brief The bound resource as a view.
        ///
        /// Sampler/TLAS views are degenerate identity wrappers. Replaces the old raw-pointer + flag
        /// smuggling.
        RHIBoundResource                    Bound                = {};
        /// @brief Backend-filled effective descriptor count.
        ///
        /// ArrayCount for fixed arrays, the chosen layout count for unbounded ones.
        Uint32                              ArrayCapacity        = 1;
        /// True when the slot backs an unbounded (variable descriptor count) array.
        bool                                IsVariableCountArray = false;
        /// @brief Bound array elements; empty for scalar slots.
        ///
        /// Indices may have gaps — unbound elements are never written.
        std::vector<RHIArrayBindingElement> ArrayElements        = {};
    };

    RHIShaderBindingSet(const RHIShaderBindingSet&)                    = delete;
    auto operator=(const RHIShaderBindingSet&) -> RHIShaderBindingSet& = delete;
    RHIShaderBindingSet(RHIShaderBindingSet&&)                         = delete;
    auto operator=(RHIShaderBindingSet&&) -> RHIShaderBindingSet&      = delete;
    virtual ~RHIShaderBindingSet()                                     = default;

    [[nodiscard]] auto GetBindingsBySet() const -> std::span<const std::vector<Slot>> {
        return m_BindingsBySet;
    }

    [[nodiscard]] auto HasUnboundBindings() const -> bool;

    [[nodiscard]] auto BindResources(std::span<const RHIShaderBindingRequest> Resources)
        -> std::expected<void, ErrorMessage>;

    /// @brief Binds shader-side resource arrays (fixed or unbounded).
    ///
    /// One batch must target a single descriptor set; scalar slots of the same set are bound through
    /// BindResources.
    [[nodiscard]] auto BindArrayResources(std::span<const RHIShaderArrayBindingRequest> Requests)
        -> std::expected<void, ErrorMessage>;

    template <typename T>
    [[nodiscard]] auto PushConstants(Uint32 Offset, const T& Data) -> std::expected<void, ErrorMessage> {
        const auto Bytes = std::as_bytes(std::span<const T>{&Data, 1});
        const auto Size  = static_cast<Uint64>(Bytes.size_bytes());
        if (Size == 0)
            return std::unexpected(ErrorMessage("Push constant data must not be empty"));

        const auto End          = static_cast<Uint64>(Offset) + Size;
        Uint64     ReflectedEnd = 0;
        for (const auto& Range : m_PushConstantRanges)
            ReflectedEnd = (std::max)(ReflectedEnd, static_cast<Uint64>(Range.Offset) + Range.Size);
        if (End > ReflectedEnd) {
            return std::unexpected(ErrorMessage(
                Format("Push constant range exceeds reflected size ({} + {} > {})", Offset, Size, ReflectedEnd)));
        }

        m_PendingPushConstants.push_back(PushConstantWrite{
            .Offset = Offset,
            .Data   = std::vector<std::byte>{Bytes.begin(), Bytes.end()},
        });
        return {};
    }

  protected:
    /// @brief One pass-local push-constant write awaiting state capture.
    struct PushConstantWrite {
        Uint32                 Offset = 0;
        std::vector<std::byte> Data   = {};
    };

    RHIShaderBindingSet(String Name, const RHIShaderBindingSetDesc& Desc);

    [[nodiscard]] virtual auto OnResourcesBound(Uint32) -> std::expected<void, ErrorMessage> = 0;

    std::vector<std::vector<Slot>>       m_BindingsBySet        = {};
    std::vector<PushConstantWrite>       m_PendingPushConstants = {};
    std::optional<Uint32>                m_BindlessSpace        = std::nullopt;
    std::vector<ShaderPushConstantRange> m_PushConstantRanges   = {};
    ShaderStage                          m_Stages               = ShaderStage::Unknown;
};
// Pipeline base

/// @brief Polymorphic base for all pipeline resources.
/// Backend concrete classes (e.g. VulkanGraphicsPipeline) own native pipeline state. Higher-level owners keep
/// RHIRef instances; command lists only observe them.
export class RHIPipeline : public RHIObject {
  public:
    RHIPipeline(const RHIPipeline&)                    = delete;
    auto operator=(const RHIPipeline&) -> RHIPipeline& = delete;
    RHIPipeline(RHIPipeline&&)                         = delete;
    auto operator=(RHIPipeline&&) -> RHIPipeline&      = delete;
    virtual ~RHIPipeline()                             = default;

    [[nodiscard]] virtual auto GetType() const -> RHIPipelineType = 0;

    [[nodiscard]] auto GetShaderBindingSet() const -> RHIRef<RHIShaderBindingSet> {
        return m_ShaderBindingSet;
    }

  protected:
    explicit RHIPipeline(String Name, RHIRef<RHIShaderBindingSet> BindingSet)
        : RHIObject(std::move(Name)), m_ShaderBindingSet(std::move(BindingSet)) {}

  private:
    RHIRef<RHIShaderBindingSet> m_ShaderBindingSet = nullptr;
};
// Graphics pipeline

export struct RHIGraphicsPipelineDesc {
    ShaderGraphicsProgram       Program           = {};
    RHIRef<RHIShaderBindingSet> BindingSet        = nullptr;
    RHIVertexInputLayoutDesc    VertexInputLayout = {};
    RHIPrimitiveTopology        Topology          = RHIPrimitiveTopology::TriangleList;
    RHIRasterizerState          Rasterizer        = {};
    RHIBlendState               Blend             = {};
    RHIDepthStencilState        DepthStencil      = {};
    std::vector<RHIFormat>      ColorFormats      = {RHIFormat::B8G8R8A8_UNORM};
    RHIFormat                   DepthFormat       = RHIFormat::Unknown;
};

export class RHIGraphicsPipeline : public RHIPipeline {
  public:
    RHIGraphicsPipeline(const RHIGraphicsPipeline&)                    = delete;
    auto operator=(const RHIGraphicsPipeline&) -> RHIGraphicsPipeline& = delete;
    RHIGraphicsPipeline(RHIGraphicsPipeline&&)                         = delete;
    auto operator=(RHIGraphicsPipeline&&) -> RHIGraphicsPipeline&      = delete;
    virtual ~RHIGraphicsPipeline()                                     = default;

    [[nodiscard]] auto GetType() const -> RHIPipelineType override {
        return RHIPipelineType::Graphics;
    }

  protected:
    explicit RHIGraphicsPipeline(String Name, RHIRef<RHIShaderBindingSet> BindingSet)
        : RHIPipeline(std::move(Name), std::move(BindingSet)) {}
};
// Compute pipeline

/// @brief One host-provided specialization constant value.
///
/// The reflection remains the type authority: a variant holding the wrong scalar type for the reflected
/// constant is a hard error.
export struct RHISpecializationConstant {
    String                                     Name  = {};
    std::variant<bool, Int32, Uint32, Float32> Value = false;

    bool operator==(const RHISpecializationConstant&) const = default;
};

/// @brief Validates provided spec values against the shader reflection: unknown names, duplicate names,
/// and variant/reflection scalar-type mismatches are hard errors.
///
/// Device-independent, so it lives here rather than in a backend.
export [[nodiscard]] auto ValidateSpecializationConstants(std::span<const RHISpecializationConstant> Values,
                                                          const ShaderReflection&                    Reflection)
    -> std::expected<void, ErrorMessage>;

export struct RHIComputePipelineDesc {
    ShaderComputeProgram                   Program                 = {};
    RHIRef<RHIShaderBindingSet>            BindingSet              = nullptr;
    /// @brief Values fixed at pipeline creation (VkSpecializationInfo).
    ///
    /// They are part of the pipeline identity: two value sets must yield two pipelines.
    std::vector<RHISpecializationConstant> SpecializationConstants = {};
};

export class RHIComputePipeline : public RHIPipeline {
  public:
    RHIComputePipeline(const RHIComputePipeline&)                    = delete;
    auto operator=(const RHIComputePipeline&) -> RHIComputePipeline& = delete;
    RHIComputePipeline(RHIComputePipeline&&)                         = delete;
    auto operator=(RHIComputePipeline&&) -> RHIComputePipeline&      = delete;
    virtual ~RHIComputePipeline()                                    = default;

    [[nodiscard]] auto GetType() const -> RHIPipelineType override {
        return RHIPipelineType::Compute;
    }

  protected:
    explicit RHIComputePipeline(String Name, RHIRef<RHIShaderBindingSet> BindingSet)
        : RHIPipeline(std::move(Name), std::move(BindingSet)) {}
};
// Ray-tracing pipeline

export enum class RHIRayTracingShaderGroupType : Uint8 {
    Unknown = 0,
    General,
    TrianglesHit,
    ProceduralHit,
};

export struct RHIRayTracingShaderGroupDesc {
    RHIRayTracingShaderGroupType Type = RHIRayTracingShaderGroupType::Unknown;
};

export struct RHIRayTracingPipelineDesc {
    ShaderRayTracingProgram                   Program           = {};
    RHIRef<RHIShaderBindingSet>               BindingSet        = nullptr;
    std::vector<RHIRayTracingShaderGroupDesc> ShaderGroups      = {};
    Uint32                                    MaxRecursionDepth = 1;
};

export class RHIRayTracingPipeline : public RHIPipeline {
  public:
    RHIRayTracingPipeline(const RHIRayTracingPipeline&)                    = delete;
    auto operator=(const RHIRayTracingPipeline&) -> RHIRayTracingPipeline& = delete;
    RHIRayTracingPipeline(RHIRayTracingPipeline&&)                         = delete;
    auto operator=(RHIRayTracingPipeline&&) -> RHIRayTracingPipeline&      = delete;
    virtual ~RHIRayTracingPipeline()                                       = default;

    [[nodiscard]] auto GetType() const -> RHIPipelineType override {
        return RHIPipelineType::RayTracing;
    }

  protected:
    explicit RHIRayTracingPipeline(String Name, RHIRef<RHIShaderBindingSet> BindingSet)
        : RHIPipeline(std::move(Name), std::move(BindingSet)) {}
};

} // namespace SoulEngine
