module;

#include <nlohmann/json.hpp>

module WindowSystem.Input;

import magic_enum;
import std;
import Core;
import EditorCommands;
import WindowSystem.Types;

namespace SoulEngine {

[[nodiscard]] auto ModifiersToJson(const InputModifiers& Modifiers) -> CommandJson {
    return CommandJson{{"shift", Modifiers.IsShift},
                       {"control", Modifiers.IsControl},
                       {"alt", Modifiers.IsAlt},
                       {"super", Modifiers.IsSuper}};
}

[[nodiscard]] auto ToJson(const InputEvent& Event) -> CommandJson {
    return std::visit(
        []<class T>(const T& Value) -> CommandJson {
            if constexpr (std::is_same_v<T, PointerInputEvent>)
                return CommandJson{{"type", "pointer"}, {"x", Value.Position.X}, {"y", Value.Position.Y}};
            else if constexpr (std::is_same_v<T, MouseButtonInputEvent>)
                return CommandJson{{"type", "button"},
                                   {"button", std::to_underlying(Value.Button)},
                                   {"action", std::to_underlying(Value.Action)},
                                   {"modifiers", ModifiersToJson(Value.Modifiers)}};
            else if constexpr (std::is_same_v<T, WheelInputEvent>)
                return CommandJson{{"type", "wheel"}, {"x", Value.X}, {"y", Value.Y}};
            else if constexpr (std::is_same_v<T, KeyInputEvent>)
                return CommandJson{{"type", "key"},
                                   {"key", std::to_underlying(Value.Key)},
                                   {"native", Value.NativeKey},
                                   {"scancode", Value.Scancode},
                                   {"action", std::to_underlying(Value.Action)},
                                   {"modifiers", ModifiersToJson(Value.Modifiers)}};
            else if constexpr (std::is_same_v<T, TextInputEvent>)
                return CommandJson{{"type", "text"}, {"codepoint", Value.Codepoint}};
            else if constexpr (std::is_same_v<T, FocusInputEvent>)
                return CommandJson{{"type", "focus"}, {"focused", Value.IsFocused}};
            else if constexpr (std::is_same_v<T, CursorEnterInputEvent>)
                return CommandJson{{"type", "cursor_enter"}, {"inside", Value.IsInside}};
            else
                return CommandJson{{"type", "reset"}};
        },
        Event);
}

[[nodiscard]] auto Member(const CommandJson& Json, StringView Name) -> const CommandJson* {
    const auto It = Json.find(String(Name));
    return It == Json.end() ? nullptr : &*It;
}

template <class T>
[[nodiscard]] auto ReadInteger(const CommandJson& Json, StringView Name, T& Output) -> bool {
    const auto* Value = Member(Json, Name);
    if (!Value || !Value->is_number_integer())
        return false;
    if (Value->is_number_unsigned()) {
        const auto Number = Value->get<Uint64>();
        if (!std::in_range<T>(Number))
            return false;
        Output = static_cast<T>(Number);
        return true;
    }
    const auto Number = Value->get<Int64>();
    if (!std::in_range<T>(Number))
        return false;
    Output = static_cast<T>(Number);
    return true;
}

[[nodiscard]] auto ReadFloat(const CommandJson& Json, StringView Name, Float32& Output) -> bool {
    const auto* Value = Member(Json, Name);
    if (!Value || !Value->is_number())
        return false;
    const auto Number = Value->get<double>();
    if (!std::isfinite(Number) || Number < std::numeric_limits<Float32>::lowest() ||
        Number > std::numeric_limits<Float32>::max())
        return false;
    Output = static_cast<Float32>(Number);
    return true;
}

[[nodiscard]] auto ReadBool(const CommandJson& Json, StringView Name, bool& Output) -> bool {
    const auto* Value = Member(Json, Name);
    if (!Value || !Value->is_boolean())
        return false;
    Output = Value->get<bool>();
    return true;
}

template <class T>
[[nodiscard]] auto ReadEnum(const CommandJson& Json, StringView Name, T& Output) -> bool {
    Int32 Value = 0;
    if (!ReadInteger(Json, Name, Value))
        return false;
    const auto Enumerator = magic_enum::enum_cast<T>(Value);
    if (!Enumerator)
        return false;
    Output = *Enumerator;
    return true;
}

[[nodiscard]] auto ReadModifiers(const CommandJson& Json, InputModifiers& Output) -> bool {
    const auto* Value = Member(Json, "modifiers");
    return Value && Value->is_object() && ReadBool(*Value, "shift", Output.IsShift) &&
           ReadBool(*Value, "control", Output.IsControl) && ReadBool(*Value, "alt", Output.IsAlt) &&
           ReadBool(*Value, "super", Output.IsSuper);
}

[[nodiscard]] auto ParseEvent(const CommandJson& Json) -> std::expected<InputEvent, ErrorMessage> {
    auto Invalid = []() -> std::expected<InputEvent, ErrorMessage> {
        return std::unexpected(ErrorMessage("Invalid window input event"));
    };
    if (!Json.is_object())
        return Invalid();
    const auto* TypeValue = Member(Json, "type");
    if (!TypeValue || !TypeValue->is_string())
        return Invalid();
    const auto Type = TypeValue->get<String>();
    if (Type == "reset")
        return InputEvent{ResetInputEvent{}};
    if (Type == "pointer") {
        PointerInputEvent Event;
        if (!ReadFloat(Json, "x", Event.Position.X) || !ReadFloat(Json, "y", Event.Position.Y))
            return Invalid();
        return InputEvent{Event};
    }
    if (Type == "wheel") {
        WheelInputEvent Event;
        if (!ReadFloat(Json, "x", Event.X) || !ReadFloat(Json, "y", Event.Y))
            return Invalid();
        return InputEvent{Event};
    }
    if (Type == "text") {
        TextInputEvent Event;
        if (!ReadInteger(Json, "codepoint", Event.Codepoint))
            return Invalid();
        return InputEvent{Event};
    }
    if (Type == "focus") {
        FocusInputEvent Event;
        if (!ReadBool(Json, "focused", Event.IsFocused))
            return Invalid();
        return InputEvent{Event};
    }
    if (Type == "cursor_enter") {
        CursorEnterInputEvent Event;
        if (!ReadBool(Json, "inside", Event.IsInside))
            return Invalid();
        return InputEvent{Event};
    }
    if (Type == "button") {
        MouseButtonInputEvent Event;
        if (!ReadEnum(Json, "button", Event.Button) || Event.Button == WindowMouseButton::Unknown ||
            !ReadEnum(Json, "action", Event.Action) ||
            (Event.Action != InputAction::Press && Event.Action != InputAction::Release) ||
            !ReadModifiers(Json, Event.Modifiers))
            return Invalid();
        return InputEvent{Event};
    }
    if (Type == "key") {
        KeyInputEvent Event;
        if (!ReadEnum(Json, "key", Event.Key) || !ReadInteger(Json, "native", Event.NativeKey) || Event.NativeKey < 0 ||
            !ReadInteger(Json, "scancode", Event.Scancode) || !ReadEnum(Json, "action", Event.Action) ||
            Event.Action == InputAction::Unknown || !ReadModifiers(Json, Event.Modifiers))
            return Invalid();
        return InputEvent{Event};
    }
    return Invalid();
}

auto AdvanceStableState(InputState& State) -> void {
    if (State == InputState::Pressed)
        State = InputState::Held;
    else if (State == InputState::Released || State == InputState::Unknown)
        State = InputState::Up;
}

[[nodiscard]] auto IsDown(InputState State) -> bool {
    return State == InputState::Pressed || State == InputState::Held;
}

auto ApplyTransition(InputState& State, InputAction Action) -> void {
    if (Action == InputAction::Press)
        State = InputState::Pressed;
    else if (Action == InputAction::Release)
        State = IsDown(State) ? InputState::Released : InputState::Up;
}

auto InputFrameState::ResetPending(const InputEvent& Reset) -> void {
    m_Pending.clear();
    for (const auto& [NativeKey, Key] : m_NativeKeys) {
        if (!IsDown(Key.State))
            continue;
        m_Pending.emplace_back(KeyInputEvent{
            .Key = Key.Key, .NativeKey = NativeKey, .Scancode = Key.Scancode, .Action = InputAction::Release});
    }
    for (std::size_t Index = 1; Index < m_Frame.Mouse.ButtonStates.size(); ++Index) {
        if (!IsDown(m_Frame.Mouse.ButtonStates[Index]))
            continue;
        m_Pending.emplace_back(
            MouseButtonInputEvent{.Button = static_cast<WindowMouseButton>(Index), .Action = InputAction::Release});
    }
    m_Pending.push_back(Reset);
}

auto InputFrameState::Apply(const InputEvent& Event) -> std::expected<void, ErrorMessage> {
    const auto* Focus = std::get_if<FocusInputEvent>(&Event);
    if (std::holds_alternative<ResetInputEvent>(Event) || (Focus && !Focus->IsFocused)) {
        ResetPending(Event);
        return {};
    }
    if (const auto* Pointer = std::get_if<PointerInputEvent>(&Event); Pointer && !m_Pending.empty()) {
        if (auto* Previous = std::get_if<PointerInputEvent>(&m_Pending.back())) {
            Previous->Position = Pointer->Position;
            return {};
        }
    }
    if (const auto* Wheel = std::get_if<WheelInputEvent>(&Event)) {
        if (auto* Previous = m_Pending.empty() ? nullptr : std::get_if<WheelInputEvent>(&m_Pending.back())) {
            Previous->X += Wheel->X;
            Previous->Y += Wheel->Y;
            return {};
        }
    }
    if (m_Pending.size() >= m_PendingCapacity) {
        ResetPending(InputEvent{ResetInputEvent{}});
        return std::unexpected(ErrorMessage("Pending input capacity reached; held input was reset"));
    }
    m_Pending.push_back(Event);
    return {};
}

auto InputFrameState::AdvanceFrame() -> const InputFrame& {
    m_Frame.Events.clear();
    m_Frame.Mouse.PreviousCursorPosition = m_Frame.Mouse.CurrentCursorPosition;
    m_Frame.Mouse.ScrollX                = 0.0f;
    m_Frame.Mouse.ScrollY                = 0.0f;
    for (auto& State : m_Frame.Keyboard.States)
        AdvanceStableState(State);
    for (auto& State : m_Frame.Mouse.ButtonStates)
        AdvanceStableState(State);
    for (auto& [NativeKey, Key] : m_NativeKeys)
        AdvanceStableState(Key.State);

    std::unordered_set<Int32> TransitionedKeys;
    std::unordered_set<Int32> TransitionedButtons;
    while (!m_Pending.empty()) {
        const auto& Next = m_Pending.front();
        if (const auto* Key = std::get_if<KeyInputEvent>(&Next);
            Key && Key->Action != InputAction::Repeat && !TransitionedKeys.insert(Key->NativeKey).second)
            break;
        if (const auto* Button = std::get_if<MouseButtonInputEvent>(&Next);
            Button && !TransitionedButtons.insert(std::to_underlying(Button->Button)).second)
            break;

        auto Event = std::move(m_Pending.front());
        m_Pending.pop_front();
        const bool StopAfter = std::holds_alternative<ResetInputEvent>(Event) ||
                               (std::get_if<FocusInputEvent>(&Event) && !std::get<FocusInputEvent>(Event).IsFocused);
        std::visit(
            [this]<class T>(const T& Value) -> void {
                if constexpr (std::is_same_v<T, PointerInputEvent>) {
                    m_Frame.Mouse.CurrentCursorPosition = Value.Position;
                } else if constexpr (std::is_same_v<T, WheelInputEvent>) {
                    m_Frame.Mouse.ScrollX += Value.X;
                    m_Frame.Mouse.ScrollY += Value.Y;
                } else if constexpr (std::is_same_v<T, TextInputEvent>) {
                } else if constexpr (std::is_same_v<T, MouseButtonInputEvent>) {
                    m_Frame.Keyboard.Modifiers = Value.Modifiers;
                    const auto Index           = std::to_underlying(Value.Button);
                    if (Index > 0 && Index < m_Frame.Mouse.ButtonStates.size())
                        ApplyTransition(m_Frame.Mouse.ButtonStates[Index], Value.Action);
                } else if constexpr (std::is_same_v<T, KeyInputEvent>) {
                    m_Frame.Keyboard.Modifiers = Value.Modifiers;
                    auto& Native               = m_NativeKeys[Value.NativeKey];
                    Native.Key                 = Value.Key;
                    Native.Scancode            = Value.Scancode;
                    ApplyTransition(Native.State, Value.Action);
                    const auto Index = std::to_underlying(Value.Key);
                    if (Index > 0 && Index < m_Frame.Keyboard.States.size())
                        ApplyTransition(m_Frame.Keyboard.States[Index], Value.Action);
                } else if constexpr (std::is_same_v<T, FocusInputEvent>) {
                    m_Frame.IsFocused = Value.IsFocused;
                    if (!Value.IsFocused)
                        m_Frame.Keyboard.Modifiers = {};
                } else if constexpr (std::is_same_v<T, CursorEnterInputEvent>) {
                    m_Frame.IsCursorInside = Value.IsInside;
                } else if constexpr (std::is_same_v<T, ResetInputEvent>) {
                    m_Frame.Keyboard.Modifiers = {};
                    m_Frame.IsFocused          = false;
                    m_Frame.IsCursorInside     = false;
                }
            },
            Event);
        m_Frame.Events.push_back(std::move(Event));
        if (StopAfter)
            break;
    }
    return m_Frame;
}

auto InputFrameState::Shutdown() -> void {
    m_Pending.clear();
    m_NativeKeys.clear();
    m_Frame           = {};
    m_Frame.IsFocused = false;
    std::ranges::fill(m_Frame.Keyboard.States, InputState::Up);
    std::ranges::fill(m_Frame.Mouse.ButtonStates, InputState::Up);
}

InputCommandProvider::~InputCommandProvider() {
    Unbind();
}

auto InputCommandProvider::Bind(CommandDispatcher& Commands) -> std::expected<void, ErrorMessage> {
    if (m_Commands)
        return std::unexpected(ErrorMessage("Window input commands already bound"));
    auto Registration =
        Commands.Register(CommandDescriptor{.Name             = "window.input.apply",
                                            .Parameters       = {{.Name = "event", .Type = CommandValueType::Json}},
                                            .Phase            = CommandPhase::Input,
                                            .MaxArgumentBytes = 2048,
                                            .Category         = CommandCategory::Input},
                          [this](CommandContext& Context, const CommandJson& Json) -> void {
                              auto Event = ParseEvent(Json);
                              if (!Event) {
                                  Context.Fail("invalid_arguments", Event.error().ToString());
                                  return;
                              }
                              if (auto Applied = m_State->Apply(*Event); !Applied)
                                  Context.Fail("input_capacity", Applied.error().ToString());
                              if (std::holds_alternative<ResetInputEvent>(*Event)) {
                                  m_IsRecoveryPending = false;
                                  m_IsResetExecuted   = true;
                              } else if (m_IsResetExecuted &&
                                         (std::holds_alternative<FocusInputEvent>(*Event) ||
                                          std::holds_alternative<CursorEnterInputEvent>(*Event)) &&
                                         m_RecoveryOutstanding > 0) {
                                  --m_RecoveryOutstanding;
                              }
                              FinishRecovery();
                          });
    if (!Registration)
        return std::unexpected(Registration.error().Append("Failed to register window input command"));
    m_Commands               = &Commands;
    m_Registration           = *Registration;
    const auto& Frame        = m_State->GetFrame();
    m_IsFocusIntended        = Frame.IsFocused;
    m_IsCursorInsideIntended = Frame.IsCursorInside;
    return {};
}

auto InputCommandProvider::Unbind() -> void {
    if (m_Commands && m_Registration)
        m_Commands->Retire(m_Registration);
    m_Commands            = nullptr;
    m_Registration        = 0;
    m_IsRecovering        = false;
    m_IsRecoveryPending   = false;
    m_IsResetExecuted     = false;
    m_RecoveryOutstanding = 0;
    m_DeferredRecovery.clear();
}

auto InputCommandProvider::UpdateIntent(const InputEvent& Event) -> void {
    if (const auto* Focus = std::get_if<FocusInputEvent>(&Event))
        m_IsFocusIntended = Focus->IsFocused;
    else if (const auto* Cursor = std::get_if<CursorEnterInputEvent>(&Event))
        m_IsCursorInsideIntended = Cursor->IsInside;
}

auto InputCommandProvider::Submit(InputEvent Event) -> std::expected<void, ErrorMessage> {
    if (!m_Commands)
        return std::unexpected(ErrorMessage("Window input commands are not bound"));
    if (m_IsRecovering) {
        if (!std::holds_alternative<FocusInputEvent>(Event) && !std::holds_alternative<CursorEnterInputEvent>(Event))
            return std::unexpected(ErrorMessage("Window input recovery is pending"));
        if (m_DeferredRecovery.size() == RecoveryCapacity) {
            LogError("Physical focus/cursor recovery capacity reached");
            return std::unexpected(ErrorMessage("Window recovery transition capacity reached"));
        }
        UpdateIntent(Event);
        m_DeferredRecovery.push_back(std::move(Event));
        return {};
    }
    UpdateIntent(Event);
    auto Result = m_Commands->SubmitByUser("window.input.apply", ToJson(Event));
    if (!Result) {
        LogError("Physical input admission failed: {}", Result.error().ToString());
        m_IsRecovering      = true;
        m_IsRecoveryPending = true;
        m_IsResetExecuted   = false;
        m_DeferredRecovery.emplace_back(FocusInputEvent{.IsFocused = m_IsFocusIntended});
        m_DeferredRecovery.emplace_back(CursorEnterInputEvent{.IsInside = m_IsCursorInsideIntended});
        SubmitRecovery();
        return std::unexpected(std::move(Result).error());
    }
    return {};
}

auto InputCommandProvider::RetryRecovery() -> void {
    if (m_IsRecoveryPending)
        SubmitRecovery();
    if (m_IsResetExecuted)
        FlushRecovery();
}

auto InputCommandProvider::SubmitRecovery() -> void {
    if (!m_Commands || !m_IsRecoveryPending)
        return;
    auto Result = m_Commands->SubmitByUser(
        "window.input.apply", ToJson(InputEvent{ResetInputEvent{}}), CommandOptions{.IsRecovery = true});
    if (Result)
        m_IsRecoveryPending = false;
}

auto InputCommandProvider::FlushRecovery() -> void {
    while (m_Commands && m_IsRecovering && !m_DeferredRecovery.empty()) {
        auto Result = m_Commands->SubmitByUser(
            "window.input.apply", ToJson(m_DeferredRecovery.front()), CommandOptions{.IsRecovery = true});
        if (!Result) {
            LogError("Physical focus/cursor recovery admission failed: {}", Result.error().ToString());
            return;
        }
        m_DeferredRecovery.pop_front();
        ++m_RecoveryOutstanding;
    }
}

auto InputCommandProvider::FinishRecovery() -> void {
    if (!m_IsRecovering || !m_IsResetExecuted || !m_DeferredRecovery.empty() || m_RecoveryOutstanding != 0)
        return;
    m_IsRecovering    = false;
    m_IsResetExecuted = false;
}

} // namespace SoulEngine
