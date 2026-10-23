module;

// volk.h must precede every other Vulkan header in this fragment: it defines VK_NO_PROTOTYPES itself before
// pulling in vulkan_core.h, and errors out if vulkan.h was already included without it. Dropping the raw C
// prototypes is safe here — every call goes through Vulkan-Hpp's dynamic dispatcher or VMA's dynamically
// fetched function table.
#include <volk.h>
#include <vk_mem_alloc.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

module RHIVulkan.Context;

import Core;
import vulkan;
import std;

import RHIVulkan.Capability;
import RHIVulkan.Debug;
import RHIVulkan.SurfaceProvider;
import RHIVulkan.Semaphore;
import WindowSystem;

namespace SoulEngine {

auto VulkanImageSubresourceKeyHash::operator()(const VulkanImageSubresourceKey& Key) const -> std::size_t {
    std::size_t Hash    = std::hash<VkImage>{}(static_cast<VkImage>(Key.Image));
    auto        Combine = [&Hash](Uint32 Value) {
        Hash ^= std::hash<Uint32>{}(Value) + 0x9e3779b9U + (Hash << 6U) + (Hash >> 2U);
    };
    Combine(std::to_underlying(Key.Aspect));
    Combine(Key.MipLevel);
    Combine(Key.ArrayLayer);
    return Hash;
}

auto VulkanImageTracker::Seed(vk::Image Image, Uint32 MipLevels, Uint32 ArrayLayers, VulkanImageState State) -> void {
    for (Uint32 Mip = 0; Mip < MipLevels; ++Mip)
        for (Uint32 Layer = 0; Layer < ArrayLayers; ++Layer)
            m_SubresourceStates[VulkanImageSubresourceKey{
                .Image = Image, .Aspect = vk::ImageAspectFlagBits::eColor, .MipLevel = Mip, .ArrayLayer = Layer}] =
                State;
}

auto VulkanImageTracker::Forget(vk::Image Image) -> void {
    for (auto It = m_SubresourceStates.begin(); It != m_SubresourceStates.end();) {
        if (It->first.Image == Image)
            It = m_SubresourceStates.erase(It);
        else
            ++It;
    }
}

auto VulkanImageTracker::Transition(const vk::raii::CommandBuffer&  CmdBuf,
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
                const auto             It      = m_SubresourceStates.find(Key);
                const VulkanImageState Current = (It != m_SubresourceStates.end()) ? It->second : VulkanImageState{};

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

auto VulkanBufferTracker::Transition(vk::raii::CommandBuffer& CmdBuf,
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

auto VulkanBufferTracker::KeyHash::operator()(const Key& K) const noexcept -> std::size_t {
    const std::size_t BufferHash = std::hash<VkBuffer>{}(static_cast<VkBuffer>(K.Buffer));
    return BufferHash ^ (std::hash<vk::DeviceSize>{}(K.Offset) + 0x9e3779b9 + (BufferHash << 6) + (BufferHash >> 2));
}

auto VulkanDeferredDeletionQueue::Init(VmaAllocator Allocator, VulkanTimelineSemaphore& Timeline) -> void {
    m_Allocator = Allocator;
    m_Timeline  = &Timeline;
}

auto VulkanDeferredDeletionQueue::Tick() -> void {
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

auto VulkanDeferredDeletionQueue::Flush() -> void {
    if (!m_Allocator)
        return;
    for (const BufferEntry& E : m_BufferEntries)
        vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(E.Buffer), E.Allocation);
    m_BufferEntries.clear();
    for (const ImageEntry& E : m_ImageEntries)
        vmaDestroyImage(m_Allocator, static_cast<VkImage>(E.Image), E.Allocation);
    m_ImageEntries.clear();
}

VulkanResourceContext::~VulkanResourceContext() {
    DestroyTracyCtx();
    if (m_Allocator)
        vmaDestroyAllocator(m_Allocator);
}

auto VulkanResourceContext::Create(IWindowSystem* WindowSys, Uint32 FramesInFlight)
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
        return std::unexpected(ErrorMessage(
            Format("volk failed to load the vulkan loader: {}", vk::to_string(static_cast<vk::Result>(VolkResult)))));
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

    auto Timeline =
        VulkanTimelineSemaphore::Create(Result->m_Device, &Result->m_DebugUtils, "Internal/Semaphore/GraphicsTimeline");
    if (!Timeline)
        return std::unexpected(Timeline.error().Append("Vulkan graphics timeline creation failed"));
    Result->m_Timeline = std::move(*Timeline);

    if (auto Res = Result->CreateVMA(Context); !Res)
        return std::unexpected(Res.error());
    Result->m_DeferredDeletionQueue.Init(Result->m_Allocator, Result->m_Timeline);

    Result->CreateTracyCtx();

    return Result;
}

auto VulkanResourceContext::CreateTracyCtx() -> void {
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

auto VulkanResourceContext::DestroyTracyCtx() noexcept -> void {
    if (!m_TracyCtx)
        return;
    tracy::DestroyVkContext(m_TracyCtx);
    m_TracyCtx = nullptr;
}

auto VulkanResourceContext::CreateInstance(vk::raii::Context& Context) -> std::expected<void, ErrorMessage> {
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

    auto EnabledInstanceExts = VulkanCapability::Get().ResolveInstanceExtensions(Context, *RequiredInstanceExtensions);
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

auto VulkanResourceContext::PickPhysicalDevice() -> std::expected<void, ErrorMessage> {
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

auto VulkanResourceContext::ResolveQueueFamilies(std::span<const vk::QueueFamilyProperties2> QueueProps)
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
            if ((QueueProps[i].queueFamilyProperties.queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{}) {
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

auto VulkanResourceContext::CreateLogicalDevice() -> std::expected<void, ErrorMessage> {
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

auto VulkanResourceContext::CreateVMA(vk::raii::Context& Context) -> std::expected<void, ErrorMessage> {
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
        return std::unexpected(
            ErrorMessage(Format("Failed to create VMA allocator: {}", vk::to_string(static_cast<vk::Result>(Result)))));
    return {};
}

} // namespace SoulEngine
