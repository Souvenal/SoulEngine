/// @file   Vulkan/VKContext.cppm
/// @brief  Vulkan device services and shared image-state tracking.

module;

// volk.h must precede every other Vulkan header in this fragment: it defines VK_NO_PROTOTYPES itself before
// pulling in vulkan_core.h, and errors out if vulkan.h was already included without it. Dropping the raw C
// prototypes is safe here — every call goes through Vulkan-Hpp's dynamic dispatcher or VMA's dynamically
// fetched function table.
#include <volk.h>
#include <vk_mem_alloc.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

export module Vulkan:Context;

import Core;
import vulkan;
import std;

import :Capability;
import :Debug;
import :SurfaceProvider;
import :Semaphore;
import WindowSystem;

export namespace SoulEngine {

/// @brief Tracked Vulkan image subresource state used for automatic synchronization barriers.
struct VulkanImageState {
    vk::PipelineStageFlags2 stage       = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access      = vk::AccessFlagBits2::eNone;
    vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
    Uint32                  queueFamily = vk::QueueFamilyIgnored;
    bool                    isWrite     = false;
};

/// @brief Per-subresource tracking key: (image, single aspect bit, mip level, array layer).
struct VulkanImageSubresourceKey {
    vk::Image               Image      = nullptr;
    vk::ImageAspectFlagBits Aspect     = vk::ImageAspectFlagBits::eColor;
    Uint32                  MipLevel   = 0;
    Uint32                  ArrayLayer = 0;

    bool operator==(const VulkanImageSubresourceKey&) const = default;
};

struct VulkanImageSubresourceKeyHash {
    auto operator()(const VulkanImageSubresourceKey& Key) const -> std::size_t {
        std::size_t Hash    = std::hash<VkImage>{}(static_cast<VkImage>(Key.Image));
        auto        Combine = [&Hash](Uint32 Value) {
            Hash ^= std::hash<Uint32>{}(Value) + 0x9e3779b9U + (Hash << 6U) + (Hash >> 2U);
        };
        Combine(std::to_underlying(Key.Aspect));
        Combine(Key.MipLevel);
        Combine(Key.ArrayLayer);
        return Hash;
    }
};

/// @brief Device-level per-subresource image state tracker shared by Vulkan command recording.
///
/// State is keyed by (image, aspect, mip, layer); barrier granularity matches the key — no whole-image
/// over-sync, no coalescing (all engine images have arrayLayers == 1 and at most ~13 mips, so per-subresource
/// barrier counts stay small). Ranges are always concrete: every call site resolves subresource counts from
/// the payload it already holds, so the tracker stores no image metadata and creation never touches it.
/// RHI-thread confined — Seed/Forget/Transition all run there.
class VulkanImageTracker {
  public:
    VulkanImageTracker() = default;

    /// @brief Seed every (color, mip, layer) subresource with State.
    ///
    /// Only for images whose initial state is not Undefined (swapchain images seed their transfer stage); a
    /// default-constructed state needs no seeding — Transition's missing-key default is already Undefined.
    /// Depth/stencil aspect keys are created lazily by Transition.
    auto Seed(vk::Image Image, Uint32 MipLevels, Uint32 ArrayLayers, VulkanImageState State) -> void {
        for (Uint32 Mip = 0; Mip < MipLevels; ++Mip)
            for (Uint32 Layer = 0; Layer < ArrayLayers; ++Layer)
                m_SubresourceStates[VulkanImageSubresourceKey{
                    .Image = Image, .Aspect = vk::ImageAspectFlagBits::eColor, .MipLevel = Mip, .ArrayLayer = Layer}] =
                    State;
    }

    /// @brief Drop a destroyed image's tracked state.
    ///
    /// Without this, a driver-reused VkImage handle would make the new image inherit a stale layout and emit
    /// an invalid oldLayout barrier (pool era: eviction destroys idle images while their entries would
    /// otherwise linger).
    auto Forget(vk::Image Image) -> void {
        for (auto It = m_SubresourceStates.begin(); It != m_SubresourceStates.end();) {
            if (It->first.Image == Image)
                It = m_SubresourceStates.erase(It);
            else
                ++It;
        }
    }

    /// @brief Drive every subresource in Range to DesiredState.
    ///
    /// Emits one image memory barrier per subresource whose tracked state differs (or after a prior write).
    /// Range must be concrete — no vk::RemainingMipLevels/vk::RemainingArrayLayers.
    auto Transition(const vk::raii::CommandBuffer&  CmdBuf,
                    vk::Image                       Image,
                    const vk::ImageSubresourceRange Range,
                    const VulkanImageState&         DesiredState) -> void {
        const Uint32 MipCount   = Range.levelCount;
        const Uint32 LayerCount = Range.layerCount;

        static constexpr vk::ImageAspectFlagBits KnownAspects[] = {
            vk::ImageAspectFlagBits::eColor,
            vk::ImageAspectFlagBits::eDepth,
            vk::ImageAspectFlagBits::eStencil,
        };

        std::vector<vk::ImageMemoryBarrier2> Barriers;
        for (const auto Aspect : KnownAspects) {
            if ((static_cast<Uint32>(Range.aspectMask) & static_cast<Uint32>(Aspect)) == 0)
                continue;
            for (Uint32 Mip = Range.baseMipLevel; Mip < Range.baseMipLevel + MipCount; ++Mip) {
                for (Uint32 Layer = Range.baseArrayLayer; Layer < Range.baseArrayLayer + LayerCount; ++Layer) {
                    const VulkanImageSubresourceKey Key{
                        .Image = Image, .Aspect = Aspect, .MipLevel = Mip, .ArrayLayer = Layer};
                    const auto             It = m_SubresourceStates.find(Key);
                    const VulkanImageState Current =
                        (It != m_SubresourceStates.end()) ? It->second : VulkanImageState{};

                    const bool NeedsBarrier = (Current.stage != DesiredState.stage) ||
                                              (Current.access != DesiredState.access) ||
                                              (Current.layout != DesiredState.layout) || Current.isWrite;

                    if (NeedsBarrier) {
                        Barriers.push_back(vk::ImageMemoryBarrier2{
                            .srcStageMask        = Current.stage,
                            .srcAccessMask       = Current.access,
                            .dstStageMask        = DesiredState.stage,
                            .dstAccessMask       = DesiredState.access,
                            .oldLayout           = Current.layout,
                            .newLayout           = DesiredState.layout,
                            .srcQueueFamilyIndex = Current.queueFamily,
                            .dstQueueFamilyIndex = DesiredState.queueFamily,
                            .image               = Image,
                            .subresourceRange    = {.aspectMask     = Aspect,
                                                    .baseMipLevel   = Mip,
                                                    .levelCount     = 1,
                                                    .baseArrayLayer = Layer,
                                                    .layerCount     = 1},
                        });
                    }

                    m_SubresourceStates[Key] = DesiredState;
                }
            }
        }

        if (!Barriers.empty()) {
            vk::DependencyInfo Dependency{
                .dependencyFlags         = vk::DependencyFlagBits::eByRegion,
                .imageMemoryBarrierCount = static_cast<Uint32>(Barriers.size()),
                .pImageMemoryBarriers    = Barriers.data(),
            };
            CmdBuf.pipelineBarrier2(Dependency);
        }
    }

  private:
    std::unordered_map<VulkanImageSubresourceKey, VulkanImageState, VulkanImageSubresourceKeyHash> m_SubresourceStates =
        {};
};

/// @brief Tracked Vulkan buffer range state used for automatic synchronization barriers.
///
/// Buffers have no layout; only the pipeline stage, access mask, and write hazard flag are tracked.
struct VulkanBufferState {
    vk::PipelineStageFlags2 stage   = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access  = vk::AccessFlagBits2::eNone;
    bool                    isWrite = false;
};

/// @brief Device-level buffer state tracker shared by Vulkan command recording.
///
/// State is keyed by (buffer, offset). Transient belt slices are disjoint within a frame and an in-flight
/// frame's ranges stay ring-owned until the timeline retires them, so keys never collide with another slice's
/// live range; persistent buffers simply always use offset 0. The tracker is Reset() at every BeginFrame: the
/// frame-slot fence guarantees the previous occupant's GPU work has completed, so a range's first use within
/// a frame needs no barrier and is registered directly.
class VulkanBufferTracker {
  public:
    VulkanBufferTracker() = default;

    /// @brief Drop all tracked range states.
    ///
    /// Must be called once per frame before new transient allocations are recorded.
    auto Reset() -> void {
        m_BufferStates.clear();
    }

    /// @brief Drive the buffer range [Offset, Offset+Size) to DesiredState.
    ///
    /// Emits a buffer memory barrier when the tracked state differs. A WAR hazard (read -> write) needs an
    /// execution dependency, and any previous write (WAW/RAW) forces a barrier just like the image tracker.
    auto Transition(vk::raii::CommandBuffer& CmdBuf,
                    vk::Buffer               Buffer,
                    vk::DeviceSize           Offset,
                    vk::DeviceSize           Size,
                    const VulkanBufferState& DesiredState) -> void {
        const Key               RangeKey{.Buffer = Buffer, .Offset = Offset};
        const auto              It      = m_BufferStates.find(RangeKey);
        const VulkanBufferState Current = (It != m_BufferStates.end()) ? It->second : VulkanBufferState{};

        const bool NeedsBarrier =
            It != m_BufferStates.end() &&
            ((Current.stage != DesiredState.stage) || (Current.access != DesiredState.access) || Current.isWrite);

        if (NeedsBarrier) {
            vk::BufferMemoryBarrier2 Barrier{
                .srcStageMask        = Current.stage,
                .srcAccessMask       = Current.access,
                .dstStageMask        = DesiredState.stage,
                .dstAccessMask       = DesiredState.access,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .buffer              = Buffer,
                .offset              = Offset,
                .size                = Size,
            };
            vk::DependencyInfo Dependency{
                .dependencyFlags          = {},
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers    = &Barrier,
            };
            CmdBuf.pipelineBarrier2(Dependency);
        }

        m_BufferStates[RangeKey] = DesiredState;
    }

  private:
    struct Key {
        vk::Buffer     Buffer = nullptr;
        vk::DeviceSize Offset = 0;

        auto operator==(const Key&) const -> bool = default;
    };

    struct KeyHash {
        auto operator()(const Key& K) const noexcept -> std::size_t {
            const std::size_t BufferHash = std::hash<VkBuffer>{}(static_cast<VkBuffer>(K.Buffer));
            return BufferHash ^
                   (std::hash<vk::DeviceSize>{}(K.Offset) + 0x9e3779b9 + (BufferHash << 6) + (BufferHash >> 2));
        }
    };

    std::unordered_map<Key, VulkanBufferState, KeyHash> m_BufferStates = {};
};

/// @brief Backend-owned deferred destruction queue for VMA resources (buffers and images).
///
/// Destruction is treated as a virtual timeline event: Enqueue stamps the entry with a fresh host value
/// (IncreaseHostValue) that no submission ever signals directly — the monotonic device counter covers it once
/// the NEXT real submission completes. No new command can reference a resource after its last ref is gone, so
/// the destruction frame's own submission is an upper bound on its last GPU use, and "covered by the next
/// submission" is always safe. Each lane's entries arrive in non-decreasing stamp order, so retirement pops
/// from the front.
///
/// Distinct from the RHI-level deferred deletion queue: that one is the cross-thread transport (any thread ->
/// RHI thread); this one is the GPU-retirement hop (RHI thread -> GPU completion). Tick() retires entries the
/// device timeline has passed; Flush() destroys unconditionally (shutdown, after the device is idle).
/// RHI-thread confined — buffer destructors only run there; the stamp itself comes from an atomic counter.
class VulkanDeferredDeletionQueue {
  public:
    VulkanDeferredDeletionQueue() = default;
    /// Safety net: the proper flush point is device Shutdown after the device is idle.
    ~VulkanDeferredDeletionQueue() {
        Flush();
    }

    VulkanDeferredDeletionQueue(const VulkanDeferredDeletionQueue&)                    = delete;
    auto operator=(const VulkanDeferredDeletionQueue&) -> VulkanDeferredDeletionQueue& = delete;
    VulkanDeferredDeletionQueue(VulkanDeferredDeletionQueue&&)                         = delete;
    auto operator=(VulkanDeferredDeletionQueue&&) -> VulkanDeferredDeletionQueue&      = delete;

    /// One dying VMA buffer: the handles destruction needs, plus its retirement stamp.
    struct BufferEntry {
        Uint64        RetireValue = 0;       ///< virtual event value; retired once the device timeline passes it
        vk::Buffer    Buffer      = nullptr; ///< native buffer handle
        VmaAllocation Allocation  = nullptr; ///< VMA allocation destroyed together with the handle
    };

    /// One dying VMA image: the handles destruction needs, plus its retirement stamp.
    struct ImageEntry {
        Uint64        RetireValue = 0;       ///< virtual event value; retired once the device timeline passes it
        vk::Image     Image       = nullptr; ///< native image handle
        VmaAllocation Allocation  = nullptr; ///< VMA allocation destroyed together with the handle
    };

    /// Bind the allocator and timeline the queue destroys and stamps through.
    auto Init(VmaAllocator Allocator, VulkanTimelineSemaphore& Timeline) -> void {
        m_Allocator = Allocator;
        m_Timeline  = &Timeline;
    }

    /// @brief Queue a dying buffer, stamped with a fresh virtual event value.
    ///
    /// The handles stay valid until Tick() retires the entry, so a same-frame consumer that snapshotted the
    /// raw handle (the frame-start upload batch) records safely.
    auto Enqueue(vk::Buffer Buffer, VmaAllocation Allocation) -> void {
        m_BufferEntries.push_back(
            BufferEntry{.RetireValue = m_Timeline->IncreaseHostValue(), .Buffer = Buffer, .Allocation = Allocation});
    }

    /// @brief Queue a dying image, stamped with a fresh virtual event value. Same contract as the
    /// buffer overload: the frame-start upload batch's image lane snapshots raw image handles.
    auto Enqueue(vk::Image Image, VmaAllocation Allocation) -> void {
        m_ImageEntries.push_back(
            ImageEntry{.RetireValue = m_Timeline->IncreaseHostValue(), .Image = Image, .Allocation = Allocation});
    }

    /// Retire every entry whose stamp the device timeline has passed.
    auto Tick() -> void {
        const Uint64 Completed = m_Timeline->GetDeviceValue();
        while (!m_BufferEntries.empty() && m_BufferEntries.front().RetireValue <= Completed) {
            vmaDestroyBuffer(
                m_Allocator, static_cast<VkBuffer>(m_BufferEntries.front().Buffer), m_BufferEntries.front().Allocation);
            m_BufferEntries.pop_front();
        }
        while (!m_ImageEntries.empty() && m_ImageEntries.front().RetireValue <= Completed) {
            vmaDestroyImage(
                m_Allocator, static_cast<VkImage>(m_ImageEntries.front().Image), m_ImageEntries.front().Allocation);
            m_ImageEntries.pop_front();
        }
    }

    /// Destroy every remaining entry regardless of stamps. Shutdown only, after the device is idle.
    auto Flush() -> void {
        if (!m_Allocator)
            return;
        for (const BufferEntry& E : m_BufferEntries)
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(E.Buffer), E.Allocation);
        m_BufferEntries.clear();
        for (const ImageEntry& E : m_ImageEntries)
            vmaDestroyImage(m_Allocator, static_cast<VkImage>(E.Image), E.Allocation);
        m_ImageEntries.clear();
    }

  private:
    VmaAllocator             m_Allocator     = nullptr; ///< borrowed from the resource context
    VulkanTimelineSemaphore* m_Timeline      = nullptr; ///< borrowed graphics timeline
    std::deque<BufferEntry>  m_BufferEntries = {};
    std::deque<ImageEntry>   m_ImageEntries  = {};
};

/// Owns Vulkan objects and services shared by backend resource factories.
class VulkanResourceContext final {
  public:
    VulkanResourceContext() = default;

    ~VulkanResourceContext() {
        DestroyTracyCtx();
        if (m_Allocator)
            vmaDestroyAllocator(m_Allocator);
    }

    VulkanResourceContext(const VulkanResourceContext&)                    = delete;
    auto operator=(const VulkanResourceContext&) -> VulkanResourceContext& = delete;
    VulkanResourceContext(VulkanResourceContext&&)                         = delete;
    auto operator=(VulkanResourceContext&&) -> VulkanResourceContext&      = delete;

    [[nodiscard]] static auto Create(IWindowSystem* WindowSys, Uint32 FramesInFlight)
        -> std::expected<UPtr<VulkanResourceContext>, ErrorMessage> {
        auto SurfaceProvider = CreateVulkanSurfaceProvider(WindowSys);
        if (!SurfaceProvider)
            return std::unexpected(SurfaceProvider.error().Append("Failed to create Vulkan surface provider"));

        auto Result               = std::make_unique<VulkanResourceContext>();
        Result->m_SurfaceProvider = std::move(*SurfaceProvider);
        Result->m_FramesInFlight  = FramesInFlight;

        // vk::raii::Context does: 1. get `vkGetInstanceProcAddr`, whether dynamically or statically
        //    (dynamically means mechanism like dlopen)
        // 2. using `vkGetInstanceProcAddr` to collect all Instance Functions
        //
        // Once an instance is created, the instance will hold `vkGetInstanceProcAddr`, thus the Context can
        // be locally assigned.
        //
        // volk performs step 1 in place of Vulkan-Hpp's `DynamicLoader`: it searches for the loader from
        // variable paths and reports a missing loader as VK_ERROR_INITIALIZATION_FAILED instead of seg
        // faulting on a null vkGetInstanceProcAddr.
        if (const VkResult VolkResult = volkInitialize(); VolkResult != VK_SUCCESS)
            return std::unexpected(ErrorMessage(Format("volk failed to load the vulkan loader: {}",
                                                       vk::to_string(static_cast<vk::Result>(VolkResult)))));
        vk::raii::Context Context(vkGetInstanceProcAddr);
        if (auto Res = Result->CreateInstance(Context); !Res)
            return std::unexpected(Res.error());

        // Surface must exist before PickPhysicalDevice so we can verify surface presentation support via
        // getSurfaceSupportKHR.
        auto Surface = Result->m_SurfaceProvider->CreateSurface(Result->m_Instance);
        if (!Surface)
            return std::unexpected(Surface.error().Append("Failed to create Vulkan presentation surface"));
        Result->m_Surface = std::move(*Surface);

        if (auto Res = Result->PickPhysicalDevice(); !Res)
            return std::unexpected(Res.error());
        if (auto Res = Result->CreateLogicalDevice(); !Res)
            return std::unexpected(Res.error());

        auto Timeline = VulkanTimelineSemaphore::Create(
            Result->m_Device, &Result->m_DebugUtils, "Internal/Semaphore/GraphicsTimeline");
        if (!Timeline)
            return std::unexpected(Timeline.error().Append("Vulkan graphics timeline creation failed"));
        Result->m_Timeline = std::move(*Timeline);

        if (auto Res = Result->CreateVMA(Context); !Res)
            return std::unexpected(Res.error());
        Result->m_DeferredDeletionQueue.Init(Result->m_Allocator, Result->m_Timeline);

        Result->CreateTracyCtx();

        return Result;
    }

    [[nodiscard]] auto GetInstance() const -> vk::raii::Instance& {
        return const_cast<vk::raii::Instance&>(m_Instance);
    }
    [[nodiscard]] auto GetSurface() const -> vk::raii::SurfaceKHR& {
        return const_cast<vk::raii::SurfaceKHR&>(m_Surface);
    }
    [[nodiscard]] auto GetPhysicalDevice() const -> vk::raii::PhysicalDevice& {
        return const_cast<vk::raii::PhysicalDevice&>(m_PhysicalDevice);
    }
    [[nodiscard]] auto GetDevice() const -> vk::raii::Device& {
        return const_cast<vk::raii::Device&>(m_Device);
    }
    [[nodiscard]] auto GetDebugUtils() const -> VulkanDebugUtils& {
        return const_cast<VulkanDebugUtils&>(m_DebugUtils);
    }
    /// Create the Tracy GPU profiling context after the device and queues exist.
    auto CreateTracyCtx() -> void {
        if (m_TracyCtx || !VulkanCapability::Get().IsDeviceExtensionEnabled(vk::EXTCalibratedTimestampsExtensionName))
            return;

        const auto& Dispatcher   = *m_Instance.getDispatcher();
        const auto  InstanceProc = Dispatcher.vkGetInstanceProcAddr;
        const auto  DeviceProc   = Dispatcher.vkGetDeviceProcAddr;
        const auto  PDCTD        = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
            InstanceProc(static_cast<VkInstance>(*m_Instance), "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
        const auto GCT = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
            DeviceProc(static_cast<VkDevice>(*m_Device), "vkGetCalibratedTimestampsEXT"));
        if (!PDCTD || !GCT)
            return;

        const vk::CommandPoolCreateInfo PoolCI{
            .flags = vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = m_GraphicsFamily,
        };
        auto PoolRes = m_Device.createCommandPool(PoolCI);
        if (PoolRes.result != vk::Result::eSuccess)
            return;
        const vk::CommandBufferAllocateInfo AllocCI{
            .commandPool        = *PoolRes.value,
            .level              = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        auto InitCmdRes = m_Device.allocateCommandBuffers(AllocCI);
        if (InitCmdRes.result != vk::Result::eSuccess)
            return;
        m_TracyCtx = tracy::CreateVkContext(static_cast<VkInstance>(*m_Instance),
                                            static_cast<VkPhysicalDevice>(*m_PhysicalDevice),
                                            static_cast<VkDevice>(*m_Device),
                                            static_cast<VkQueue>(*m_GraphicsQueue),
                                            static_cast<VkCommandBuffer>(*InitCmdRes.value[0]),
                                            InstanceProc,
                                            DeviceProc,
                                            true);
    }

    auto DestroyTracyCtx() noexcept -> void {
        if (!m_TracyCtx)
            return;
        tracy::DestroyVkContext(m_TracyCtx);
        m_TracyCtx = nullptr;
    }

    [[nodiscard]] auto GetTracyCtx() const noexcept -> tracy::VkCtx* {
        return m_TracyCtx;
    }

    [[nodiscard]] auto GetGraphicsQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_GraphicsQueue);
    }
    [[nodiscard]] auto GetComputeQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_ComputeQueue);
    }
    [[nodiscard]] auto GetTransferQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_TransferQueue);
    }
    [[nodiscard]] auto GetImageTracker() const -> VulkanImageTracker& {
        return const_cast<VulkanImageTracker&>(m_ImageTracker);
    }
    [[nodiscard]] auto GetBufferTracker() const -> VulkanBufferTracker& {
        return const_cast<VulkanBufferTracker&>(m_BufferTracker);
    }
    [[nodiscard]] auto GetAllocator() const -> VmaAllocator {
        return m_Allocator;
    }
    [[nodiscard]] auto GetSurfaceProvider() const -> IVulkanSurfaceProvider& {
        return *m_SurfaceProvider;
    }
    [[nodiscard]] auto GetGraphicsFamily() const -> Uint32 {
        return m_GraphicsFamily;
    }
    [[nodiscard]] auto GetComputeFamily() const -> Uint32 {
        return m_ComputeFamily;
    }
    [[nodiscard]] auto GetTransferFamily() const -> Uint32 {
        return m_TransferFamily;
    }
    [[nodiscard]] auto GetFramesInFlight() const -> Uint32 {
        return m_FramesInFlight;
    }
    /// @brief Current frame slot being executed, written by the render device in BeginFrame.
    ///
    /// RHI-thread confined: written and read only while executing that thread's frame, so a plain value
    /// suffices. Frame-slotted payloads (e.g. the TLAS) resolve their internal slot from it.
    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 {
        return m_CurrentFrameIndex;
    }
    auto SetCurrentFrameIndex(Uint32 FrameIndex) -> void {
        m_CurrentFrameIndex = FrameIndex;
    }
    [[nodiscard]] auto GetTimeline() const -> VulkanTimelineSemaphore& {
        return const_cast<VulkanTimelineSemaphore&>(m_Timeline);
    }
    [[nodiscard]] auto GetDeferredDeletionQueue() const -> VulkanDeferredDeletionQueue& {
        return const_cast<VulkanDeferredDeletionQueue&>(m_DeferredDeletionQueue);
    }

  private:
    [[nodiscard]] auto CreateInstance(vk::raii::Context& Context) -> std::expected<void, ErrorMessage> {
        vk::ApplicationInfo AppInfo{
            .pApplicationName   = "SoulEngine Application",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName        = "SoulEngine",
            .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion         = vk::ApiVersion14,
        };

        auto EnabledLayers = VulkanCapability::Get().ResolveInstanceLayers(Context);
        if (!EnabledLayers)
            return std::unexpected(EnabledLayers.error());
        const bool DebugUtils = VulkanDebugUtils::IsConfigEnabled();

        auto RequiredInstanceExtensions = m_SurfaceProvider->GetRequiredInstanceExtensions();
        if (!RequiredInstanceExtensions)
            return std::unexpected(
                RequiredInstanceExtensions.error().Append("Failed to query window-system Vulkan extensions"));
        if (DebugUtils)
            RequiredInstanceExtensions->emplace_back(vk::EXTDebugUtilsExtensionName);

        auto EnabledInstanceExts =
            VulkanCapability::Get().ResolveInstanceExtensions(Context, *RequiredInstanceExtensions);
        if (!EnabledInstanceExts)
            return std::unexpected(EnabledInstanceExts.error());

        for (auto* Ext : *EnabledInstanceExts)
            LogDebug("Enabled instance extension: {}", Ext);
        for (auto* Layer : *EnabledLayers)
            LogDebug("Enabled instance layer: {}", Layer);

        vk::InstanceCreateInfo InstCI{
            .pApplicationInfo        = &AppInfo,
            .enabledLayerCount       = static_cast<Uint32>(EnabledLayers->size()),
            .ppEnabledLayerNames     = EnabledLayers->data(),
            .enabledExtensionCount   = static_cast<Uint32>(EnabledInstanceExts->size()),
            .ppEnabledExtensionNames = EnabledInstanceExts->data(),
        };
        if (VulkanCapability::Get().IsInstanceExtensionEnabled(vk::KHRPortabilityEnumerationExtensionName))
            InstCI.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;

        if (const auto ChainCI = VulkanDebugUtils::PrepareInstanceChain()) {
            vk::StructureChain<vk::InstanceCreateInfo, vk::DebugUtilsMessengerCreateInfoEXT> InstanceChain{
                InstCI,
                *ChainCI,
            };
            auto InstanceResult = Context.createInstance(InstanceChain.get<vk::InstanceCreateInfo>());
            if (InstanceResult.result != vk::Result::eSuccess)
                return std::unexpected(
                    ErrorMessage(Format("Failed to create Vulkan instance: {}", vk::to_string(InstanceResult.result))));
            m_Instance = std::move(InstanceResult.value);
        } else {
            auto InstanceResult = Context.createInstance(InstCI);
            if (InstanceResult.result != vk::Result::eSuccess)
                return std::unexpected(
                    ErrorMessage(Format("Failed to create Vulkan instance: {}", vk::to_string(InstanceResult.result))));
            m_Instance = std::move(InstanceResult.value);
        }

        if (const auto MessengerRes = m_DebugUtils.InitializeMessenger(m_Instance); !MessengerRes)
            return std::unexpected(MessengerRes.error());
        return {};
    }

    [[nodiscard]] auto PickPhysicalDevice() -> std::expected<void, ErrorMessage> {
        auto DevicesResult = m_Instance.enumeratePhysicalDevices();
        if (DevicesResult.result != vk::Result::eSuccess || DevicesResult.value.empty())
            return std::unexpected(ErrorMessage("No Vulkan-capable physical devices found"));

        vk::StructureChain<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties> PropsChain;
        for (std::size_t i = 0; i < DevicesResult.value.size(); ++i) {
            auto PD = DevicesResult.value[i];
            PD.getProperties2(&PropsChain.get<vk::PhysicalDeviceProperties2>());
            auto& DevProps = PropsChain.get<vk::PhysicalDeviceProperties2>().properties;
            auto& Driver   = PropsChain.get<vk::PhysicalDeviceDriverProperties>();
            LogDebug("Physical device [{}]: {} (driver: {}, ICD: {}, API version {}.{}.{})",
                     i,
                     static_cast<const char*>(DevProps.deviceName),
                     static_cast<const char*>(Driver.driverName),
                     vk::to_string(Driver.driverID),
                     VK_API_VERSION_MAJOR(DevProps.apiVersion),
                     VK_API_VERSION_MINOR(DevProps.apiVersion),
                     VK_API_VERSION_PATCH(DevProps.apiVersion));
        }

        m_PhysicalDevice = std::move(DevicesResult.value[0]);
        m_PhysicalDevice.getProperties2(&PropsChain.get<vk::PhysicalDeviceProperties2>());
        auto& DevProps = PropsChain.get<vk::PhysicalDeviceProperties2>().properties;
        auto& Driver   = PropsChain.get<vk::PhysicalDeviceDriverProperties>();
        LogInfo("Selected GPU: {} (driver: {}, ICD: {}, API version {}.{}.{})",
                static_cast<const char*>(DevProps.deviceName),
                static_cast<const char*>(Driver.driverName),
                vk::to_string(Driver.driverID),
                VK_API_VERSION_MAJOR(DevProps.apiVersion),
                VK_API_VERSION_MINOR(DevProps.apiVersion),
                VK_API_VERSION_PATCH(DevProps.apiVersion));

        auto QueueProps = m_PhysicalDevice.getQueueFamilyProperties2();
        return ResolveQueueFamilies(QueueProps);
    }

    [[nodiscard]] auto ResolveQueueFamilies(std::span<const vk::QueueFamilyProperties2> QueueProps)
        -> std::expected<void, ErrorMessage> {
        for (std::size_t i = 0; i < QueueProps.size(); ++i) {
            LogDebug("Queue family [{}]: count={}, flags={}",
                     i,
                     QueueProps[i].queueFamilyProperties.queueCount,
                     vk::to_string(QueueProps[i].queueFamilyProperties.queueFlags));
        }

        // Graphics + Present
        {
            for (std::size_t i = 0; i < QueueProps.size(); ++i) {
                if ((QueueProps[i].queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) !=
                    vk::QueueFlags{}) {
                    auto [Res, Supported] = m_PhysicalDevice.getSurfaceSupportKHR(static_cast<Uint32>(i), m_Surface);
                    if (Res == vk::Result::eSuccess && Supported) {
                        m_GraphicsFamily = static_cast<Uint32>(i);
                        break;
                    }
                }
            }
        }
        if (m_GraphicsFamily == vk::QueueFamilyIgnored)
            return std::unexpected(ErrorMessage("No queue family supports both graphics and presentation"));

        // Compute
        auto ComputeIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
            return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{};
        });
        if (ComputeIt == QueueProps.end())
            ComputeIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
                return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{};
            });
        if (ComputeIt == QueueProps.end())
            return std::unexpected(ErrorMessage("No compute-capable queue family found"));
        m_ComputeFamily = static_cast<Uint32>(std::distance(QueueProps.begin(), ComputeIt));

        // Transfer
        auto TransferIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
            return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{} &&
                   (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eCompute) == vk::QueueFlags{};
        });
        if (TransferIt == QueueProps.end())
            TransferIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
                return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{} &&
                       (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{};
            });
        if (TransferIt == QueueProps.end())
            TransferIt = std::ranges::find_if(QueueProps, [](const auto& QFP) {
                return (QFP.queueFamilyProperties.queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{};
            });
        if (TransferIt == QueueProps.end())
            return std::unexpected(ErrorMessage("No transfer-capable queue family found"));
        m_TransferFamily = static_cast<Uint32>(std::distance(QueueProps.begin(), TransferIt));

        LogInfo("Queue families resolved: graphics={}, compute={}, transfer={}",
                m_GraphicsFamily,
                m_ComputeFamily,
                m_TransferFamily);
        return {};
    }

    [[nodiscard]] auto CreateLogicalDevice() -> std::expected<void, ErrorMessage> {
        std::vector<Uint32> UniqueFamilies;
        auto                AddUnique = [&](Uint32 Family) {
            if (std::ranges::find(UniqueFamilies, Family) == UniqueFamilies.end())
                UniqueFamilies.push_back(Family);
        };
        AddUnique(m_GraphicsFamily);
        AddUnique(m_ComputeFamily);
        AddUnique(m_TransferFamily);

        float                                  QueuePriority = 1.0f;
        std::vector<vk::DeviceQueueCreateInfo> QueueCIs;
        for (Uint32 Family : UniqueFamilies)
            QueueCIs.push_back(vk::DeviceQueueCreateInfo{
                .queueFamilyIndex = Family,
                .queueCount       = 1,
                .pQueuePriorities = &QueuePriority,
            });

        auto EnabledDeviceExts = VulkanCapability::Get().ResolveDeviceExtensionsAndFeatures(m_PhysicalDevice);
        if (!EnabledDeviceExts)
            return std::unexpected(EnabledDeviceExts.error());
        auto& [DevExts, FeatsChain] = *EnabledDeviceExts;
        vk::DeviceCreateInfo DevCI{
            .pNext                   = &FeatsChain.get<vk::PhysicalDeviceFeatures2>(),
            .queueCreateInfoCount    = static_cast<Uint32>(QueueCIs.size()),
            .pQueueCreateInfos       = QueueCIs.data(),
            .enabledExtensionCount   = static_cast<Uint32>(DevExts.size()),
            .ppEnabledExtensionNames = DevExts.data(),
            .pEnabledFeatures        = nullptr,
        };

        auto DevResult = m_PhysicalDevice.createDevice(DevCI);
        if (DevResult.result != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Format("Failed to create logical device: {}", vk::to_string(DevResult.result))));
        m_Device = std::move(DevResult.value);
        m_DebugUtils.Initialize(m_Device);
        m_DebugUtils.SetObjectName(*m_Instance, "Internal/Instance");
        m_DebugUtils.SetObjectName(*m_Surface, "Internal/Surface");
        m_DebugUtils.SetObjectName(*m_PhysicalDevice, "Internal/PhysicalDevice");
        m_DebugUtils.SetObjectName(*m_Device, "Internal/Device");

        const auto& V13 = VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan13Features>();
        if (!V13.synchronization2)
            return std::unexpected(ErrorMessage("synchronization2 feature not supported by device"));
        if (!V13.dynamicRendering)
            return std::unexpected(ErrorMessage("dynamicRendering feature not supported by device"));

        m_GraphicsQueue = m_Device.getQueue(m_GraphicsFamily, 0);
        m_ComputeQueue  = m_Device.getQueue(m_ComputeFamily, 0);
        m_TransferQueue = m_Device.getQueue(m_TransferFamily, 0);
        if (m_GraphicsFamily == m_ComputeFamily && m_GraphicsFamily == m_TransferFamily) {
            m_DebugUtils.SetObjectName(*m_GraphicsQueue, "Internal/Queue/Graphics&Transfer&Compute");
        } else if (m_GraphicsFamily == m_ComputeFamily) {
            m_DebugUtils.SetObjectName(*m_GraphicsQueue, "Internal/Queue/Graphics&Compute");
            m_DebugUtils.SetObjectName(*m_TransferQueue, "Internal/Queue/Transfer");
        } else if (m_GraphicsFamily == m_TransferFamily) {
            m_DebugUtils.SetObjectName(*m_GraphicsQueue, "Internal/Queue/Graphics&Transfer");
            m_DebugUtils.SetObjectName(*m_ComputeQueue, "Internal/Queue/Compute");
        } else if (m_ComputeFamily == m_TransferFamily) {
            m_DebugUtils.SetObjectName(*m_GraphicsQueue, "Internal/Queue/Graphics");
            m_DebugUtils.SetObjectName(*m_ComputeQueue, "Internal/Queue/Transfer&Compute");
        } else {
            m_DebugUtils.SetObjectName(*m_GraphicsQueue, "Internal/Queue/Graphics");
            m_DebugUtils.SetObjectName(*m_ComputeQueue, "Internal/Queue/Compute");
            m_DebugUtils.SetObjectName(*m_TransferQueue, "Internal/Queue/Transfer");
        }

        VulkanCapability::Get().ResolveDeviceProperties(m_PhysicalDevice);
        return {};
    }

    [[nodiscard]] auto CreateVMA(vk::raii::Context& Context) -> std::expected<void, ErrorMessage> {
        const auto&        CtxDispatcher  = Context.getDispatcher();
        const auto&        InstDispatcher = m_Instance.getDispatcher();
        VmaVulkanFunctions VmaVF{
            .vkGetInstanceProcAddr = CtxDispatcher->vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr   = InstDispatcher->vkGetDeviceProcAddr,
        };
        VmaAllocatorCreateInfo VmaInfo{
            .physicalDevice   = static_cast<VkPhysicalDevice>(*m_PhysicalDevice),
            .device           = static_cast<VkDevice>(*m_Device),
            .pVulkanFunctions = &VmaVF,
            .instance         = static_cast<VkInstance>(*m_Instance),
            .vulkanApiVersion = VK_API_VERSION_1_4,
        };
        if (VulkanCapability::Get().IsDeviceExtensionEnabled(vk::EXTMemoryBudgetExtensionName))
            VmaInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        if (VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress)
            VmaInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        VkResult Result = vmaCreateAllocator(&VmaInfo, &m_Allocator);
        if (Result != VK_SUCCESS)
            return std::unexpected(ErrorMessage(
                Format("Failed to create VMA allocator: {}", vk::to_string(static_cast<vk::Result>(Result)))));
        return {};
    }

    // Global descriptor manager
    UPtr<IVulkanSurfaceProvider> m_SurfaceProvider = nullptr;
    vk::raii::Instance           m_Instance        = nullptr;
    vk::raii::SurfaceKHR         m_Surface         = nullptr;
    vk::raii::PhysicalDevice     m_PhysicalDevice  = nullptr;
    vk::raii::Device             m_Device          = nullptr;
    tracy::VkCtx*                m_TracyCtx        = nullptr;
    VulkanDebugUtils             m_DebugUtils;
    Uint32                       m_GraphicsFamily = vk::QueueFamilyIgnored;
    Uint32                       m_ComputeFamily  = vk::QueueFamilyIgnored;
    Uint32                       m_TransferFamily = vk::QueueFamilyIgnored;
    vk::raii::Queue              m_GraphicsQueue  = nullptr;
    vk::raii::Queue              m_ComputeQueue   = nullptr;
    vk::raii::Queue              m_TransferQueue  = nullptr;
    VmaAllocator                 m_Allocator      = nullptr;
    VulkanImageTracker           m_ImageTracker;
    VulkanBufferTracker          m_BufferTracker;
    VulkanTimelineSemaphore      m_Timeline;
    VulkanDeferredDeletionQueue  m_DeferredDeletionQueue;
    Uint32                       m_FramesInFlight    = 2;
    Uint32                       m_CurrentFrameIndex = 0;
};

} // namespace SoulEngine
