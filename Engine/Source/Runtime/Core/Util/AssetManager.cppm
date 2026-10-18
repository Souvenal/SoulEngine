/// @file   AssetManager.cppm
/// @brief  Global asset registry: path -> handle -> cached content.
module;

export module Core:Util.AssetManager;

import std;
export import :Util.Types;
export import :Util.Singleton;
export import :Util.Logging;

namespace SoulEngine {

/// @brief Opaque reference to a registered asset. Value type; copy freely.
///
/// `Id == 0` is the invalid sentinel: default-constructed handles and failed loads both produce it, and
/// `GetContent` returns null for it. `Generation` is reserved for future ABA detection and is always zero for
/// now.
export struct AssetHandle {
    Uint64 Id         = 0;
    Uint64 Generation = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return Id != 0;
    }
    [[nodiscard]] auto operator==(const AssetHandle&) const -> bool = default;
};

/// @brief Process-wide asset registry keyed by stable handles.
///
/// `Load` registers a path (relative paths resolve against the current application root, injected at
/// application creation) and returns a handle. Content is read lazily: `ShouldRead` only prefetches;
/// `GetContent` reads and caches on first use. Handles stay valid for the process lifetime — entries are
/// never evicted, so the handle needs no generation checks yet.
export class AssetManager : public Singleton<AssetManager> {
    friend class Singleton<AssetManager>;

  public:
    /// @brief Set the directory relative asset paths resolve against.
    auto SetApplicationRoot(Path Root) -> void;

    /// @brief Register an asset path and return its stable handle.
    ///
    /// Relative paths resolve against the application root. A path that was already registered returns the
    /// existing handle, so repeated loads of one asset share content. A missing `ShouldRead` prefetch is not
    /// an error — reading happens lazily in `GetContent`.
    [[nodiscard]] auto Load(const Path& AssetPath, bool ShouldRead = false) -> AssetHandle;

    /// @brief Return the registered full path of an asset, or empty for an
    /// invalid handle. Diagnostic use; content goes through `GetContent`.
    [[nodiscard]] auto GetPath(AssetHandle Handle) -> Path;

    /// @brief Return the asset's bytes, reading from disk on first access.
    ///
    /// Returns null for invalid handles or unreadable files. The returned pointer borrows from the manager's
    /// cache and stays valid for the process lifetime.
    [[nodiscard]] auto GetContent(AssetHandle Handle) -> SPtr<const std::vector<std::byte>>;

  private:
    AssetManager()  = default;
    ~AssetManager() = default;

    struct Entry {
        Path                         FullPath   = {};
        Uint64                       Generation = 0;
        SPtr<std::vector<std::byte>> Data       = nullptr;
    };

    [[nodiscard]] auto MakeHandle(Uint64 Id) const -> AssetHandle;

    /// @brief Resolve an asset path against the application root.
    ///
    /// Absolute paths pass through normalized; callers without a set root must pass absolute paths.
    [[nodiscard]] auto ResolvePath(const Path& AssetPath) const -> Path;

    /// @brief Read the entry's file into the cache if not cached yet.
    ///
    /// Caller holds the mutex. Read failures log a warning and leave Data null.
    auto ReadContentLocked(Entry& Entry) -> void;

    std::mutex                        m_Mutex;
    Path                              m_ApplicationRoot = {};
    std::unordered_map<Uint64, Entry> m_Entries         = {};
    std::unordered_map<Path, Uint64>  m_PathsToHandles  = {};
    Uint64                            m_NextId          = 1;
};

} // namespace SoulEngine
