module;

export module RHI:Ref;

export import Core;
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
    auto Drain() -> void {
        std::vector<std::function<void()>> Batch;
        {
            std::scoped_lock Lock(m_Mutex);
            Batch = std::move(m_Pending);
        }
        for (auto& Destroy : Batch)
            Destroy();
    }

  private:
    std::mutex                                            m_Mutex;
    std::vector<std::function<void()>> m_Pending;
};

/// @brief Process-wide deferred deletion queue. Set by RHIRenderDevice::Create().
RHIDeferredDeletionQueue* GDeferredDeletionQueue = nullptr;

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

    [[nodiscard]] auto IsValid() const noexcept -> bool {
        return m_Payload != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return IsValid();
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

    SPtr<RHIRefPayload<T>> m_Payload = nullptr;

    friend class RHIRenderDevice;
};

} // namespace SoulEngine
