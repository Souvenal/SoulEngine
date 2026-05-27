export module Core:Util.Singleton;

export namespace SoulEngine::Core {
/// @brief Meyer's singleton base class.
///
/// Derive your singleton class publicly from `Singleton<Derived>`:
///
/// ```cpp
/// class A : public Singleton<Renderer>
/// {
///     friend class Singleton<A>;
/// public:
///     // ...
/// private:
///     A();
///     ~A();
/// };
/// ```
///
/// @tparam T  The derived singleton type.
///
/// ## Friend declaration
///
/// `Singleton<T>::Get()` must be able to construct `T`, but the singleton
/// class should hide its constructor/destructor from all other callers.
/// Add `friend class Singleton<T>;` inside `T` so that `Get()` can access
/// the private constructor.  Without the friendship, `Get()` cannot
/// instantiate the static local `T`.
///
/// ## Thread safety
///
/// The function-local `static T Instance` (Meyer's singleton) is
/// guaranteed to initialise exactly once in a thread-safe manner by the
/// C++ standard (since C++11).  No external synchronisation is needed.
///
/// ## Lifecycle
///
/// The instance is destroyed in reverse order of construction during
/// static deinitialisation.  Avoid accessing one singleton from the
/// destructor of another singleton that may have already been destroyed.
template <typename T>
class Singleton {
  public:
    Singleton(const Singleton&)            = delete;
    Singleton& operator=(const Singleton&) = delete;
    Singleton(Singleton&&)                 = delete;
    Singleton& operator=(Singleton&&)      = delete;

    /// Return a reference to the one-and-only instance of `T`.
    [[nodiscard]] static auto Get() -> T& {
        static T Instance;
        return Instance;
    }

  protected:
    Singleton()  = default;
    ~Singleton() = default;
};
} // namespace SoulEngine::Core
