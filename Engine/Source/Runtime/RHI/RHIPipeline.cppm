module;

export module RHI:Pipeline;

export import Core;
export import :Types;
import :Ref;
import Shader;
export import std;

export namespace SoulEngine {

// ── RHIPipelineType ─────────────────────────────────────────────────────────

enum class RHIPipelineType : Uint8 {
    Unknown = 0,
    Graphics,
    Compute,
    RayTracing,
};

// ── RHIShaderBindingSet ─────────────────────────────────────────────────────

class RHIShaderBindingSet;

/// @brief Reflected shader interface used to construct a binding set.
struct RHIShaderBindingSetDesc {
    ShaderReflection Reflection = {};
    /// Bitmask of every stage in the shader program (magic_enum flags).
    ShaderStage      Stages     = ShaderStage::Unknown;
};

struct RHIShaderBindingRequest {
    StringView         ParameterPath  = {};
    RHIObject*         Resource       = nullptr;
    ShaderResourceType Type           = ShaderResourceType::Unknown;
    // Sampled textures and render targets share SampledTexture at the shader
    // level; retain this concrete kind so Vulkan can dispatch the raw pointer safely.
    bool               IsRenderTarget = false;
    bool               IsReadOnly     = true;

    RHIShaderBindingRequest() = default;

    template <typename T>
    RHIShaderBindingRequest(StringView ParameterPathIn, RHIRef<T> Resource, bool IsReadOnlyIn)
        : ParameterPath(ParameterPathIn),
          Resource(Resource.TryGet()),
          Type(Resource ? Resource->GetShaderBindingType() : ShaderResourceType::Unknown),
          IsRenderTarget(std::same_as<T, RHIRenderTarget>),
          IsReadOnly(IsReadOnlyIn) {}
};

/// @brief Backend-owned descriptor resources for one reflected shader layout.
class RHIShaderBindingSet : public RHIObject {
  public:
    /// @brief One reflected binding slot: the shader-side contract plus the
    /// bound resource.
    struct Slot {
        ShaderBinding      Info           = {};
        RHIObject*         Resource       = nullptr;
        // The shader type alone cannot distinguish sampled textures from
        // render targets after binding resources are type-erased.
        bool               IsRenderTarget = false;
        bool               IsReadOnly     = true;
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
        if (m_BindlessSpace && !m_BindlessTextures) {
            // Only require bindless textures when a binding actually references
            // the bindless space. Slang may allocate a bindless space index for
            // a module that never uses it (e.g. StructuredBuffer in ParameterBlock).
            const bool HasBindlessBinding = std::ranges::any_of(m_BindingsBySet, [&](const auto& Bindings) {
                return std::ranges::any_of(Bindings, [&](const Slot& Binding) {
                    return Binding.Info.Set == *m_BindlessSpace;
                });
            });
            if (HasBindlessBinding)
                return true;
        }
        return std::ranges::any_of(m_BindingsBySet, [](const auto& Bindings) {
            return std::ranges::any_of(Bindings, [](const Slot& Binding) { return Binding.Resource == nullptr; });
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
            if (!Request.Resource)
                return std::unexpected(
                    ErrorMessage(Format("Shader parameter '{}' has no ready resource", Request.ParameterPath)));
            if (Request.Type != (*It)->Info.Type)
                return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {}, received {}",
                                                           Request.ParameterPath,
                                                           magic_enum::enum_name((*It)->Info.Type),
                                                           magic_enum::enum_name(Request.Type))));
            if ((*It)->Info.ArrayCount != 1)
                return std::unexpected(ErrorMessage(Format(
                    "Shader parameter '{}' is an array and cannot use scalar batch binding", Request.ParameterPath)));
        }

        // Then we check if all set index are the same
        const auto SetIndex = BindingLocations.front().first;
        for (Uint32 Index = 0; Index < BindingLocations.size(); ++Index)
            for (Uint32 Previous = 0; Previous < Index; ++Previous)
                if (BindingLocations[Index] == BindingLocations[Previous])
                    return std::unexpected(ErrorMessage(
                        Format("Shader parameter '{}' appears more than once in a binding batch",
                               Resources[Index].ParameterPath)));
        if (std::ranges::any_of(BindingLocations, [SetIndex](const auto& Location) {
                return Location.first != SetIndex;
            })) {
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
        if (Resources.size() != m_BindingsBySet[SetIndex].size())
            return std::unexpected(ErrorMessage(Format("Shader binding set {} requires {} resources, received {}",
                                                       SetIndex,
                                                       m_BindingsBySet[SetIndex].size(),
                                                       Resources.size())));

        // After checking, we write the resource info into the slot
        for (Uint32 Index = 0; Index < Resources.size(); ++Index) {
            const auto [BindingSet, BindingIndex] = BindingLocations[Index];
            auto& SetBindings = m_BindingsBySet[BindingSet];
            const auto Slot = std::ranges::find_if(
                SetBindings, [BindingIndex](const RHIShaderBindingSet::Slot& Candidate) {
                    return Candidate.Info.BindingIndex == BindingIndex;
                });
            Slot->Resource = Resources[Index].Resource;
            Slot->IsRenderTarget = Resources[Index].IsRenderTarget;
            Slot->IsReadOnly = Resources[Index].IsReadOnly;
        }
        return OnResourcesBound(SetIndex);
    }

    /// Binds the sampled-image array used by the reflected bindless space.
    [[nodiscard]] auto BindBindlessResource(RHIRefArray<RHISampledTexture> Resource)
        -> std::expected<void, ErrorMessage> {
        if (!m_BindlessSpace)
            return std::unexpected(ErrorMessage("Shader binding set has no reflected bindless space"));
        if (!Resource)
            return std::unexpected(ErrorMessage("Bindless sampled-texture array is invalid"));
        m_BindlessTextures = std::move(Resource);
        return OnBindlessResourceBound();
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
    /// Applies backend-specific updates after the bindless array is stored.
    [[nodiscard]] virtual auto OnBindlessResourceBound() -> std::expected<void, ErrorMessage> = 0;

    std::vector<std::vector<Slot>>                m_BindingsBySet        = {};
    std::vector<PushConstantWrite>                m_PendingPushConstants = {};
    std::optional<RHIRefArray<RHISampledTexture>> m_BindlessTextures     = std::nullopt;
    std::optional<Uint32>                         m_BindlessSpace        = std::nullopt;
    std::vector<ShaderPushConstantRange>          m_PushConstantRanges   = {};
    ShaderStage                                   m_Stages               = ShaderStage::Unknown;
};

// ── Pipeline base ───────────────────────────────────────────────────────────

/// @brief Polymorphic base for all pipeline resources.
/// Backend concrete classes (e.g. VulkanGraphicsPipeline) own native pipeline state.
/// ResourceManager owns RHIPipeline instances; command lists only observe them.
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

// ── Graphics pipeline ───────────────────────────────────────────────────────

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

// ── Compute pipeline ────────────────────────────────────────────────────────

struct RHIComputePipelineDesc {
    ShaderComputeProgram        Program    = {};
    RHIRef<RHIShaderBindingSet> BindingSet = nullptr;
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

// ── Ray-tracing pipeline ────────────────────────────────────────────────────

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
