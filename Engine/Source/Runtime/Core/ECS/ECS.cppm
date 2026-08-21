module;

#include <entt/entt.hpp>

export module Core:ECS;

export import :Util;

export namespace SoulEngine {

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
/// Invocation order is unspecified. Systems must communicate cross-cutting
/// dependencies through explicit lifecycle phases or registry state rather
/// than depending on registration or dispatch order.
class SystemScheduler {
  public:
    explicit SystemScheduler(entt::registry& Registry) : m_Registry(Registry) {}
    ~SystemScheduler()                                 = default;
    SystemScheduler(const SystemScheduler&)            = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;
    SystemScheduler(SystemScheduler&&)                 = delete;
    SystemScheduler& operator=(SystemScheduler&&)      = delete;

    /// @brief Register one system type.
    /// @tparam T System implementation derived from ISystem.
    /// @param Arguments Arguments forwarded to T's constructor.
    template <typename T, typename... Args>
        requires std::derived_from<T, ISystem>
    auto Register(Args&&... Arguments) -> void {
        const auto Type = entt::type_index<T>::value();
        if (m_Systems.contains(Type))
            return;

        auto System = std::make_unique<T>(m_Registry, std::forward<Args>(Arguments)...);
        m_Systems.emplace(Type, std::move(System));
    }

    /// @brief Get a registered system by its concrete type.
    /// @tparam T System implementation derived from ISystem.
    /// @return The system, or nullptr if T is not registered.
    template <typename T>
        requires std::derived_from<T, ISystem>
    [[nodiscard]] auto Get() const -> T* {
        const auto Type = entt::type_index<T>::value();
        const auto It   = m_Systems.find(Type);
        if (It == m_Systems.end())
            return nullptr;
        return static_cast<T*>(It->second.get());
    }

    /// @brief Remove a registered system by its concrete type.
    /// @tparam T System implementation derived from ISystem.
    /// @return true if T was registered and removed.
    template <typename T>
        requires std::derived_from<T, ISystem>
    [[nodiscard]] auto Remove() -> bool {
        const auto Type = entt::type_index<T>::value();
        const auto It   = m_Systems.find(Type);
        if (It == m_Systems.end())
            return false;

        It->second->TeardownObservers();
        m_Systems.erase(It);
        return true;
    }

    /// @brief Call OnUpdate on every registered system.
    auto OnUpdate(Float32 DeltaTime) -> void {
        for (auto&& System : m_Systems)
            System.second->OnUpdate(DeltaTime);
    }

    /// @brief Set up observers for every registered system.
    auto SetupObservers() -> void {
        for (auto&& System : m_Systems)
            System.second->SetupObservers();
    }

    /// @brief Tear down observers for every registered system.
    auto TeardownObservers() -> void {
        for (auto&& System : m_Systems)
            System.second->TeardownObservers();
    }

    /// @brief Clear observers for every registered system.
    auto ClearObservers() -> void {
        for (auto&& System : m_Systems)
            System.second->ClearObservers();
    }

  private:
    entt::registry&                                      m_Registry;
    std::flat_map<entt::id_type, UPtr<ISystem>> m_Systems = {};
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
