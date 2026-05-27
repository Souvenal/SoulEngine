/// @file   Core/Util/Factory.cppm
/// @brief  Type-safe object factory with automatic registration.
///
/// Factory<Interface, CtorArgs...> creates named instances of Interface
/// subclasses.  It inherits Singleton so that AutoRegistrar can safely
/// register implementations from any translation unit without static
/// initialization order fiasco — the singleton ensures the registry map
/// is constructed on first use, before any registrar touches it.

export module Core:Util.Factory;

import std;
import :Util.Types;
import :Util.Singleton;

export namespace SoulEngine::Core {

/// @brief Type-safe factory that creates named instances of Interface
///        subclasses, forwarding CtorArgs to their constructors.
///
/// Usage:
///   // In a header or anywhere the factory is visible:
///   using MyFactory = Factory<IMyInterface, int, float>;
///   MyFactory::AutoRegistrar<MyImpl> Reg{"my_impl"};
///
///   // Anywhere:
///   auto Obj = MyFactory::Get().Create("my_impl", 42, 3.14f);
template <typename Interface, typename... CtorArgs>
class Factory : public Singleton<Factory<Interface, CtorArgs...>> {
    friend class Singleton<Factory<Interface, CtorArgs...>>;

    Factory()  = default;
    ~Factory() = default;

  public:
    /// @brief Function signature for creating an Interface instance.
    using Creator = std::function<UPtr<Interface>(CtorArgs...)>;

    /// @brief Register an implementation under the given name.
    /// @tparam Impl  Concrete type derived from Interface.
    /// @param  name  Unique name for this implementation.
    /// @return Reference to this factory, for chaining.
    template <typename Impl>
    auto Register(StringView Name) -> Factory& {
        static_assert(std::is_base_of_v<Interface, Impl>);
        m_Registry[String(Name)] = [](CtorArgs... args) -> UPtr<Interface> {
            return std::make_unique<Impl>(std::forward<CtorArgs>(args)...);
        };
        return *this;
    }

    /// @brief Create an instance by registered name.
    /// @param name  Name previously passed to Register().
    /// @param args  Arguments forwarded to the implementation's constructor.
    /// @return Unique pointer to the new instance, or nullptr if `name`
    ///         was not registered.
    [[nodiscard]] auto Create(StringView Name, CtorArgs... args) const -> UPtr<Interface> {
        auto It = m_Registry.find(String(Name));
        return (It != m_Registry.end()) ? It->second(std::forward<CtorArgs>(args)...) : nullptr;
    }

    /// @brief Check whether a name has been registered.
    [[nodiscard]] auto Contains(StringView Name) const -> bool {
        return m_Registry.contains(String(Name));
    }

    /// @brief Return all registered implementation names.
    /// The returned StringViews point into the factory's owned keys,
    /// which live as long as the factory itself (forever, since
    /// Factory is a singleton).
    [[nodiscard]] auto Keys() const -> std::vector<StringView> {
        std::vector<StringView> Result;
        Result.reserve(m_Registry.size());
        for (auto const& [Key, _] : m_Registry)
            Result.push_back(Key);
        return Result;
    }

    /// @brief RAII helper that auto-registers an implementation on
    ///        construction.  Safe to instantiate at global/namespace scope
    ///        because Factory inherits Singleton — the registry map is
    ///        guaranteed to exist when this constructor runs.
    template <typename Impl>
    struct AutoRegistrar {
        /// @brief Construct and register @p Impl under @p name.
        explicit AutoRegistrar(StringView Name) {
            Factory::Get().template Register<Impl>(Name);
        }
    };

  private:
    std::flat_map<String, Creator> m_Registry;
};

} // namespace SoulEngine::Core
