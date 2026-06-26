export module Resource:Context;

export import Core;
export import RHI;
export import :Types;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

template <ManagedRHIResource T>
struct ResourceRequestResult {
    ResourceHandle<T> Handle          = {};

    /// @brief True only for the caller that created a new slot and must start resource work.
    ///
    /// False means an existing slot was found. That existing slot may be ready,
    /// still loading, failed, or otherwise waiting on its current generation.
    bool ShouldStartWork = false;
};

template <ManagedRHIResource T>
struct GpuPendingResource {
    ResourceGeneration      Generation       = 0;
    String                  Key              = {};
    Resource<T>             Resource         = {};
    RHI::GpuCompletionToken UploadCompletion = {};
};

template <ManagedRHIResource T>
struct ResourceEntry {
    ResourceSlot<T>        Slot     = {};
    ResourceLifetimePolicy Policy   = ResourceTraits<T>::Info.DefaultPolicy;
    Uint32                 RefCount = 0;
};

template <ManagedRHIResource T>
using ResourceEntryMap = std::unordered_map<String, UPtr<ResourceEntry<T>>>;

/// @brief Per-resource-type entry registry.
///
/// `HasGpuPending = true` means the resource type has a GPU upload/completion
/// phase after RHI object creation, so the family owns a `GpuPending` queue.
/// Types without that phase publish Ready directly and do not carry the queue.
template <ManagedRHIResource T, bool HasGpuPending = ResourceTraits<T>::Info.HasGpuPending()>
struct ResourceFamily {
    using ResourceType = T;

    std::mutex                 Mutex;
    ResourceEntryMap<T>        Entries;
};

template <ManagedRHIResource T>
struct ResourceFamily<T, true> {
    using ResourceType = T;

    std::mutex                          Mutex;
    ResourceEntryMap<T>                 Entries;
    std::vector<GpuPendingResource<T>>  GpuPending;
};

template <typename Tuple>
struct ResourceFamilyTuple;

template <ManagedRHIResource... T>
struct ResourceFamilyTuple<std::tuple<T...>> {
    using Type = std::tuple<ResourceFamily<T>...>;
};

using ResourceFamilies = typename ResourceFamilyTuple<ManagedRHIResourceTypes>::Type;

template <ManagedRHIResource T>
struct LockedResourceEntry {
    std::unique_lock<std::mutex> Lock  = {};
    ResourceEntry<T>*            Entry = nullptr;

    [[nodiscard]] explicit operator bool() const {
        return Entry != nullptr;
    }
};

/// @brief Owns resource lifecycle state, typed entry registries, and GPU-pending queues.
class ResourceContext {
  public:
    auto Init(TaskGraph& InTaskGraph) -> void {
        m_ShutdownRequested.store(false, std::memory_order_release);
        m_TaskGraph = &InTaskGraph;
    }

    auto BeginShutdown() -> void {
        std::lock_guard Lock(m_PublishMutex);
        m_ShutdownRequested.store(true, std::memory_order_release);
        LogDebug("Resource manager shutdown requested");
    }

    [[nodiscard]] auto GetTaskGraph() const -> TaskGraph* {
        return m_TaskGraph;
    }

    [[nodiscard]] auto IsShutdownRequested() const -> bool {
        return m_ShutdownRequested.load(std::memory_order_acquire);
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto CreateOrGet(String Key) -> ResourceRequestResult<T> {
        auto& Family = GetFamily<T>();

        std::lock_guard Lock(Family.Mutex);
        if (auto It = Family.Entries.find(Key); It != Family.Entries.end()) {
            auto* Entry = It->second.get();
            if (Entry && Entry->Policy == ResourceLifetimePolicy::Transient && Entry->RefCount == 0 &&
                Entry->Slot.IsReleased()) {
                auto Generation = Entry->Slot.Reset();
                return ResourceRequestResult<T>{
                    .Handle          = ResourceHandle<T>::Create(Key, Generation),
                    .ShouldStartWork = true,
                };
            }

            auto Generation = Entry ? Entry->Slot.GetGeneration() : 0;
            LogDebug("{} request coalesced '{}'", ResourceTraits<T>::Info.Label, Key);
            return ResourceRequestResult<T>{
                .Handle          = ResourceHandle<T>::Create(Key, Generation),
                .ShouldStartWork = false,
            };
        }

        auto Entry  = std::make_unique<ResourceEntry<T>>();
        auto Gen    = Entry->Slot.Reset();
        auto Handle = ResourceHandle<T>::Create(Key, Gen);
        Family.Entries.emplace(std::move(Key), std::move(Entry));
        return ResourceRequestResult<T>{
            .Handle          = Handle,
            .ShouldStartWork = true,
        };
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto GetState(const ResourceHandle<T>& Handle) -> ResourceState {
        auto Locked = LockEntry(Handle);
        if (!Locked)
            return ResourceState::Unknown;

        return Locked.Entry->Slot.GetState(Handle.GetGeneration());
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto GetError(const ResourceHandle<T>& Handle) -> std::optional<ErrorMessage> {
        auto Locked = LockEntry(Handle);
        if (!Locked)
            return std::nullopt;

        return Locked.Entry->Slot.GetError(Handle.GetGeneration());
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto Pin(const ResourceHandle<T>& Handle) -> ResourcePin<T> {
        auto Locked = LockEntry(Handle);
        if (!Locked)
            return {};

        return Locked.Entry->Slot.PinReady(Handle.GetGeneration());
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto AddRef(const ResourceHandle<T>& Handle) -> bool {
        auto Locked = LockEntry(Handle);
        if (!Locked)
            return false;

        if (Locked.Entry->Slot.GetGeneration() != Handle.GetGeneration())
            return false;
        if (Locked.Entry->Slot.GetState(Handle.GetGeneration()) == ResourceState::Stale)
            return false;

        ++Locked.Entry->RefCount;
        return true;
    }

    template <ManagedRHIResource T>
    auto ReleaseRef(const ResourceHandle<T>& Handle) -> void {
        auto Locked = LockEntry(Handle);
        if (!Locked || Locked.Entry->Slot.GetGeneration() != Handle.GetGeneration())
            return;

        if (Locked.Entry->RefCount > 0)
            --Locked.Entry->RefCount;
        if (Locked.Entry->RefCount == 0 && Locked.Entry->Policy == ResourceLifetimePolicy::Transient)
            Locked.Entry->Slot.RequestRelease();
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto MarkRhiCommitting(const String& Key, ResourceGeneration Generation) -> bool {
        auto Locked = LockEntry<T>(Key);
        if (!Locked)
            return false;

        return Locked.Entry->Slot.MarkRhiCommitting(Generation);
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto PublishReady(const String& Key, ResourceGeneration Generation, Resource<T> Value) -> bool {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested())
            return false;

        auto Locked = LockEntry<T>(Key);
        if (!Locked)
            return false;

        return Locked.Entry->Slot.PublishReady(Generation, std::move(Value));
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto PublishFailed(const String& Key, ResourceGeneration Generation, ErrorMessage Error) -> bool {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested())
            return false;

        auto Locked = LockEntry<T>(Key);
        if (!Locked)
            return false;

        return Locked.Entry->Slot.PublishFailed(Generation, std::move(Error));
    }

    template <GpuPendingManagedRHIResource T>
    [[nodiscard]] auto PublishGpuPending(const String& Key,
                                         ResourceGeneration Generation,
                                         Resource<T> Value,
                                         RHI::GpuCompletionToken UploadCompletion) -> bool {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async {} GPU pending discarded after shutdown '{}'", ResourceTraits<T>::Info.Label, Key);
            return false;
        }

        auto& Family = GetFamily<T>();
        std::lock_guard FamilyLock(Family.Mutex);
        auto* Entry = FindEntry(Family.Entries, Key);
        if (!Entry || !Entry->Slot.PublishGpuPending(Generation)) {
            LogDebug("Stale async {} GPU pending discarded '{}'", ResourceTraits<T>::Info.Label, Key);
            return false;
        }

        Family.GpuPending.push_back(GpuPendingResource<T>{
            .Generation       = Generation,
            .Key              = Key,
            .Resource         = std::move(Value),
            .UploadCompletion = UploadCompletion,
        });
        return true;
    }

    auto TickGpuPending() -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            ClearGpuPendingQueues();
            return;
        }

        ForEachGpuPendingFamily([this]<GpuPendingManagedRHIResource T>() -> void {
            TickGpuPendingFamily<T>();
        });
    }

    auto Clear() -> void {
        ForEachFamily([]<ManagedRHIResource T>(ResourceFamily<T>& Family) -> void {
            std::lock_guard Lock(Family.Mutex);
            ReleaseAndEraseEntries(Family.Entries);
        });

        {
            std::lock_guard Lock(m_PublishMutex);
            ClearGpuPendingQueues();
        }
    }

    auto CollectReleasedResources() -> void {
        ForEachFamily([]<ManagedRHIResource T>(ResourceFamily<T>& Family) -> void {
            std::lock_guard Lock(Family.Mutex);
            EraseReleasedTransientEntries(Family.Entries);
        });
    }

  private:
    template <ManagedRHIResource T>
    [[nodiscard]] auto GetFamily() -> ResourceFamily<T>& {
        return std::get<ResourceFamily<T>>(m_Families);
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto LockEntry(const ResourceHandle<T>& Handle) -> LockedResourceEntry<T> {
        if (!Handle.IsValid())
            return {};

        return LockEntry<T>(Handle.GetKey());
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto LockEntry(const String& Key) -> LockedResourceEntry<T> {
        auto& Family = GetFamily<T>();
        auto  Lock   = std::unique_lock<std::mutex>(Family.Mutex);
        return LockedResourceEntry<T>{
            .Lock  = std::move(Lock),
            .Entry = FindEntry(Family.Entries, Key),
        };
    }

    template <ManagedRHIResource T>
    [[nodiscard]] static auto FindEntry(ResourceEntryMap<T>& Entries, const String& Key) -> ResourceEntry<T>* {
        auto It = Entries.find(Key);
        if (It == Entries.end() || !It->second)
            return nullptr;
        return It->second.get();
    }

    template <typename Fn>
    auto ForEachFamily(Fn&& Callback) -> void {
        std::apply(
            [&]<typename... Families>(Families&... Family) -> void {
                (Callback.template operator()<typename Families::ResourceType>(Family), ...);
            },
            m_Families);
    }

    template <typename Fn>
    auto ForEachGpuPendingFamily(Fn&& Callback) -> void {
        ForEachFamily([&]<ManagedRHIResource T>(ResourceFamily<T>&) -> void {
            if constexpr (ResourceTraits<T>::Info.HasGpuPending())
                Callback.template operator()<T>();
        });
    }

    auto ClearGpuPendingQueues() -> void {
        ForEachGpuPendingFamily([this]<GpuPendingManagedRHIResource T>() -> void {
            GetFamily<T>().GpuPending.clear();
        });
    }

    template <typename T>
    static auto ReleaseAndEraseEntries(ResourceEntryMap<T>& Entries) -> void {
        for (auto It = Entries.begin(); It != Entries.end();) {
            if (It->second)
                It->second->Slot.RequestRelease();
            if (!It->second || !It->second->Slot.HasPins())
                It = Entries.erase(It);
            else
                ++It;
        }
    }

    template <typename T>
    static auto EraseReleasedTransientEntries(ResourceEntryMap<T>& Entries) -> void {
        for (auto It = Entries.begin(); It != Entries.end();) {
            if (!It->second ||
                (It->second->Policy == ResourceLifetimePolicy::Transient && It->second->RefCount == 0 &&
                 !It->second->Slot.HasPins() && It->second->Slot.IsReleased()))
                It = Entries.erase(It);
            else
                ++It;
        }
    }

    template <GpuPendingManagedRHIResource T>
    auto TickGpuPendingFamily() -> void {
        auto& Family = GetFamily<T>();
        std::vector<GpuPendingResource<T>> Next;
        Next.reserve(Family.GpuPending.size());
        for (auto& Pending : Family.GpuPending) {
            auto State = ResourceState::Unknown;
            {
                std::lock_guard Lock(Family.Mutex);
                auto* Entry = FindEntry(Family.Entries, Pending.Key);
                State = Entry ? Entry->Slot.GetState(Pending.Generation) : ResourceState::Unknown;
            }
            if (State == ResourceState::Stale) {
                LogDebug("Stale async {} GPU pending discarded '{}'", ResourceTraits<T>::Info.Label, Pending.Key);
                continue;
            }
            if (State != ResourceState::GpuPending) {
                LogDebug("Async {} GPU pending discarded '{}'", ResourceTraits<T>::Info.Label, Pending.Key);
                continue;
            }
            if (!RHI::RenderDevice::Get().IsGpuComplete(Pending.UploadCompletion)) {
                Next.push_back(std::move(Pending));
                continue;
            }

            auto Published = false;
            {
                std::lock_guard Lock(Family.Mutex);
                auto* Entry = FindEntry(Family.Entries, Pending.Key);
                Published = Entry && Entry->Slot.PublishReady(Pending.Generation, std::move(Pending.Resource));
            }
            if (!Published) {
                LogDebug("Stale async {} ready discarded '{}'", ResourceTraits<T>::Info.Label, Pending.Key);
                continue;
            }
            LogInfo("Async {} ready '{}'", ResourceTraits<T>::Info.Label, Pending.Key);
        }
        Family.GpuPending = std::move(Next);
    }

    ResourceFamilies                            m_Families = {};
    TaskGraph*                                  m_TaskGraph = nullptr;
    std::atomic<bool>                           m_ShutdownRequested = false;
    std::mutex                                  m_PublishMutex;
};

} // namespace SoulEngine::Resource
