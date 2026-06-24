export module Resource:Types;

export import Core;
export import RHI;
export import ShaderCompiler;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

/// @brief Runtime state for asynchronous resources.
enum class ResourceState : Uint8 {
    Unknown = 0,
    CpuPreparing,
    RhiCommitting,
    GpuPending,
    Ready,
    Failed,
    Stale,
};

/// @brief Caller policy when an asynchronous resource is not ready.
enum class ResourceWaitPolicy : Uint8 {
    Skip = 0,
    Block,
};

using ResourceId         = Uint64;
using ResourceGeneration = Uint64;

/// @brief Logical texture resource published through ResourceHandle.
struct SampledTextureResource {
    SPtr<RHI::SampledTexture> Texture = nullptr;
};

/// @brief Logical graphics pipeline resource published through ResourceHandle.
struct GraphicsPipelineResource {
    SPtr<RHI::GraphicsPipeline> Pipeline = nullptr;
};

/// @brief Async graphics pipeline request descriptor.
struct GraphicsPipelineRequest {
    ShaderCompiler::ShaderEntry VertEntry         = {};
    ShaderCompiler::ShaderEntry FragEntry         = {};
    RHI::VertexInputLayoutDesc  VertexInputLayout = {};
    RHI::PrimitiveTopology      Topology          = RHI::PrimitiveTopology::TriangleList;
    RHI::RasterizerState        Rasterizer        = {};
    RHI::BlendState             Blend             = {};
    RHI::DepthStencilState      DepthStencil      = {};
    RHI::Format                 ColorFormat       = RHI::Format::B8G8R8A8_UNORM;
    RHI::Format                 DepthFormat       = RHI::Format::Unknown;
};

template <typename T>
class ResourceSlot {
  public:
    ResourceSlot() = default;

    ResourceSlot(const ResourceSlot&)                    = delete;
    auto operator=(const ResourceSlot&) -> ResourceSlot& = delete;
    ResourceSlot(ResourceSlot&&)                         = delete;
    auto operator=(ResourceSlot&&) -> ResourceSlot&      = delete;
    ~ResourceSlot()                                      = default;

    [[nodiscard]] auto Reset(ResourceId Id) -> ResourceGeneration {
        std::lock_guard Lock(m_Mutex);
        m_Id = Id;
        ++m_Generation;
        m_State = ResourceState::CpuPreparing;
        m_Value.reset();
        m_Error.reset();
        return m_Generation;
    }

    [[nodiscard]] auto MarkCpuPreparing(ResourceGeneration Generation) -> bool {
        return SetState(Generation, ResourceState::CpuPreparing);
    }

    [[nodiscard]] auto MarkRhiCommitting(ResourceGeneration Generation) -> bool {
        return SetState(Generation, ResourceState::RhiCommitting);
    }

    [[nodiscard]] auto PublishGpuPending(ResourceGeneration Generation) -> bool {
        return SetState(Generation, ResourceState::GpuPending);
    }

    [[nodiscard]] auto PublishReady(ResourceGeneration Generation, T Value) -> bool {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation)
            return false;

        m_Value = std::move(Value);
        m_Error.reset();
        m_State = ResourceState::Ready;
        return true;
    }

    [[nodiscard]] auto PublishFailed(ResourceGeneration Generation, ErrorMessage Error) -> bool {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation)
            return false;

        m_Value.reset();
        m_Error = std::move(Error);
        m_State = ResourceState::Failed;
        return true;
    }

    [[nodiscard]] auto GetState(ResourceGeneration Generation) const -> ResourceState {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation)
            return ResourceState::Stale;
        return m_State;
    }

    [[nodiscard]] auto TryGet(ResourceGeneration Generation) const -> std::optional<T> {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation || m_State != ResourceState::Ready || !m_Value)
            return std::nullopt;
        return *m_Value;
    }

    [[nodiscard]] auto GetError(ResourceGeneration Generation) const -> std::optional<ErrorMessage> {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation || m_State != ResourceState::Failed || !m_Error)
            return std::nullopt;
        return *m_Error;
    }

    [[nodiscard]] auto GetId() const -> ResourceId {
        std::lock_guard Lock(m_Mutex);
        return m_Id;
    }

    [[nodiscard]] auto GetGeneration() const -> ResourceGeneration {
        std::lock_guard Lock(m_Mutex);
        return m_Generation;
    }

  private:
    [[nodiscard]] auto SetState(ResourceGeneration Generation, ResourceState State) -> bool {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation)
            return false;

        m_State = State;
        return true;
    }

    mutable std::mutex          m_Mutex;
    ResourceId                  m_Id         = 0;
    ResourceGeneration          m_Generation = 0;
    ResourceState               m_State      = ResourceState::Unknown;
    std::optional<T>            m_Value      = std::nullopt;
    std::optional<ErrorMessage> m_Error      = std::nullopt;
};

/// @brief Typed handle for an asynchronously published resource.
template <typename T>
class ResourceHandle {
  public:
    ResourceHandle() = default;

    [[nodiscard]] static auto Create(SPtr<ResourceSlot<T>> Slot, ResourceId Id, ResourceGeneration Generation)
        -> ResourceHandle {
        ResourceHandle Handle;
        Handle.m_Slot       = std::move(Slot);
        Handle.m_Id         = Id;
        Handle.m_Generation = Generation;
        return Handle;
    }

    [[nodiscard]] auto IsValid() const -> bool {
        return static_cast<bool>(m_Slot);
    }

    [[nodiscard]] auto GetState() const -> ResourceState {
        if (!m_Slot)
            return ResourceState::Unknown;
        return m_Slot->GetState(m_Generation);
    }

    [[nodiscard]] auto TryGet() const -> std::optional<T> {
        if (!m_Slot)
            return std::nullopt;
        return m_Slot->TryGet(m_Generation);
    }

    [[nodiscard]] auto GetError() const -> std::optional<ErrorMessage> {
        if (!m_Slot)
            return std::nullopt;
        return m_Slot->GetError(m_Generation);
    }

    [[nodiscard]] auto GetId() const -> ResourceId {
        return m_Id;
    }

    [[nodiscard]] auto GetGeneration() const -> ResourceGeneration {
        return m_Generation;
    }

  private:
    SPtr<ResourceSlot<T>> m_Slot       = nullptr;
    ResourceId            m_Id         = 0;
    ResourceGeneration    m_Generation = 0;
};

using SampledTextureHandle   = ResourceHandle<SampledTextureResource>;
using GraphicsPipelineHandle = ResourceHandle<GraphicsPipelineResource>;

template <typename T>
struct ResourceAllocation {
    SPtr<ResourceSlot<T>> Slot   = nullptr;
    ResourceHandle<T>     Handle = {};
};

template <typename T>
[[nodiscard]] auto CreatePendingResource(ResourceId Id) -> ResourceAllocation<T> {
    auto Slot       = std::make_shared<ResourceSlot<T>>();
    auto Generation = Slot->Reset(Id);
    return ResourceAllocation<T>{
        .Slot   = Slot,
        .Handle = ResourceHandle<T>::Create(Slot, Id, Generation),
    };
}

} // namespace SoulEngine::Resource
