module;

export module WindowSystem.Input;

import std;
import Core;
import EditorCommands;
import WindowSystem.Types;

namespace SoulEngine {

/// @brief Publishes bounded, frame-visible input snapshots on Game.
export class InputFrameState {
  public:
    explicit InputFrameState(std::size_t PendingCapacity = 256) : m_PendingCapacity(PendingCapacity) {}

    /// @brief Queue one executed event and report pending-capacity exhaustion after scheduling a reset.
    [[nodiscard]] auto Apply(const InputEvent& Event) -> std::expected<void, ErrorMessage>;

    /// @brief Publish the next ordered frame, retaining later edges for subsequent frames.
    [[nodiscard]] auto AdvanceFrame() -> const InputFrame&;

    [[nodiscard]] auto GetFrame() const -> const InputFrame& {
        return m_Frame;
    }

    /// @brief Clear pending and published held state during owner teardown.
    auto Shutdown() -> void;

  private:
    struct NativeKeyState {
        InputState State    = InputState::Unknown;
        WindowKey  Key      = WindowKey::Unknown;
        Int32      Scancode = 0;
    };

    auto ResetPending(const InputEvent& Reset) -> void;

    std::size_t                     m_PendingCapacity = 256;
    std::deque<InputEvent>          m_Pending;
    InputFrame                      m_Frame;
    std::map<Int32, NativeKeyState> m_NativeKeys;
};

/// @brief Registers the internal input command and submits owning physical events on Game.
export class InputCommandProvider {
  public:
    explicit InputCommandProvider(InputFrameState& State) : m_State(&State) {}
    ~InputCommandProvider();

    InputCommandProvider(const InputCommandProvider&)                    = delete;
    auto operator=(const InputCommandProvider&) -> InputCommandProvider& = delete;
    InputCommandProvider(InputCommandProvider&&)                         = delete;
    auto operator=(InputCommandProvider&&) -> InputCommandProvider&      = delete;

    [[nodiscard]] auto Bind(CommandDispatcher& Commands) -> std::expected<void, ErrorMessage>;
    auto               Unbind() -> void;

    /// @brief Submit one physical event and start reset recovery on admission failure.
    [[nodiscard]] auto Submit(InputEvent Event) -> std::expected<void, ErrorMessage>;

    /// @brief Retry recovery admission and ordered focus/cursor transitions on Game.
    auto RetryRecovery() -> void;

  private:
    auto UpdateIntent(const InputEvent& Event) -> void;
    auto SubmitRecovery() -> void;
    auto FlushRecovery() -> void;
    auto FinishRecovery() -> void;

    InputFrameState*             m_State                  = nullptr;
    CommandDispatcher*           m_Commands               = nullptr;
    Uint64                       m_Registration           = 0;
    bool                         m_IsRecovering           = false;
    bool                         m_IsRecoveryPending      = false;
    bool                         m_IsResetExecuted        = false;
    bool                         m_IsFocusIntended        = true;
    bool                         m_IsCursorInsideIntended = false;
    std::deque<InputEvent>       m_DeferredRecovery;
    std::size_t                  m_RecoveryOutstanding = 0;
    static constexpr std::size_t RecoveryCapacity      = 64;
};

} // namespace SoulEngine
