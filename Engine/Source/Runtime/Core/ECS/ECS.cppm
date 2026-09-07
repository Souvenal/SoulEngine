module;

#include <entt/entt.hpp>

export module Core:ECS;

export import :Util;

export namespace SoulEngine {

/// @brief Add or replace a reflected component on an entity.
/// @tparam T Component type.
/// @param Registry Registry that owns the component storage.
/// @param Entity Entity that receives the component.
/// @param Component Constructed component value.
template <typename T>
auto EmplaceComponent(entt::registry* Registry, entt::entity Entity, const T& Component) -> void {
    Registry->emplace_or_replace<T>(Entity, Component);
}

/// @brief Base interface for all ECS systems.
///
/// Systems process entities with specific components each frame.
class ISystem {
  public:
    explicit ISystem(entt::registry& Registry) : m_Registry(Registry) {}
    virtual ~ISystem() = default;

    /// @brief Called once per frame to update system state.
    /// @param DeltaTime Time elapsed since last frame in seconds.
    virtual auto OnUpdate(Float32 DeltaTime) -> void = 0;

    /// @brief Set up observers for component changes.
    ///
    /// Called when the system is initialized or when it needs to start observing.
    /// Override this to connect entt::observers to specific component types.
    virtual auto SetupObservers() -> void {}

    /// @brief Tear down observers.
    ///
    /// Called when the system is being destroyed or needs to stop observing.
    /// Override this to disconnect entt::observers.
    virtual auto TeardownObservers() -> void {}

    /// @brief Clear observer state without disconnecting.
    ///
    /// Called to reset observer state, typically after processing observed changes.
    /// Override this to clear observer state owned by the registry.
    virtual auto ClearObservers() -> void {}

  protected:
    entt::registry& m_Registry;
};

/// @brief Owns and dispatches the systems registered for an ECS world.
///
/// The scheduler is bound to the registry owned by its scene.
///
/// Every system is registered under a unique name and declares its execution
/// dependencies with Before/After name lists. CompileDependency() resolves
/// the declared dependency DAG into a deterministic execution order.
/// Registration and CompileDependency() are only allowed during the
/// initialization phase; OnUpdate() requires a successful CompileDependency()
/// first.
class SystemScheduler {
  public:
    explicit SystemScheduler(entt::registry& Registry) : m_Registry(Registry) {}
    ~SystemScheduler()                                 = default;
    SystemScheduler(const SystemScheduler&)            = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;
    SystemScheduler(SystemScheduler&&)                 = delete;
    SystemScheduler& operator=(SystemScheduler&&)      = delete;

    /// @brief Register one system type under a unique name.
    /// @tparam T System implementation derived from ISystem.
    /// @param Name Unique system name that other systems reference in their
    ///        Before/After dependency lists.
    /// @param Before Names of systems that must run after this system.
    /// @param After Names of systems that must run before this system.
    /// @param Arguments Arguments forwarded to T's constructor.
    /// @return Success, or an error when registration is already closed, the
    ///         name is taken, or T is already registered.
    template <typename T, typename... Args>
        requires std::derived_from<T, ISystem>
    [[nodiscard]] auto Register(String Name, std::vector<String> Before = {}, std::vector<String> After = {},
                                Args&&... Arguments) -> std::expected<void, ErrorMessage> {
        if (m_Compiled)
            return std::unexpected(ErrorMessage("System registration is only allowed before CompileDependency()"));

        const auto Type = entt::type_index<T>::value();
        if (const auto It = m_TypeToIndex.find(Type); It != m_TypeToIndex.end())
            return std::unexpected(
                ErrorMessage(Format("System type is already registered as '{}'", m_Entries[It->second].Name)));
        if (m_NameToIndex.contains(Name))
            return std::unexpected(ErrorMessage(Format("Duplicate system name '{}'", Name)));

        const auto Index = static_cast<Uint32>(m_Entries.size());
        m_Entries.push_back(SystemEntry{
            .Name   = std::move(Name),
            .System = std::make_unique<T>(m_Registry, std::forward<Args>(Arguments)...),
            .Before = std::move(Before),
            .After  = std::move(After),
        });
        m_TypeToIndex.emplace(Type, Index);
        m_NameToIndex.emplace(m_Entries[Index].Name, Index);
        return {};
    }

    /// @brief Resolve the declared Before/After dependencies into a
    ///        deterministic execution order.
    ///
    /// "A before B" and "B after A" both become the edge A -> B. Kahn's
    /// algorithm breaks ties by registration order, so identical
    /// registrations always produce identical execution orders.
    /// @return Success, or an error listing every unknown dependency
    ///         reference, or describing one concrete dependency cycle.
    [[nodiscard]] auto CompileDependency() -> std::expected<void, ErrorMessage> {
        if (m_Compiled)
            return std::unexpected(ErrorMessage("CompileDependency() has already been called"));

        const auto Count = static_cast<Uint32>(m_Entries.size());

        // Build and deduplicate edges, collecting every dangling reference
        // before failing so one run reports all of them.
        std::flat_set<std::pair<Uint32, Uint32>> Edges;
        String                                   Dangling;
        for (Uint32 I = 0; I < Count; ++I) {
            const auto& Entry = m_Entries[I];
            for (const auto& Target : Entry.Before) {
                if (const auto It = m_NameToIndex.find(Target); It != m_NameToIndex.end())
                    Edges.emplace(I, It->second);
                else
                    Dangling += Format("\n  '{}' declares Before '{}', which is not registered", Entry.Name, Target);
            }
            for (const auto& Target : Entry.After) {
                if (const auto It = m_NameToIndex.find(Target); It != m_NameToIndex.end())
                    Edges.emplace(It->second, I);
                else
                    Dangling += Format("\n  '{}' declares After '{}', which is not registered", Entry.Name, Target);
            }
        }
        if (!Dangling.empty())
            return std::unexpected(ErrorMessage("Unknown system dependency references:" + Dangling));

        std::vector<std::vector<Uint32>> Outgoing(Count);
        std::vector<std::vector<Uint32>> Incoming(Count);
        std::vector<Uint32>              Indegree(Count, 0);
        for (const auto& [From, To] : Edges) {
            Outgoing[From].push_back(To);
            Incoming[To].push_back(From);
            ++Indegree[To];
        }

        // The min-heap on registration index keeps the order deterministic
        // across platforms and runs.
        std::priority_queue<Uint32, std::vector<Uint32>, std::greater<Uint32>> Ready;
        for (Uint32 I = 0; I < Count; ++I)
            if (Indegree[I] == 0)
                Ready.push(I);

        std::vector<Uint32> Order;
        Order.reserve(Count);
        while (!Ready.empty()) {
            const Uint32 I = Ready.top();
            Ready.pop();
            Order.push_back(I);
            for (const Uint32 To : Outgoing[I])
                if (--Indegree[To] == 0)
                    Ready.push(To);
        }

        if (Order.size() != Count) {
            // Every still-blocked node has at least one blocked predecessor,
            // so walking backwards from any of them must revisit a node; that
            // segment is one concrete cycle.
            const auto DescribeCycle = [&]() -> String {
                Uint32 Start = 0;
                while (Indegree[Start] == 0)
                    ++Start;
                std::vector<Uint32> Path;
                Uint32              Current = Start;
                while (std::ranges::find(Path, Current) == Path.end()) {
                    Path.push_back(Current);
                    for (const Uint32 Predecessor : Incoming[Current])
                        if (Indegree[Predecessor] > 0) {
                            Current = Predecessor;
                            break;
                        }
                }
                String     Cycle;
                const auto Begin = std::ranges::find(Path, Current);
                for (auto It = Begin; It != Path.end(); ++It)
                    Cycle += m_Entries[*It].Name + " -> ";
                return Cycle + m_Entries[Current].Name;
            };
            return std::unexpected(ErrorMessage(Format("System dependency cycle detected: {}", DescribeCycle())));
        }

        m_Order.clear();
        m_Order.reserve(Count);
        for (const Uint32 I : Order)
            m_Order.push_back(m_Entries[I].System.get());
        m_Compiled = true;
        return {};
    }

    /// @brief Get a registered system by its concrete type.
    /// @tparam T System implementation derived from ISystem.
    /// @return A borrowed system reference, or nullopt if T is not registered.
    template <typename T>
        requires std::derived_from<T, ISystem>
    [[nodiscard]] auto Get() -> std::optional<std::reference_wrapper<T>> {
        const auto Type = entt::type_index<T>::value();
        const auto It   = m_TypeToIndex.find(Type);
        if (It == m_TypeToIndex.end())
            return std::nullopt;
        return std::ref(*static_cast<T*>(m_Entries[It->second].System.get()));
    }

    /// @brief Get a registered system by its concrete type for read-only use.
    /// @tparam T System implementation derived from ISystem.
    /// @return A borrowed const system reference, or nullopt if T is not registered.
    template <typename T>
        requires std::derived_from<T, ISystem>
    [[nodiscard]] auto Get() const -> std::optional<std::reference_wrapper<const T>> {
        const auto Type = entt::type_index<T>::value();
        const auto It   = m_TypeToIndex.find(Type);
        if (It == m_TypeToIndex.end())
            return std::nullopt;
        return std::cref(*static_cast<const T*>(m_Entries[It->second].System.get()));
    }

    /// @brief Remove a registered system by its concrete type.
    ///
    /// Allowed after CompileDependency(): the entry becomes a tombstone so
    /// registration indices stay stable, and the system is erased from the
    /// compiled execution order.
    /// @tparam T System implementation derived from ISystem.
    /// @return true if T was registered and removed.
    template <typename T>
        requires std::derived_from<T, ISystem>
    [[nodiscard]] auto Remove() -> bool {
        const auto Type = entt::type_index<T>::value();
        const auto It   = m_TypeToIndex.find(Type);
        if (It == m_TypeToIndex.end())
            return false;

        auto& Entry = m_Entries[It->second];
        Entry.System->TeardownObservers();
        std::erase(m_Order, Entry.System.get());
        Entry.System = nullptr;
        m_NameToIndex.erase(Entry.Name);
        m_TypeToIndex.erase(It);
        return true;
    }

    /// @brief Call OnUpdate on every registered system in compiled
    ///        dependency order.
    /// @param DeltaTime Time elapsed since last frame in seconds.
    /// @return Success, or an error when CompileDependency() has not
    ///         succeeded yet.
    [[nodiscard]] auto OnUpdate(Float32 DeltaTime) -> std::expected<void, ErrorMessage> {
        if (!m_Compiled)
            return std::unexpected(ErrorMessage("CompileDependency() must succeed before OnUpdate()"));
        for (auto* System : m_Order)
            System->OnUpdate(DeltaTime);
        return {};
    }

    /// @brief Set up observers for every registered system.
    ///
    /// Observer wiring has no declared ordering semantics; systems are
    /// visited in registration order.
    auto SetupObservers() -> void {
        for (auto& Entry : m_Entries)
            if (Entry.System)
                Entry.System->SetupObservers();
    }

    /// @brief Tear down observers for every registered system.
    auto TeardownObservers() -> void {
        for (auto& Entry : m_Entries)
            if (Entry.System)
                Entry.System->TeardownObservers();
    }

    /// @brief Clear observers for every registered system.
    auto ClearObservers() -> void {
        for (auto& Entry : m_Entries)
            if (Entry.System)
                Entry.System->ClearObservers();
    }

  private:
    /// @brief One registered system. Removed systems leave a tombstone
    ///        (null System) so entry indices remain stable.
    struct SystemEntry {
        String              Name    = {};
        UPtr<ISystem>       System  = nullptr;
        std::vector<String> Before  = {};
        std::vector<String> After   = {};
    };

    entt::registry&                       m_Registry;
    std::vector<SystemEntry>              m_Entries     = {}; // registration order
    std::flat_map<entt::id_type, Uint32>  m_TypeToIndex = {};
    std::flat_map<String, Uint32>         m_NameToIndex = {};
    std::vector<ISystem*>                 m_Order       = {}; // compiled execution order (non-owning)
    bool                                  m_Compiled    = false;
};

/// @brief Name component for entities.
struct NameComponent {
    String Name = {};
};

/// @brief Parent component for entity hierarchy.
///
/// Stores the parent entity reference.
struct ParentComponent {
    explicit ParentComponent(entt::entity InParent) : Parent(InParent) {}

    entt::entity Parent = entt::null;
    Uint32       Depth  = 0;

    /// @brief Initialize hierarchy depth after construction in a registry.
    static auto on_construct(entt::registry& Registry, entt::entity Entity) -> void {
        auto&      Component   = Registry.get<ParentComponent>(Entity);
        const auto ParentDepth = Registry.all_of<ParentComponent>(Component.Parent)
                                     ? Registry.get<ParentComponent>(Component.Parent).Depth
                                     : 0;
        Component.Depth        = ParentDepth + 1;
    }
};

/// @brief Children component for entity hierarchy.
///
/// Stores a list of child entities. This is optional; not all entities with
/// children need this component if they don't need to iterate children.
struct ChildrenComponent {
    std::vector<entt::entity> Children = {};
};

} // namespace SoulEngine
