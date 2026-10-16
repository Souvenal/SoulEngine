module;

// Clang 22 named-modules bug: importing a module that re-exports std (Core) breaks concept lookup via the
// re-export chain; a textual <concepts> in the GMF restores visibility. Safe to drop once clang fixes concept
// re-export.
#include <concepts>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module RHI:Pipeline;

import Core;
export import :Types;
import :Ref;
import Shader;
import std;

export namespace SoulEngine {

enum class RHIPipelineType : Uint8 {
    Unknown = 0,
    Graphics,
    Compute,
    RayTracing,
};

/// @brief Reflected shader interface used to construct a binding set.
struct RHIShaderBindingSetDesc {
    ShaderReflection Reflection = {};
    /// Bitmask of every stage in the shader program.
    ShaderStage      Stages     = ShaderStage::Unknown;
};

/// @brief One bound resource at the RHI level: always a view.
///
/// Sampler and TLAS views are degenerate (identity only), so every alternative retains its payload through
/// the embedded RHIRef.
using RHIBoundResource = std::variant<std::monostate, RHITextureView, RHIBufferView, RHISamplerView, RHITlasView>;

} // namespace SoulEngine

namespace SoulEngine {

/// @brief The shader-level type a bound resource plays in this request.
///
/// Derived from the view's role — one view, one role.
[[nodiscard]] inline auto ResolveBindingType(const RHIBoundResource& Bound) -> ShaderResourceType {
    if (const auto* View = std::get_if<RHITextureView>(&Bound))
        return ShaderResourceType::Texture;
    if (const auto* View = std::get_if<RHIBufferView>(&Bound)) {
        if (magic_enum::enum_flags_test(View->Usage, RHIBufferUsage::Constant))
            return ShaderResourceType::ConstantBuffer;
        return ShaderResourceType::StorageBuffer;
    }
    if (std::get_if<RHISamplerView>(&Bound))
        return ShaderResourceType::Sampler;
    if (std::get_if<RHITlasView>(&Bound))
        return ShaderResourceType::AccelerationStructure;
    return ShaderResourceType::Unknown;
}

/// @brief The access direction a bound view exercises.
///
/// A storage texture view writes, every other texture view reads. Buffer, sampler, and TLAS views answer
/// Unknown, and access validation skips Unknown on either side.
[[nodiscard]] inline auto ResolveBindingAccess(const RHIBoundResource& Bound) -> ShaderBindingAccess {
    if (const auto* View = std::get_if<RHITextureView>(&Bound))
        return View->Usage == RHITextureUsage::Storage ? ShaderBindingAccess::ReadWrite : ShaderBindingAccess::Read;
    return ShaderBindingAccess::Unknown;
}

/// @brief Readiness probe shared by binding validation: the resource a bound alternative refers to.
///
/// Returns nullptr when the alternative is absent or its payload is not Ready.
[[nodiscard]] inline auto ResolveBindingObject(const RHIBoundResource& Bound) -> RHIObject* {
    if (const auto* View = std::get_if<RHITextureView>(&Bound))
        return View->Texture.TryGet();
    if (const auto* View = std::get_if<RHIBufferView>(&Bound))
        return View->Buffer.TryGet();
    if (const auto* View = std::get_if<RHISamplerView>(&Bound))
        return View->Sampler.TryGet();
    if (const auto* View = std::get_if<RHITlasView>(&Bound))
        return View->Tlas.TryGet();
    return nullptr;
}

} // namespace SoulEngine

export namespace SoulEngine {

struct RHIShaderBindingRequest {
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
struct RHIArrayBindingElement {
    RHITextureView View       = {};
    Uint32         ArrayIndex = 0;
};

/// @brief Request binding a shader-side resource array (fixed or unbounded).
/// Elements may address arbitrary array indices; gaps stay unbound (partially-bound descriptor sets make that
/// legal).
struct RHIShaderArrayBindingRequest {
    StringView                              ParameterPath = {};
    std::span<const RHIArrayBindingElement> Elements      = {};
};

/// @brief Backend-owned descriptor resources for one reflected shader layout.
class RHIShaderBindingSet : public RHIObject {
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

    [[nodiscard]] auto HasUnboundBindings() const -> bool {
        return std::ranges::any_of(m_BindingsBySet, [](const auto& Bindings) {
            return std::ranges::any_of(Bindings, [](const Slot& Binding) {
                return std::holds_alternative<std::monostate>(Binding.Bound) && Binding.ArrayElements.empty();
            });
        });
    }

    [[nodiscard]] auto BindResources(std::span<const RHIShaderBindingRequest> Resources)
        -> std::expected<void, ErrorMessage> {
        if (Resources.empty())
            return std::unexpected(ErrorMessage("Shader binding resource batch must not be empty"));

        std::vector<Slot*> FlatBindings;
        for (auto& SetBindings : m_BindingsBySet)
            for (auto& Binding : SetBindings)
                FlatBindings.push_back(&Binding);

        // We first query each request's set and index
        std::vector<std::pair<Uint32, Uint32>> BindingLocations;
        BindingLocations.reserve(Resources.size());
        for (const auto& Request : Resources) {
            const auto It = std::ranges::find_if(
                FlatBindings, [&](const Slot* Slot) { return Slot->Info.ParameterPath == Request.ParameterPath; });
            if (It == FlatBindings.end())
                return std::unexpected(ErrorMessage(Format(
                    "Shader parameter '{}' is not present in the shader binding layout", Request.ParameterPath)));
            const auto Location = std::pair{(*It)->Info.Set, (*It)->Info.BindingIndex};
            BindingLocations.push_back(Location);
            if (!ResolveBindingObject(Request.Bound))
                return std::unexpected(
                    ErrorMessage(Format("Shader parameter '{}' has no ready resource", Request.ParameterPath)));
            const auto BoundType = ResolveBindingType(Request.Bound);
            if (BoundType != (*It)->Info.Type)
                return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {}, received {}",
                                                           Request.ParameterPath,
                                                           magic_enum::enum_name((*It)->Info.Type),
                                                           magic_enum::enum_name(BoundType))));
            const auto BoundAccess = ResolveBindingAccess(Request.Bound);
            if (BoundAccess != ShaderBindingAccess::Unknown && (*It)->Info.Access != ShaderBindingAccess::Unknown &&
                BoundAccess != (*It)->Info.Access)
                return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {} access, received {}",
                                                           Request.ParameterPath,
                                                           magic_enum::enum_name((*It)->Info.Access),
                                                           magic_enum::enum_name(BoundAccess))));
            if ((*It)->Info.ArrayCount != 1)
                return std::unexpected(ErrorMessage(Format(
                    "Shader parameter '{}' is an array; use BindArrayResources instead", Request.ParameterPath)));
        }

        // Then we check if all set index are the same
        const auto SetIndex = BindingLocations.front().first;
        for (Uint32 Index = 0; Index < BindingLocations.size(); ++Index)
            for (Uint32 Previous = 0; Previous < Index; ++Previous)
                if (BindingLocations[Index] == BindingLocations[Previous])
                    return std::unexpected(
                        ErrorMessage(Format("Shader parameter '{}' appears more than once in a binding batch",
                                            Resources[Index].ParameterPath)));
        if (std::ranges::any_of(BindingLocations,
                                [SetIndex](const auto& Location) { return Location.first != SetIndex; })) {
            String Details;
            for (Uint32 Index = 0; Index < Resources.size(); ++Index) {
                if (!Details.empty())
                    Details += ", ";
                Details += Format("'{}' (set {}, binding {})",
                                  Resources[Index].ParameterPath,
                                  BindingLocations[Index].first,
                                  BindingLocations[Index].second);
            }
            return std::unexpected(
                ErrorMessage(Format("Shader binding batch contains multiple descriptor sets: {}", Details)));
        }
        // Sets that contain array slots are completed by separate BindArrayResources batches, so the
        // whole-set count check only applies to purely scalar sets.
        const bool SetHasArraySlots = std::ranges::any_of(
            m_BindingsBySet[SetIndex], [](const Slot& Binding) { return Binding.Info.ArrayCount != 1; });
        if (!SetHasArraySlots && Resources.size() != m_BindingsBySet[SetIndex].size())
            return std::unexpected(ErrorMessage(Format("Shader binding set {} requires {} resources, received {}",
                                                       SetIndex,
                                                       m_BindingsBySet[SetIndex].size(),
                                                       Resources.size())));

        // After checking, we write the resource info into the slot
        for (Uint32 Index = 0; Index < Resources.size(); ++Index) {
            const auto [BindingSet, BindingIndex] = BindingLocations[Index];
            auto&      SetBindings                = m_BindingsBySet[BindingSet];
            const auto Slot =
                std::ranges::find_if(SetBindings, [BindingIndex](const RHIShaderBindingSet::Slot& Candidate) {
                    return Candidate.Info.BindingIndex == BindingIndex;
                });
            Slot->Bound = Resources[Index].Bound;
        }
        return OnResourcesBound(SetIndex);
    }

    /// @brief Binds shader-side resource arrays (fixed or unbounded).
    ///
    /// One batch must target a single descriptor set; scalar slots of the same set are bound through
    /// BindResources.
    [[nodiscard]] auto BindArrayResources(std::span<const RHIShaderArrayBindingRequest> Requests)
        -> std::expected<void, ErrorMessage> {
        if (Requests.empty())
            return std::unexpected(ErrorMessage("Shader array binding batch must not be empty"));

        struct ResolvedRequest {
            Slot*                               Target  = nullptr;
            const RHIShaderArrayBindingRequest* Request = nullptr;
        };
        std::vector<ResolvedRequest> Resolved;
        Resolved.reserve(Requests.size());
        std::optional<Uint32> SetIndex = std::nullopt;

        for (const auto& Request : Requests) {
            if (Request.Elements.empty())
                return std::unexpected(
                    ErrorMessage(Format("Shader array parameter '{}' has no elements", Request.ParameterPath)));

            Slot* Target = nullptr;
            for (auto& SetBindings : m_BindingsBySet)
                for (auto& Binding : SetBindings)
                    if (Binding.Info.ParameterPath == Request.ParameterPath) {
                        Target = &Binding;
                        break;
                    }
            if (!Target)
                return std::unexpected(ErrorMessage(Format(
                    "Shader parameter '{}' is not present in the shader binding layout", Request.ParameterPath)));
            if (Target->Info.ArrayCount == 1)
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' is not an array; use BindResources instead", Request.ParameterPath)));
            const auto BoundType = ResolveBindingType(Request.Elements.front().View);
            if (BoundType != Target->Info.Type)
                return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {}, received {}",
                                                           Request.ParameterPath,
                                                           magic_enum::enum_name(Target->Info.Type),
                                                           magic_enum::enum_name(BoundType))));
            const auto BoundAccess = ResolveBindingAccess(Request.Elements.front().View);
            if (BoundAccess != ShaderBindingAccess::Unknown && Target->Info.Access != ShaderBindingAccess::Unknown &&
                BoundAccess != Target->Info.Access)
                return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {} access, received {}",
                                                           Request.ParameterPath,
                                                           magic_enum::enum_name(Target->Info.Access),
                                                           magic_enum::enum_name(BoundAccess))));
            if (!SetIndex)
                SetIndex = Target->Info.Set;
            else if (*SetIndex != Target->Info.Set)
                return std::unexpected(
                    ErrorMessage(Format("Shader array binding batch contains multiple descriptor sets: '{}' (set {})",
                                        Request.ParameterPath,
                                        Target->Info.Set)));

            // Reject duplicate (path, index) pairs and out-of-capacity indices.
            std::vector<Uint32> SeenIndices;
            SeenIndices.reserve(Request.Elements.size());
            for (const auto& Element : Request.Elements) {
                if (!Element.View.Texture.TryGet())
                    return std::unexpected(ErrorMessage(Format(
                        "Shader array parameter '{}' has an element with no ready resource", Request.ParameterPath)));
                if (std::ranges::find(SeenIndices, Element.ArrayIndex) != SeenIndices.end())
                    return std::unexpected(
                        ErrorMessage(Format("Shader array parameter '{}' binds index {} more than once",
                                            Request.ParameterPath,
                                            Element.ArrayIndex)));
                SeenIndices.push_back(Element.ArrayIndex);
                if (Element.ArrayIndex >= Target->ArrayCapacity)
                    return std::unexpected(
                        ErrorMessage(Format("Shader array parameter '{}' index {} exceeds the array capacity of {}",
                                            Request.ParameterPath,
                                            Element.ArrayIndex,
                                            Target->ArrayCapacity)));
            }
            Resolved.push_back(ResolvedRequest{.Target = Target, .Request = &Request});
        }

        for (auto& [Target, Request] : Resolved) {
            Target->ArrayElements.assign(Request->Elements.begin(), Request->Elements.end());
        }
        return OnResourcesBound(*SetIndex);
    }

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

    RHIShaderBindingSet(String Name, const RHIShaderBindingSetDesc& Desc)
        : RHIObject(std::move(Name)),
          m_BindlessSpace(Desc.Reflection.BindlessSpace),
          m_PushConstantRanges(Desc.Reflection.PushConstants),
          m_Stages(Desc.Stages) {
        // Step 1: Resolve set count
        Uint32 SetCount = 0;
        // +1 because set index start with 0
        for (const auto& Reflected : Desc.Reflection.Bindings)
            SetCount = std::max(SetCount, Reflected.Set + 1);
        if (m_BindlessSpace)
            SetCount = std::max(SetCount, *m_BindlessSpace + 1);

        // Step 2: sort all bindings by set and binding index
        m_BindingsBySet.resize(SetCount);
        for (const auto& Reflected : Desc.Reflection.Bindings)
            m_BindingsBySet[Reflected.Set].push_back(Slot{.Info = Reflected});
        for (auto& SetBindings : m_BindingsBySet)
            std::ranges::sort(SetBindings, [](const auto& Left, const auto& Right) {
                return Left.Info.BindingIndex < Right.Info.BindingIndex;
            });
    }

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
class RHIPipeline : public RHIObject {
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

struct RHIGraphicsPipelineDesc {
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

class RHIGraphicsPipeline : public RHIPipeline {
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
struct RHISpecializationConstant {
    String                                     Name  = {};
    std::variant<bool, Int32, Uint32, Float32> Value = false;

    bool operator==(const RHISpecializationConstant&) const = default;
};

/// @brief Validates provided spec values against the shader reflection: unknown names, duplicate names,
/// and variant/reflection scalar-type mismatches are hard errors.
///
/// Device-independent, so it lives here rather than in a backend.
[[nodiscard]] inline auto ValidateSpecializationConstants(std::span<const RHISpecializationConstant> Values,
                                                          const ShaderReflection&                    Reflection)
    -> std::expected<void, ErrorMessage> {
    std::vector<StringView> Seen;
    Seen.reserve(Values.size());
    for (const auto& Value : Values) {
        if (std::ranges::find(Seen, StringView{Value.Name}) != Seen.end())
            return std::unexpected(
                ErrorMessage(Format("Specialization constant '{}' is provided more than once", Value.Name)));
        Seen.push_back(Value.Name);

        const auto It = std::ranges::find_if(Reflection.SpecializationConstants,
                                             [&](const auto& Reflected) { return Reflected.Name == Value.Name; });
        if (It == Reflection.SpecializationConstants.end())
            return std::unexpected(ErrorMessage(
                Format("Specialization constant '{}' is not present in the shader reflection", Value.Name)));

        const auto Matches = [](const auto& Held, ShaderScalarType Type) {
            using HeldType = std::decay_t<decltype(Held)>;
            if constexpr (std::same_as<HeldType, bool>)
                return Type == ShaderScalarType::Bool;
            else if constexpr (std::same_as<HeldType, Int32>)
                return Type == ShaderScalarType::Int32;
            else if constexpr (std::same_as<HeldType, Uint32>)
                return Type == ShaderScalarType::Uint32;
            else
                return Type == ShaderScalarType::Float32;
        };
        if (!std::visit([&](const auto& Held) { return Matches(Held, It->ScalarType); }, Value.Value))
            return std::unexpected(ErrorMessage(
                Format("Specialization constant '{}' holds a value of the wrong scalar type (reflected as {})",
                       Value.Name,
                       magic_enum::enum_name(It->ScalarType))));
    }
    return {};
}

struct RHIComputePipelineDesc {
    ShaderComputeProgram                   Program                 = {};
    RHIRef<RHIShaderBindingSet>            BindingSet              = nullptr;
    /// @brief Values fixed at pipeline creation (VkSpecializationInfo).
    ///
    /// They are part of the pipeline identity: two value sets must yield two pipelines.
    std::vector<RHISpecializationConstant> SpecializationConstants = {};
};

class RHIComputePipeline : public RHIPipeline {
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

enum class RHIRayTracingShaderGroupType : Uint8 {
    Unknown = 0,
    General,
    TrianglesHit,
    ProceduralHit,
};

struct RHIRayTracingShaderGroupDesc {
    RHIRayTracingShaderGroupType Type = RHIRayTracingShaderGroupType::Unknown;
};

struct RHIRayTracingPipelineDesc {
    ShaderRayTracingProgram                   Program           = {};
    RHIRef<RHIShaderBindingSet>               BindingSet        = nullptr;
    std::vector<RHIRayTracingShaderGroupDesc> ShaderGroups      = {};
    Uint32                                    MaxRecursionDepth = 1;
};

class RHIRayTracingPipeline : public RHIPipeline {
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
