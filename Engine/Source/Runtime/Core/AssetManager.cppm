/// @file   AssetManager.cppm
/// @brief  Global asset registry: path -> handle -> cached content.
module;

export module Core:AssetManager;

export import std;
export import :Util.Types;
export import :Util.Singleton;
export import :Logging;

export namespace SoulEngine {

/// @brief Opaque reference to a registered asset. Value type; copy freely.
///
/// `Id == 0` is the invalid sentinel: default-constructed handles and failed
/// loads both produce it, and `GetContent` returns null for it. `Generation`
/// is reserved for future ABA detection and is always zero for now.
struct AssetHandle {
    Uint64 Id        = 0;
    Uint64 Generation = 0;

    [[nodiscard]] explicit operator bool() const noexcept { return Id != 0; }
    [[nodiscard]] auto operator==(const AssetHandle&) const -> bool = default;
};

/// @brief Process-wide asset registry keyed by stable handles.
///
/// `Load` registers a path (relative paths resolve against the current
/// application root, injected at application creation) and returns a handle.
/// Content is read lazily: `ShouldRead` only prefetches; `GetContent` reads
/// and caches on first use. Handles stay valid for the process lifetime —
/// entries are never evicted, so the handle needs no generation checks yet.
class AssetManager : public Singleton<AssetManager> {
    friend class Singleton<AssetManager>;

  public:
    /// @brief Set the directory relative asset paths resolve against.
    auto SetApplicationRoot(Path Root) -> void {
        std::scoped_lock Lock(m_Mutex);
        m_ApplicationRoot = std::move(Root);
    }

    /// @brief Register an asset path and return its stable handle.
    ///
    /// Relative paths resolve against the application root. A path that was
    /// already registered returns the existing handle, so repeated loads of
    /// one asset share content. A missing `ShouldRead` prefetch is not an
    /// error — reading happens lazily in `GetContent`.
    [[nodiscard]] auto Load(const Path& AssetPath, bool ShouldRead = false) -> AssetHandle {
        if (AssetPath.empty()) {
            LogWarning("AssetManager::Load: empty asset path");
            return {};
        }

        std::scoped_lock Lock(m_Mutex);
        const auto FullPath = ResolvePath(AssetPath);
        if (const auto It = m_PathsToHandles.find(FullPath); It != m_PathsToHandles.end()) {
            if (ShouldRead)
                ReadContentLocked(m_Entries.at(It->second));
            return MakeHandle(It->second);
        }

        const auto Id = m_NextId++;
        auto [It, Inserted] = m_Entries.try_emplace(Id,
            Entry{.FullPath = FullPath, .Generation = 0, .Data = nullptr});
        m_PathsToHandles.emplace(FullPath, Id);
        if (ShouldRead)
            ReadContentLocked(It->second);
        return MakeHandle(Id);
    }

    /// @brief Return the registered full path of an asset, or empty for an
    /// invalid handle. Diagnostic use; content goes through `GetContent`.
    [[nodiscard]] auto GetPath(AssetHandle Handle) -> Path {
        if (!Handle)
            return {};
        std::scoped_lock Lock(m_Mutex);
        const auto It = m_Entries.find(Handle.Id);
        return It == m_Entries.end() || It->second.Generation != Handle.Generation
                   ? Path{}
                   : It->second.FullPath;
    }

    /// @brief Return the asset's bytes, reading from disk on first access.
    ///
    /// Returns null for invalid handles or unreadable files. The returned
    /// pointer borrows from the manager's cache and stays valid for the
    /// process lifetime.
    [[nodiscard]] auto GetContent(AssetHandle Handle) -> SPtr<const std::vector<std::byte>> {
        if (!Handle)
            return nullptr;

        std::scoped_lock Lock(m_Mutex);
        const auto It = m_Entries.find(Handle.Id);
        if (It == m_Entries.end() || It->second.Generation != Handle.Generation)
            return nullptr;
        ReadContentLocked(It->second);
        return It->second.Data;
    }

  private:
    AssetManager()  = default;
    ~AssetManager() = default;

    struct Entry {
        Path                        FullPath   = {};
        Uint64                      Generation = 0;
        SPtr<std::vector<std::byte>> Data       = nullptr;
    };

    [[nodiscard]] auto MakeHandle(Uint64 Id) const -> AssetHandle {
        return AssetHandle{.Id = Id, .Generation = m_Entries.at(Id).Generation};
    }

    /// Resolve an asset path against the application root. Absolute paths
    /// pass through normalized; callers without a set root must pass
    /// absolute paths.
    [[nodiscard]] auto ResolvePath(const Path& AssetPath) const -> Path {
        if (AssetPath.is_absolute() || m_ApplicationRoot.empty())
            return AssetPath.lexically_normal();
        return (m_ApplicationRoot / AssetPath).lexically_normal();
    }

    /// Read the entry's file into the cache if not cached yet. Caller holds
    /// the mutex. Read failures log a warning and leave Data null.
    auto ReadContentLocked(Entry& Entry) -> void {
        if (Entry.Data)
            return;

        std::ifstream File(Entry.FullPath, std::ios::binary | std::ios::ate);
        if (!File) {
            LogWarning("AssetManager: cannot open '{}'", Entry.FullPath.string());
            return;
        }
        const auto Size = File.tellg();
        if (Size == -1) {
            LogWarning("AssetManager: cannot determine size of '{}'", Entry.FullPath.string());
            return;
        }
        auto Data = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(Size));
        File.seekg(0);
        if (!File.read(reinterpret_cast<char*>(Data->data()), Size)) {
            LogWarning("AssetManager: failed to read '{}'", Entry.FullPath.string());
            return;
        }
        Entry.Data = std::move(Data);
    }

    std::mutex                                       m_Mutex;
    Path                                             m_ApplicationRoot = {};
    std::unordered_map<Uint64, Entry>                m_Entries         = {};
    std::unordered_map<Path, Uint64>                 m_PathsToHandles  = {};
    Uint64                                           m_NextId          = 1;
};

} // namespace SoulEngine