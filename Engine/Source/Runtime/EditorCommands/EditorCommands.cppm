module;
#include <nlohmann/json.hpp>
export module EditorCommands;
import std;
import Core;
namespace SoulEngine {
export using CommandJson = nlohmann::json;
export enum class CommandPhase { Unknown = 0, Input, Action, PostUpdate };
export enum class CommandValueType { Unknown = 0, Boolean, Integer, Number, String, Json };
export struct CommandParameter {
    String                     Name;
    CommandValueType           Type = CommandValueType::Unknown;
    std::optional<long double> Minimum;
    std::optional<long double> Maximum;
    std::optional<CommandJson> Default;
    bool                       IsUnsigned      = false;
    Int64                      SignedMinimum   = std::numeric_limits<Int64>::min();
    Int64                      SignedMaximum   = std::numeric_limits<Int64>::max();
    Uint64                     UnsignedMaximum = std::numeric_limits<Uint64>::max();
};
export enum class CommandCategory { Unknown = 0, Observe, Input, Edit, Persist, Destructive };
export struct CommandDescriptor {
    String                        Name;
    std::vector<CommandParameter> Parameters;
    bool                          IsAgentExposed   = false;
    CommandPhase                  Phase            = CommandPhase::Action;
    std::size_t                   MaxArgumentBytes = 16384;
    std::size_t                   MaxResultBytes   = 65536;
    String                        Description;
    Uint32                        Version = 1;
    CommandJson                   Schema;
    CommandCategory               Category = CommandCategory::Observe;
};
export enum class CommandStatus { Unknown = 0, Succeeded, Failed, Cancelled };
export struct CommandFailure {
    String      Code;
    String      Message;
    CommandJson Details;
};
export struct CommandResult {
    String                        Command;
    Uint64                        Invocation = 0;
    std::optional<Uint64>         Frame;
    CommandStatus                 Status = CommandStatus::Unknown;
    CommandJson                   Output;
    std::optional<CommandFailure> Failure;
};
export struct CommandOptions {
    std::optional<Uint64> Deadline;
};
/// @brief Owning terminal sink; delivery runs outside dispatcher locks and must complete without blocking.
export using CommandCompletion = std::function<void(CommandResult)>;
export class CommandContext {
  public:
    auto SetOutput(CommandJson Output) -> void {
        m_Output = std::move(Output);
    }
    auto Fail(String Code, String Message, CommandJson Details = {}) -> void {
        m_Failure =
            CommandFailure{.Code = std::move(Code), .Message = std::move(Message), .Details = std::move(Details)};
    }

  private:
    friend class CommandDispatcher;
    CommandJson                   m_Output;
    std::optional<CommandFailure> m_Failure;
};
template <class T>
constexpr auto DeducedCommandType() -> CommandValueType {
    using V = std::remove_cvref_t<T>;
    if constexpr (std::same_as<V, bool>)
        return CommandValueType::Boolean;
    else if constexpr (std::integral<V>)
        return CommandValueType::Integer;
    else if constexpr (std::floating_point<V>)
        return CommandValueType::Number;
    else if constexpr (std::same_as<V, String>)
        return CommandValueType::String;
    else
        return CommandValueType::Json;
}
template <class T>
struct CommandFunctionTraits;
template <class R, class C, class... A>
struct CommandFunctionTraits<R (C::*)(CommandContext&, A...) const> {
    using Arguments = std::tuple<std::remove_cvref_t<A>...>;
    static_assert(std::same_as<R, void>);
};
/// @brief Owns registered commands; any thread submits and cancels, while Game registers and dispatches.
export class CommandDispatcher {
  public:
    explicit CommandDispatcher(std::size_t Capacity = 256) : m_Capacity(Capacity) {}
    [[nodiscard]] auto TakeDiagnostics() -> std::vector<CommandResult>;
    [[nodiscard]] auto DescribeExposed() const -> std::vector<CommandDescriptor>;
    auto               Cancel(Uint64 InvocationId) -> void;
    auto               Shutdown() -> void;
    auto               Retire(Uint64 RegistrationId) -> void;
    [[nodiscard]] auto Register(String Name, std::function<void()> Handler) -> std::expected<Uint64, ErrorMessage>;
    template <class F>
    [[nodiscard]] auto Register(CommandDescriptor Desc, F Handler) -> std::expected<Uint64, ErrorMessage> {
        using Args = typename CommandFunctionTraits<decltype(&F::operator())>::Arguments;
        return RegisterTyped<Args>(
            std::move(Desc), std::move(Handler), std::make_index_sequence<std::tuple_size_v<Args>>{});
    }
    template <class... A>
    [[nodiscard]] auto SubmitByUser(StringView Name, A&&... Arguments) -> std::expected<Uint64, ErrorMessage> {
        return Submit(Name, CommandJson::array({CommandJson(std::forward<A>(Arguments))...}), false);
    }
    [[nodiscard]] auto SubmitByAgent(StringView         Name,
                                     const CommandJson& Arguments,
                                     CommandCompletion  Completion = {},
                                     CommandOptions     Options    = {}) -> std::expected<Uint64, ErrorMessage>;
    /// @brief Executes one bounded frozen batch per phase/frame on Game; Now is monotonic milliseconds.
    auto               ExecutePhase(CommandPhase Phase, Uint64 Frame, std::size_t Budget = 64, Uint64 Now = 0) -> void;

  private:
    template <class V>
    static auto SetParameterType(CommandParameter& Parameter) -> void {
        Parameter.Type = DeducedCommandType<V>();
        if constexpr (std::integral<V> && !std::same_as<V, bool>) {
            Parameter.IsUnsigned = std::is_unsigned_v<V>;
            if constexpr (std::is_unsigned_v<V>)
                Parameter.UnsignedMaximum = std::numeric_limits<V>::max();
            else {
                Parameter.SignedMinimum = std::numeric_limits<V>::min();
                Parameter.SignedMaximum = std::numeric_limits<V>::max();
            }
        }
        if constexpr (std::floating_point<V>) {
            Parameter.Minimum = std::max(Parameter.Minimum.value_or(std::numeric_limits<long double>::lowest()),
                                         static_cast<long double>(std::numeric_limits<V>::lowest()));
            Parameter.Maximum = std::min(Parameter.Maximum.value_or(std::numeric_limits<long double>::max()),
                                         static_cast<long double>(std::numeric_limits<V>::max()));
        }
    }
    template <class Args, class F, std::size_t... I>
    auto RegisterTyped(CommandDescriptor Desc, F Handler, std::index_sequence<I...>)
        -> std::expected<Uint64, ErrorMessage> {
        if (Desc.Parameters.size() != sizeof...(I))
            return std::unexpected(ErrorMessage("Parameter count mismatch"));
        if (((Desc.Parameters[I].Type != CommandValueType::Unknown &&
              Desc.Parameters[I].Type != DeducedCommandType<std::tuple_element_t<I, Args>>()) ||
             ...))
            return std::unexpected(ErrorMessage("Descriptor type mismatch"));
        (SetParameterType<std::tuple_element_t<I, Args>>(Desc.Parameters[I]), ...);
        return RegisterErased(
            std::move(Desc),
            [Handler = std::move(Handler)](CommandContext& Context, const CommandJson& Values) -> void {
                Handler(Context, Values[I].template get<std::tuple_element_t<I, Args>>()...);
            });
    }
    [[nodiscard]] auto RegisterErased(CommandDescriptor                                        Desc,
                                      std::function<void(CommandContext&, const CommandJson&)> Handler)
        -> std::expected<Uint64, ErrorMessage>;
    [[nodiscard]] auto Submit(StringView         Name,
                              const CommandJson& Arguments,
                              bool               IsAgent,
                              CommandCompletion  Completion = {},
                              CommandOptions     Options    = {},
                              CommandFailure*    Failure    = nullptr) -> std::expected<Uint64, ErrorMessage>;
    struct Registration {
        CommandDescriptor                                        Desc;
        std::function<void(CommandContext&, const CommandJson&)> Handler;
        Uint64                                                   Id;
        bool                                                     IsActive = true;
    };
    struct Invocation {
        SPtr<Registration> Entry;
        CommandJson        Arguments;
        Uint64             Id = 0;
        CommandCompletion  Completion;
        CommandOptions     Options;
    };
    auto                                           Deliver(Invocation& Call, CommandResult Result) -> void;
    std::unordered_map<String, SPtr<Registration>> m_Registry;
    std::vector<Invocation>                        m_Queue;
    Uint64                                         m_NextId   = 1;
    std::size_t                                    m_Capacity = 256;
    std::array<std::optional<Uint64>, 4>           m_PhaseFrames;
    std::unordered_set<Uint64>                     m_Cancelled;
    bool                                           m_IsClosed      = false;
    bool                                           m_IsDispatching = false;
    mutable std::mutex                             m_Mutex;
    std::vector<CommandResult>                     m_Diagnostics;
    std::size_t                                    m_Outstanding = 0;
    std::unordered_set<Uint64>                     m_PendingIds;
};
} // namespace SoulEngine
