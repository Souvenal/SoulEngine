module Core.Util.AssetManager;

namespace SoulEngine {

auto AssetManager::SetApplicationRoot(Path Root) -> void {
    std::scoped_lock Lock(m_Mutex);
    m_ApplicationRoot = std::move(Root);
}

auto AssetManager::Load(const Path& AssetPath, bool ShouldRead) -> AssetHandle {
    if (AssetPath.empty()) {
        LogWarning("AssetManager::Load: empty asset path");
        return {};
    }
    std::scoped_lock Lock(m_Mutex);
    const auto       FullPath = ResolvePath(AssetPath);
    if (const auto It = m_PathsToHandles.find(FullPath); It != m_PathsToHandles.end()) {
        if (ShouldRead)
            ReadContentLocked(m_Entries.at(It->second));
        return MakeHandle(It->second);
    }
    const auto Id       = m_NextId++;
    auto [It, Inserted] = m_Entries.try_emplace(Id, Entry{.FullPath = FullPath, .Generation = 0, .Data = nullptr});
    m_PathsToHandles.emplace(FullPath, Id);
    if (ShouldRead)
        ReadContentLocked(It->second);
    return MakeHandle(Id);
}

auto AssetManager::GetPath(AssetHandle Handle) -> Path {
    if (!Handle)
        return {};
    std::scoped_lock Lock(m_Mutex);
    const auto       It = m_Entries.find(Handle.Id);
    return It == m_Entries.end() || It->second.Generation != Handle.Generation ? Path{} : It->second.FullPath;
}

auto AssetManager::GetContent(AssetHandle Handle) -> SPtr<const std::vector<std::byte>> {
    if (!Handle)
        return nullptr;
    std::scoped_lock Lock(m_Mutex);
    const auto       It = m_Entries.find(Handle.Id);
    if (It == m_Entries.end() || It->second.Generation != Handle.Generation)
        return nullptr;
    ReadContentLocked(It->second);
    return It->second.Data;
}

auto AssetManager::MakeHandle(Uint64 Id) const -> AssetHandle {
    return AssetHandle{.Id = Id, .Generation = m_Entries.at(Id).Generation};
}

auto AssetManager::ResolvePath(const Path& AssetPath) const -> Path {
    if (AssetPath.is_absolute() || m_ApplicationRoot.empty())
        return AssetPath.lexically_normal();
    return (m_ApplicationRoot / AssetPath).lexically_normal();
}

auto AssetManager::ReadContentLocked(Entry& Entry) -> void {
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

} // namespace SoulEngine
