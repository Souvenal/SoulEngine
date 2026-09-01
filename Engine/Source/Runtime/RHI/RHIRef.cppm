module;

export module RHI:Ref;

export import Core;
import Shader;
export import std;

export namespace SoulEngine {

// ── Deferred Deletion Queue ───────────────────────────────────────────────

/// @brief Thread-safe queue of destruction callbacks drained on the RHI thread.
///
/// When an RHI resource's last reference is released, its destructor is
/// captured in a lambda and enqueued here.  The RHI thread calls Drain()
/// each frame to reclaim GPU resources safely.
class RHIDeferredDeletionQueue {
  public:
    RHIDeferredDeletionQueue()                                                   = default;
    RHIDeferredDeletionQueue(const RHIDeferredDeletionQueue&)                    = delete;
    auto operator=(const RHIDeferredDeletionQueue&) -> RHIDeferredDeletionQueue& = delete;

    /// @brief Enqueue a destruction callback. Thread-safe.
    auto Enqueue(std::function<void()> Destructor) -> void {
        std::scoped_lock Lock(m_Mutex);
        m_Pending.push_back(std::move(Destructor));
    }

    /// @brief Destroy all pending resources. Must run on the RHI thread.
    ///
    /// Destruction cascades: running one destructor can release further
    /// RHIRef payloads that enqueue their own destructors (e.g. a pipeline
    /// releases its shader binding set, which then releases bound textures).
    /// Drain in batches until the queue is empty so the final shutdown flush
    /// cannot leave cascade-enqueued destructors behind.
    auto Drain() -> void {
        for (;;) {
            std::vector<std::function<void()>> Batch;
            {
                std::scoped_lock Lock(m_Mutex);
                if (m_Pending.empty())
                    return;
                Batch = std::move(m_Pending);
            }
            for (auto& Destroy : Batch)
                Destroy();
        }
    }

  private:
    std::mutex                                            m_Mutex;
    std::vector<std::function<void()>> m_Pending;
};

/// @brief Module-owned default deferred deletion queue storage.
///
/// RHIRenderDevice::Create() points GDeferredDeletionQueue here; tests may
/// temporarily redirect the pointer to a local queue instance.
inline RHIDeferredDeletionQueue GDeferredDeletionQueueStorage = {};

/// @brief Process-wide deferred deletion queue. Set by RHIRenderDevice::Create().
RHIDeferredDeletionQueue* GDeferredDeletionQueue = nullptr;

/// @brief Drain the process-wide deferred deletion queue; no-op while no device is alive.
///
/// Driven by RHILoop on the RHI thread once per frame after
/// RHIRenderDevice::Tick(), and by backend Shutdown() for the final flush.
inline auto DrainRHIDeferredDeletions() -> void {
    if (GDeferredDeletionQueue)
        GDeferredDeletionQueue->Drain();
}

// ── RHI Ref State ─────────────────────────────────────────────────────────

enum class RHIRefState : Uint8 { Unknown = 0, RhiCommitting, GpuPending, Ready, Failed };

// ── RHI Ref Payload ───────────────────────────────────────

template <typename T>
class RHIRefPayload final {
  public:
    RHIRefPayload()                                        = default;
    RHIRefPayload(const RHIRefPayload&)                    = delete;
    auto operator=(const RHIRefPayload&) -> RHIRefPayload& = delete;
    RHIRefPayload(RHIRefPayload&&)                         = delete;
    auto operator=(RHIRefPayload&&) -> RHIRefPayload&      = delete;

    ~RHIRefPayload() {
        if (!m_Object)
            return;

        GDeferredDeletionQueue->Enqueue(
            [Object = std::make_shared<UPtr<T>>(std::move(m_Object))]() noexcept {});
    }

    [[nodiscard]] auto GetState() const -> RHIRefState {
        return m_State.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto GetError() const -> std::optional<ErrorMessage> {
        if (GetState() != RHIRefState::Failed)
            return std::nullopt;
        return m_Error;
    }

    auto MarkFailed(ErrorMessage Error) -> void {
        m_Error = std::move(Error);
        m_State.store(RHIRefState::Failed, std::memory_order_release);
    }

    [[nodiscard]] auto TryMarkReady() -> bool {
        auto Expected = RHIRefState::GpuPending;
        return m_State.compare_exchange_strong(
            Expected, RHIRefState::Ready, std::memory_order_release, std::memory_order_acquire);
    }

    [[nodiscard]] auto TryGet() const -> T* {
        return GetState() == RHIRefState::Ready ? m_Object.get() : nullptr;
    }

    [[nodiscard]] auto Publish(UPtr<T> Object, RHIRefState State) -> bool {
        if (!Object || (State != RHIRefState::GpuPending && State != RHIRefState::Ready) || m_Object ||
            GetState() != RHIRefState::RhiCommitting)
            return false;

        m_Object = std::move(Object);
        m_State.store(State, std::memory_order_release);
        return true;
    }

    auto Publish(RHIRefPayload& Source, RHIRefState State) -> bool {
        if (this == &Source || (State != RHIRefState::GpuPending && State != RHIRefState::Ready) ||
            GetState() != RHIRefState::RhiCommitting || m_Object || Source.GetState() != RHIRefState::Ready ||
            !Source.m_Object)
            return false;

        m_Object = std::move(Source.m_Object);
        m_State.store(State, std::memory_order_release);
        return true;
    }

  private:
    std::atomic<RHIRefState>    m_State  = RHIRefState::RhiCommitting;
    std::optional<ErrorMessage> m_Error  = std::nullopt;
    UPtr<T>                     m_Object = nullptr;
};

template <typename T>
class RHIRef {
  public:
    /// @brief Create an empty resource handle.
    RHIRef() noexcept = default;
    
    /// @brief Create an explicitly empty resource handle.
    RHIRef(std::nullptr_t) noexcept {}

    /// @brief Create a pending resource handle for asynchronous creation.
    [[nodiscard]] static auto Create() -> RHIRef {
        RHIRef Ref;
        Ref.m_Payload = std::make_shared<RHIRefPayload<T>>();
        return Ref;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return m_Payload != nullptr && TryGet() != nullptr;
    }

    [[nodiscard]] auto operator==(const RHIRef& Other) const noexcept -> bool {
        return m_Payload == Other.m_Payload;
    }

    [[nodiscard]] auto operator->() const noexcept -> T* {
        return TryGet();
    }

    [[nodiscard]] auto operator*() const -> T& {
        return *TryGet();
    }

    [[nodiscard]] auto TryGet() const -> T* {
        return m_Payload ? m_Payload->TryGet() : nullptr;
    }

    [[nodiscard]] auto GetState() const -> RHIRefState {
        return m_Payload ? m_Payload->GetState() : RHIRefState::Unknown;
    }

    [[nodiscard]] auto GetError() const -> std::optional<ErrorMessage> {
        return m_Payload ? m_Payload->GetError() : std::nullopt;
    }

    /// @brief Used only when a pending creation task cannot be queued.
    auto MarkFailed(ErrorMessage Error) const -> void {
        if (m_Payload)
            m_Payload->MarkFailed(std::move(Error));
    }

    [[nodiscard]] auto Publish(UPtr<T> Object, RHIRefState State) const -> std::expected<void, ErrorMessage> {
        if (!m_Payload || !m_Payload->Publish(std::move(Object), State))
            return std::unexpected(ErrorMessage("Cannot publish an invalid RHI resource payload"));
        return {};
    }

    [[nodiscard]] auto Publish(RHIRef&& Source, RHIRefState State) const -> std::expected<void, ErrorMessage> {
        if (!m_Payload || !Source.m_Payload)
            return std::unexpected(ErrorMessage("Cannot publish from an empty RHI resource reference"));
        if (!m_Payload->Publish(*Source.m_Payload, State))
            return std::unexpected(ErrorMessage("Cannot publish an invalid RHI resource payload"));
        return {};
    }

    SPtr<RHIRefPayload<T>> m_Payload = nullptr;

    friend class RHIRenderDevice;
};

/// @brief Shared CPU payload for an append-only bindless resource array.
///
/// This payload owns the resource list and append-cycle state without
/// participating in the RHI deferred-deletion queue.
template <typename T>
class RHIRefArrayPayload final {
  public:
    static constexpr Uint32 FrameSlotCount = 3;

    // Descriptor slot 0 is the null-handle sentinel used by the material GPU
    // ABI. All descriptor-facing resource indices therefore start at 1.
    static constexpr Uint32 NullDescriptorSlot  = 0;
    static constexpr Uint32 FirstResourceSlot   = 1;

    auto BeginAppend() -> void {
        std::scoped_lock Lock(m_Mutex);
        m_CurrentAppendCycle = (m_CurrentAppendCycle + 1) % FrameSlotCount;
        m_AppendCycles[m_CurrentAppendCycle].clear();
    }

    [[nodiscard]] auto Append(RHIRef<T> Resource) -> std::expected<void, ErrorMessage> {
        if (Resource.GetState() == RHIRefState::Unknown)
            return std::unexpected(
                ErrorMessage("Cannot append an invalid resource to bindless array"));
        std::scoped_lock Lock(m_Mutex);
        if (std::ranges::any_of(m_Resources, [&Resource](const RHIRef<T>& Existing) {
                return Existing == Resource;
            }))
            return {};
        // m_Resources[0] is permanently reserved for the null descriptor.
        const Uint32 Index = static_cast<Uint32>(m_Resources.size());
        m_Resources.push_back(std::move(Resource));
        m_AppendCycles[m_CurrentAppendCycle].emplace_back(Index, m_Resources.back());
        return {};
    }

    auto EndAppend() -> void {}

    [[nodiscard]] auto GetChangedElements() const
        -> std::vector<std::pair<Uint32, RHIRef<T>>> {
        std::scoped_lock Lock(m_Mutex);
        std::vector<std::pair<Uint32, RHIRef<T>>> Changed;
        for (Uint32 Cycle = 0; Cycle < FrameSlotCount; ++Cycle) {
            Changed.append_range(m_AppendCycles[Cycle]);
        }
        return Changed;
    }

    /// Copy out one slot's ref under the lock.
    [[nodiscard]] auto GetElement(Uint32 Slot) const -> RHIRef<T> {
        std::scoped_lock Lock(m_Mutex);
        return Slot < m_Resources.size() ? m_Resources[Slot] : RHIRef<T>{};
    }

    /// Return the descriptor-array extent, including the reserved null slot.
    [[nodiscard]] auto GetSize() const -> Uint32 {
        std::scoped_lock Lock(m_Mutex);
        return static_cast<Uint32>(m_Resources.size());
    }

    /// Return the descriptor-facing slot, where slot 0 is the null sentinel.
    [[nodiscard]] auto FindIndex(const RHIRef<T>& Resource) const -> std::optional<Uint32> {
        std::scoped_lock Lock(m_Mutex);
        for (Uint32 Index = 0; Index < m_Resources.size(); ++Index)
            if (m_Resources[Index] == Resource)
                return Index;
        return std::nullopt;
    }

  private:
    mutable std::mutex     m_Mutex       = {};
    // Keep slot 0 empty so a zeroed shader handle never aliases a live resource.
    std::vector<RHIRef<T>> m_Resources   = {RHIRef<T>{}};
    std::array<std::vector<std::pair<Uint32, RHIRef<T>>>, FrameSlotCount> m_AppendCycles = {};
    Uint32                 m_CurrentAppendCycle = 0;
};

/// @brief Copyable handle to a shared append-only bindless resource array.
///
/// The array is a CPU-side object. Its payload owns the resource list and the
/// three FrameSlot append-cycle buffers; copied handles share that state.
template <typename T>
class RHIRefArray {
  public:
    static constexpr Uint32 FrameSlotCount = RHIRefArrayPayload<T>::FrameSlotCount;
    static constexpr Uint32 NullDescriptorSlot = RHIRefArrayPayload<T>::NullDescriptorSlot;
    static constexpr Uint32 FirstResourceSlot  = RHIRefArrayPayload<T>::FirstResourceSlot;

    RHIRefArray() : m_Payload(std::make_shared<RHIRefArrayPayload<T>>()) {}

    RHIRefArray(const RHIRefArray&)                    = default;
    auto operator=(const RHIRefArray&) -> RHIRefArray& = default;
    RHIRefArray(RHIRefArray&&)                         = default;
    auto operator=(RHIRefArray&&) -> RHIRefArray&      = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return m_Payload != nullptr;
    }

    [[nodiscard]] auto operator==(const RHIRefArray& Other) const noexcept -> bool {
        return m_Payload == Other.m_Payload;
    }

    /// Begin one GameThread append cycle.
    ///
    /// The GameThread must execute exactly one BeginAppend()/EndAppend() pair
    /// per loop. The pair is a producer-side protocol; the array does not
    /// track or validate loop ownership.
    auto BeginAppend() -> void {
        if (m_Payload)
            m_Payload->BeginAppend();
    }

    /// Retain Resource unless an identical resource is already present.
    [[nodiscard]] auto Append(RHIRef<T> Resource) -> std::expected<void, ErrorMessage> {
        if (!m_Payload)
            return std::unexpected(ErrorMessage("Cannot append to an invalid resource array"));
        return m_Payload->Append(std::move(Resource));
    }

    /// Finish the current GameThread append cycle.
    auto EndAppend() -> void {
        if (m_Payload)
            m_Payload->EndAppend();
    }

    /// Return all appended elements from the retained FrameSlot cycles.
    [[nodiscard]] auto GetChangedElements() const
        -> std::vector<std::pair<Uint32, RHIRef<T>>> {
        if (!m_Payload)
            return {};
        return m_Payload->GetChangedElements();
    }

    [[nodiscard]] auto GetElement(Uint32 Slot) const -> RHIRef<T> {
        if (!m_Payload)
            return {};
        return m_Payload->GetElement(Slot);
    }

    /// Return the descriptor-array extent, including the reserved null slot.
    [[nodiscard]] auto GetSize() const -> Uint32 {
        if (!m_Payload)
            return 0;
        return m_Payload->GetSize();
    }

    [[nodiscard]] auto FindIndex(const RHIRef<T>& Resource) const -> std::optional<Uint32> {
        if (!m_Payload)
            return std::nullopt;
        return m_Payload->FindIndex(Resource);
    }

  private:
    SPtr<RHIRefArrayPayload<T>> m_Payload = nullptr;
};

} // namespace SoulEngine
