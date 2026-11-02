module;
#include <nlohmann/json.hpp>
module EditorCommands;
namespace SoulEngine {
[[nodiscard]] auto IsValidParameter(const CommandParameter& Parameter, const CommandJson& Value) -> bool {
    const auto Type = Parameter.Type;
    if (Type == CommandValueType::Integer) {
        if (!Value.is_number_integer())
            return false;
        if (Value.is_number_unsigned()) {
            const auto Number = Value.get<Uint64>();
            if (Parameter.IsUnsigned) {
                if (Number > Parameter.UnsignedMaximum)
                    return false;
            } else if (Number > static_cast<Uint64>(Parameter.SignedMaximum) ||
                       (Parameter.SignedMinimum > 0 && Number < static_cast<Uint64>(Parameter.SignedMinimum)))
                return false;
            if (Parameter.Minimum && *Parameter.Minimum > 0) {
                const auto Bound = std::ceil(*Parameter.Minimum);
                if (Bound >= std::ldexp(1.0L, 64) || Number < static_cast<Uint64>(Bound))
                    return false;
            }
            if (Parameter.Maximum) {
                const auto Bound = std::floor(*Parameter.Maximum);
                if (Bound < 0 || (Bound < std::ldexp(1.0L, 64) && Number > static_cast<Uint64>(Bound)))
                    return false;
            }
        } else {
            const auto Number = Value.get<Int64>();
            if (Parameter.IsUnsigned) {
                if (Number < 0 || static_cast<Uint64>(Number) > Parameter.UnsignedMaximum)
                    return false;
            } else if (Number < Parameter.SignedMinimum || Number > Parameter.SignedMaximum)
                return false;
            if (Parameter.Minimum) {
                const auto Bound = std::ceil(*Parameter.Minimum);
                if (Bound >= std::ldexp(1.0L, 63) ||
                    (Bound >= -std::ldexp(1.0L, 63) && Number < static_cast<Int64>(Bound)))
                    return false;
            }
            if (Parameter.Maximum) {
                const auto Bound = std::floor(*Parameter.Maximum);
                if (Bound < -std::ldexp(1.0L, 63) ||
                    (Bound < std::ldexp(1.0L, 63) && Number > static_cast<Int64>(Bound)))
                    return false;
            }
        }
        return true;
    }
    if (Type == CommandValueType::Json)
        return true;
    if (Type == CommandValueType::String)
        return Value.is_string();
    if (Type == CommandValueType::Boolean)
        return Value.is_boolean();
    if (Type != CommandValueType::Number || !Value.is_number())
        return false;
    const auto Number = Value.get<long double>();
    return std::isfinite(Number) && (!Parameter.Minimum || Number >= *Parameter.Minimum) &&
           (!Parameter.Maximum || Number <= *Parameter.Maximum);
}

[[nodiscard]] auto TerminalSize(const CommandResult& R) -> std::size_t {
    CommandJson Failure = nullptr;
    if (R.Failure)
        Failure =
            CommandJson{{"code", R.Failure->Code}, {"message", R.Failure->Message}, {"details", R.Failure->Details}};
    return CommandJson{{"command", R.Command},
                       {"invocation", R.Invocation},
                       {"frame", R.Frame ? CommandJson(*R.Frame) : CommandJson(nullptr)},
                       {"status", static_cast<int>(R.Status)},
                       {"output", R.Output},
                       {"failure", std::move(Failure)}}
        .dump(-1, ' ', false, CommandJson::error_handler_t::replace)
        .size();
}
auto CommandDispatcher::Deliver(Invocation& Call, CommandResult Result) -> void {
    if (TerminalSize(Result) > Call.Entry->Desc.MaxResultBytes) {
        Result.Output  = nullptr;
        Result.Status  = CommandStatus::Failed;
        Result.Failure = CommandFailure{.Code = "result_budget", .Message = "Result budget exceeded"};
    }
    if (Call.Completion)
        Call.Completion(std::move(Result));
    else if (Result.Failure) {
        std::scoped_lock Lock(m_Mutex);
        if (m_Diagnostics.size() == 64)
            m_Diagnostics.erase(m_Diagnostics.begin());
        m_Diagnostics.push_back(std::move(Result));
    }
}
auto CommandDispatcher::TakeDiagnostics() -> std::vector<CommandResult> {
    std::scoped_lock Lock(m_Mutex);
    auto             Result = std::move(m_Diagnostics);
    m_Diagnostics.clear();
    return Result;
}
auto CommandDispatcher::DescribeExposed() const -> std::vector<CommandDescriptor> {
    std::scoped_lock               Lock(m_Mutex);
    std::vector<CommandDescriptor> Result;
    for (const auto& [Name, Entry] : m_Registry)
        if (Entry->Desc.IsAgentExposed &&
            (Entry->Desc.Category == CommandCategory::Observe || Entry->Desc.Category == CommandCategory::Input))
            Result.push_back(Entry->Desc);
    std::ranges::sort(Result, {}, &CommandDescriptor::Name);
    return Result;
}
auto CommandDispatcher::Cancel(Uint64 InvocationId) -> void {
    std::scoped_lock Lock(m_Mutex);
    if (m_PendingIds.contains(InvocationId))
        m_Cancelled.insert(InvocationId);
}
auto CommandDispatcher::Shutdown() -> void {
    std::unique_lock Lock(m_Mutex);
    m_IsClosed   = true;
    auto Pending = std::move(m_Queue);
    m_Queue.clear();
    m_Outstanding -= Pending.size();
    for (const auto& Call : Pending) {
        m_PendingIds.erase(Call.Id);
        m_Cancelled.erase(Call.Id);
    }
    for (auto& [Name, Entry] : m_Registry)
        Entry->IsActive = false;
    m_Registry.clear();
    Lock.unlock();
    for (auto& Call : Pending)
        Deliver(Call,
                CommandResult{.Command    = Call.Entry->Desc.Name,
                              .Invocation = Call.Id,
                              .Status     = CommandStatus::Cancelled,
                              .Failure = CommandFailure{.Code = "shutdown", .Message = "Command dispatcher stopped"}});
}
auto CommandDispatcher::Retire(Uint64 RegistrationId) -> void {
    std::unique_lock Lock(m_Mutex);
    for (auto It = m_Registry.begin(); It != m_Registry.end(); ++It) {
        if (It->second->Id == RegistrationId) {
            It->second->IsActive = false;
            m_Registry.erase(It);
            break;
        }
    }
    std::vector<Invocation> Retired;
    for (auto It = m_Queue.begin(); It != m_Queue.end();) {
        if (!It->Entry->IsActive) {
            Retired.push_back(std::move(*It));
            It = m_Queue.erase(It);
        } else
            ++It;
    }
    m_Outstanding -= Retired.size();
    for (const auto& Call : Retired) {
        m_PendingIds.erase(Call.Id);
        m_Cancelled.erase(Call.Id);
    }
    Lock.unlock();
    for (auto& Call : Retired)
        Deliver(Call,
                CommandResult{.Command    = Call.Entry->Desc.Name,
                              .Invocation = Call.Id,
                              .Status     = CommandStatus::Cancelled,
                              .Failure =
                                  CommandFailure{.Code = "provider_retired", .Message = "Command provider retired"}});
}
auto CommandDispatcher::Register(String Name, std::function<void()> Handler) -> std::expected<Uint64, ErrorMessage> {
    return RegisterErased(CommandDescriptor{.Name = std::move(Name)},
                          [Handler = std::move(Handler)](CommandContext&, const CommandJson&) -> void { Handler(); });
}
auto CommandDispatcher::RegisterErased(CommandDescriptor                                        Desc,
                                       std::function<void(CommandContext&, const CommandJson&)> Handler)
    -> std::expected<Uint64, ErrorMessage> {
    std::scoped_lock Lock(m_Mutex);
    if (m_IsClosed)
        return std::unexpected(ErrorMessage("Dispatcher closed"));
    if (Desc.Phase != CommandPhase::Input && Desc.Phase != CommandPhase::Action &&
        Desc.Phase != CommandPhase::PostUpdate)
        return std::unexpected(ErrorMessage("Invalid command phase"));
    if (Desc.Name.empty() || m_Registry.contains(Desc.Name))
        return std::unexpected(ErrorMessage("Duplicate or empty command name"));
    CommandResult MinimumResult{.Command    = Desc.Name,
                                .Invocation = std::numeric_limits<Uint64>::max(),
                                .Frame      = std::numeric_limits<Uint64>::max(),
                                .Status     = CommandStatus::Failed,
                                .Failure =
                                    CommandFailure{.Code = "result_budget", .Message = "Result budget exceeded"}};
    if (Desc.MaxResultBytes < TerminalSize(MinimumResult))
        return std::unexpected(ErrorMessage("Result budget cannot hold terminal metadata"));
    Desc.Schema = CommandJson{{"type", "object"},
                              {"properties", CommandJson::object()},
                              {"required", CommandJson::array()},
                              {"additionalProperties", false}};
    std::unordered_set<String> Names;
    for (const auto& Param : Desc.Parameters) {
        if (Param.Minimum && Param.Maximum && *Param.Minimum > *Param.Maximum)
            return std::unexpected(ErrorMessage("Invalid parameter bounds"));
        if (Param.Default && !IsValidParameter(Param, *Param.Default))
            return std::unexpected(ErrorMessage("Invalid parameter default"));
        if (Param.Name.empty() || !Names.insert(Param.Name).second)
            return std::unexpected(ErrorMessage("Duplicate or empty parameter name"));
        const auto Type   = Param.Type == CommandValueType::String    ? "string"
                            : Param.Type == CommandValueType::Boolean ? "boolean"
                            : Param.Type == CommandValueType::Integer ? "integer"
                            : Param.Type == CommandValueType::Number  ? "number"
                                                                      : "object";
        auto&      Schema = Desc.Schema["properties"][Param.Name];
        Schema            = Param.Type == CommandValueType::Json ? CommandJson::object() : CommandJson{{"type", Type}};
        if (Param.Minimum)
            Schema["minimum"] = static_cast<double>(*Param.Minimum);
        if (Param.Maximum)
            Schema["maximum"] = static_cast<double>(*Param.Maximum);
        if (Param.Default)
            Schema["default"] = *Param.Default;
        else
            Desc.Schema["required"].push_back(Param.Name);
    }
    auto Entry = std::make_shared<Registration>(
        Registration{.Desc = std::move(Desc), .Handler = std::move(Handler), .Id = m_NextId++});
    m_Registry.emplace(Entry->Desc.Name, Entry);
    return Entry->Id;
}
auto CommandDispatcher::SubmitByAgent(StringView         Name,
                                      const CommandJson& Arguments,
                                      CommandCompletion  Completion,
                                      CommandOptions     Options) -> std::expected<Uint64, ErrorMessage> {
    CommandFailure Failure;
    auto           Submitted = Submit(Name, Arguments, true, Completion, Options, &Failure);
    if (!Submitted && Completion) {
        Uint64 Id;
        {
            std::scoped_lock Lock(m_Mutex);
            Id = m_NextId++;
        }
        Completion(CommandResult{
            .Command = String(Name), .Invocation = Id, .Status = CommandStatus::Failed, .Failure = std::move(Failure)});
    }
    return Submitted;
}
auto CommandDispatcher::Submit(StringView         Name,
                               const CommandJson& Arguments,
                               bool               IsAgent,
                               CommandCompletion  Completion,
                               CommandOptions     Options,
                               CommandFailure*    Failure) -> std::expected<Uint64, ErrorMessage> {
    auto Reject = [&](String Code, String Message) -> std::expected<Uint64, ErrorMessage> {
        if (Failure)
            *Failure = CommandFailure{.Code = std::move(Code), .Message = Message};
        return std::unexpected(ErrorMessage(std::move(Message)));
    };
    std::scoped_lock Lock(m_Mutex);
    if (m_IsClosed)
        return Reject("shutdown", "Dispatcher closed");
    auto Found = m_Registry.find(String(Name));
    if (Found == m_Registry.end())
        return Reject("unknown_command", "Unknown command");
    auto Entry = Found->second;
    if (IsAgent && (!Entry->Desc.IsAgentExposed || (Entry->Desc.Category != CommandCategory::Observe &&
                                                    Entry->Desc.Category != CommandCategory::Input)))
        return Reject("denied_exposure", "Command is internal");
    CommandJson Values = CommandJson::array();
    if (IsAgent) {
        if (!Arguments.is_object() || Arguments.size() > Entry->Desc.Parameters.size())
            return Reject("invalid_arguments", "Named argument count mismatch");
        for (const auto& Param : Entry->Desc.Parameters) {
            auto It = Arguments.find(Param.Name);
            if (It == Arguments.end()) {
                if (!Param.Default)
                    return Reject("invalid_arguments", "Missing argument");
                Values.push_back(*Param.Default);
            } else
                Values.push_back(*It);
        }
        for (auto It = Arguments.begin(); It != Arguments.end(); ++It)
            if (std::ranges::none_of(Entry->Desc.Parameters, [&](const auto& P) -> bool { return P.Name == It.key(); }))
                return Reject("invalid_arguments", "Unknown argument");
    } else {
        Values = Arguments;
        for (auto I = Values.size(); I < Entry->Desc.Parameters.size(); ++I) {
            if (!Entry->Desc.Parameters[I].Default)
                break;
            Values.push_back(*Entry->Desc.Parameters[I].Default);
        }
    }
    if (Values.size() != Entry->Desc.Parameters.size())
        return Reject("invalid_arguments", "Argument count mismatch");
    for (std::size_t I = 0; I < Values.size(); ++I)
        if (!IsValidParameter(Entry->Desc.Parameters[I], Values[I]))
            return Reject("invalid_arguments", "Argument type or range mismatch");
    if (Values.dump(-1, ' ', false, CommandJson::error_handler_t::replace).size() > Entry->Desc.MaxArgumentBytes)
        return Reject("invalid_arguments", "Argument budget exceeded");
    if (m_Outstanding >= m_Capacity)
        return Reject("queue_capacity", "Queue capacity reached");
    const auto Id = m_NextId++;
    ++m_Outstanding;
    m_PendingIds.insert(Id);
    m_Queue.push_back(Invocation{.Entry      = Entry,
                                 .Arguments  = std::move(Values),
                                 .Id         = Id,
                                 .Completion = std::move(Completion),
                                 .Options    = Options});
    return Id;
}
auto CommandDispatcher::ExecutePhase(CommandPhase Phase, Uint64 Frame, std::size_t Budget, Uint64 Now) -> void {
    std::unique_lock Lock(m_Mutex);
    if (m_IsDispatching)
        return;
    const auto Index = static_cast<std::size_t>(Phase);
    if (Index == 0 || Index >= m_PhaseFrames.size() || (m_PhaseFrames[Index] && Frame <= *m_PhaseFrames[Index]))
        return;
    m_PhaseFrames[Index] = Frame;
    m_IsDispatching      = true;
    std::vector<Invocation> Batch;
    for (auto It = m_Queue.begin(); It != m_Queue.end();) {
        if (It->Entry->Desc.Phase == Phase && Batch.size() < Budget) {
            Batch.push_back(std::move(*It));
            It = m_Queue.erase(It);
        } else
            ++It;
    }
    Lock.unlock();
    for (auto& Call : Batch) {
        Lock.lock();
        const String Reason = m_IsClosed                                               ? "shutdown"
                              : m_Cancelled.erase(Call.Id)                             ? "cancelled"
                              : Call.Options.Deadline && Now >= *Call.Options.Deadline ? "deadline"
                              : !Call.Entry->IsActive                                  ? "provider_retired"
                                                                                       : "";
        m_PendingIds.erase(Call.Id);
        Lock.unlock();
        if (!Reason.empty()) {
            {
                std::scoped_lock CountLock(m_Mutex);
                --m_Outstanding;
            }
            Deliver(Call,
                    CommandResult{.Command    = Call.Entry->Desc.Name,
                                  .Invocation = Call.Id,
                                  .Status     = CommandStatus::Cancelled,
                                  .Failure    = CommandFailure{.Code = Reason, .Message = "Command did not start"}});
            continue;
        }
        CommandContext Context;
        Call.Entry->Handler(Context, Call.Arguments);
        CommandResult Result{.Command    = Call.Entry->Desc.Name,
                             .Invocation = Call.Id,
                             .Frame      = Frame,
                             .Status     = Context.m_Failure ? CommandStatus::Failed : CommandStatus::Succeeded,
                             .Output     = std::move(Context.m_Output),
                             .Failure    = std::move(Context.m_Failure)};
        {
            std::scoped_lock CountLock(m_Mutex);
            --m_Outstanding;
        }
        Deliver(Call, std::move(Result));
    }
    Lock.lock();
    m_IsDispatching = false;
}
} // namespace SoulEngine
