/// @file   Vulkan/VKContext.cppm
/// @brief  Vulkan device services and shared image-state tracking.

module;

#include <vk_mem_alloc.h>

export module Vulkan:Context;

import Core;
import vulkan;
import std;

import :Capability;
import :Debug;
import :ImmediateContext;
import :SurfaceProvider;
import :Semaphore;

export namespace SoulEngine {

/// @brief Tracked Vulkan image state used for automatic synchronization barriers.
struct VulkanImageState {
    vk::PipelineStageFlags2 stage       = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access      = vk::AccessFlagBits2::eNone;
    vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
    vk::ImageAspectFlags    aspect      = vk::ImageAspectFlagBits::eColor;
    Uint32                  queueFamily = vk::QueueFamilyIgnored;
    bool                    isWrite     = false;
};

/// @brief Device-level image state tracker shared by Vulkan command recording.
class VulkanImageTracker {
  public:
    VulkanImageTracker() = default;

    auto Register(vk::Image Image, VulkanImageState State = {}) -> void {
        m_ImageStates[Image] = State;
    }

    auto Transition(vk::raii::CommandBuffer& CmdBuf,
                    vk::Image                Image,
                    const VulkanImageState&  DesiredState) -> void {
        auto It      = m_ImageStates.find(Image);
        auto Current = (It != m_ImageStates.end()) ? It->second : VulkanImageState{};

        const bool NeedsBarrier =
            (Current.stage != DesiredState.stage) || (Current.access != DesiredState.access) ||
            (Current.layout != DesiredState.layout) || Current.isWrite;

        if (NeedsBarrier) {
            vk::ImageMemoryBarrier2 Barrier{
                .srcStageMask        = Current.stage,
                .srcAccessMask       = Current.access,
                .dstStageMask        = DesiredState.stage,
                .dstAccessMask       = DesiredState.access,
                .oldLayout           = Current.layout,
                .newLayout           = DesiredState.layout,
                .srcQueueFamilyIndex = Current.queueFamily,
                .dstQueueFamilyIndex = DesiredState.queueFamily,
                .image               = Image,
                .subresourceRange    = {.aspectMask     = DesiredState.aspect,
                                        .baseMipLevel   = 0,
                                        .levelCount     = vk::RemainingMipLevels,
                                        .baseArrayLayer = 0,
                                        .layerCount     = vk::RemainingArrayLayers},
            };
            vk::DependencyInfo Dependency{
                .dependencyFlags         = vk::DependencyFlagBits::eByRegion,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers    = &Barrier,
            };
            CmdBuf.pipelineBarrier2(Dependency);
        }

        m_ImageStates[Image] = DesiredState;
    }

  private:
    std::unordered_map<vk::Image, VulkanImageState> m_ImageStates = {};
};

/// Owns Vulkan objects and services shared by backend resource factories.
class VulkanResourceContext final {
  public:
    VulkanResourceContext() = default;

    ~VulkanResourceContext() {
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

        // vk::raii::Context does:
        // 1. get `vkGetInstanceProcAddr`, whether dynamically or statically
        //    (dynamically means mechanism like dlopen)
        // 2. using `vkGetInstanceProcAddr` to collect all Instance Functions
        //
        // Once an instance is created, the instance will hold `vkGetInstanceProcAddr`,
        // thus the Context can be locally assigned.
        //
        // A `DynamicLoader` is hidden here, which searches for the loader from variable paths.
        //
        // TODO: Seg fault happens when loader is missing.
        //       Consider using `volk` instead.
        vk::raii::Context Context;
        if (auto Res = Result->CreateInstance(Context); !Res)
            return std::unexpected(Res.error());

        // Surface must exist before PickPhysicalDevice so we can verify
        // surface presentation support via getSurfaceSupportKHR.
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

        // ── VMA ───────────────────────────────────────────────────────────
        if (auto Res = Result->CreateVMA(Context); !Res)
            return std::unexpected(Res.error());

        // ── Immediate Context ──────────────────────────────────────────────
        auto Immediate = VulkanImmediateContext::Create(
            Result->m_Device,
            Result->m_DebugUtils,
            Result->m_TransferQueue,
            Result->m_TransferFamily,
            Result->m_GraphicsQueue,
            Result->m_GraphicsFamily);
        if (!Immediate)
            return std::unexpected(Immediate.error().Append("VulkanImmediateContext creation failed"));
        Result->m_ImmediateContext = std::move(*Immediate);

        return Result;
    }

    [[nodiscard]] auto GetInstance() const -> vk::raii::Instance& { return const_cast<vk::raii::Instance&>(m_Instance); }
    [[nodiscard]] auto GetDebugMessenger() const -> vk::raii::DebugUtilsMessengerEXT& {
        return const_cast<vk::raii::DebugUtilsMessengerEXT&>(m_DebugMessenger);
    }
    [[nodiscard]] auto GetSurface() const -> vk::raii::SurfaceKHR& { return const_cast<vk::raii::SurfaceKHR&>(m_Surface); }
    [[nodiscard]] auto GetPhysicalDevice() const -> vk::raii::PhysicalDevice& {
        return const_cast<vk::raii::PhysicalDevice&>(m_PhysicalDevice);
    }
    [[nodiscard]] auto GetDevice() const -> vk::raii::Device& { return const_cast<vk::raii::Device&>(m_Device); }
    [[nodiscard]] auto GetDebugUtils() const -> VulkanDebugUtils& { return const_cast<VulkanDebugUtils&>(m_DebugUtils); }
    [[nodiscard]] auto GetGraphicsQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_GraphicsQueue);
    }
    [[nodiscard]] auto GetComputeQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_ComputeQueue);
    }
    [[nodiscard]] auto GetTransferQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_TransferQueue);
    }
    [[nodiscard]] auto GetImmediateContext() const -> VulkanImmediateContext& {
        return const_cast<VulkanImmediateContext&>(m_ImmediateContext);
    }
    [[nodiscard]] auto GetImageTracker() const -> VulkanImageTracker& {
        return const_cast<VulkanImageTracker&>(m_ImageTracker);
    }
    [[nodiscard]] auto GetAllocator() const -> VmaAllocator { return m_Allocator; }
    [[nodiscard]] auto GetSurfaceProvider() const -> IVulkanSurfaceProvider& { return *m_SurfaceProvider; }
    [[nodiscard]] auto GetGraphicsFamily() const -> Uint32 { return m_GraphicsFamily; }
    [[nodiscard]] auto GetComputeFamily() const -> Uint32 { return m_ComputeFamily; }
    [[nodiscard]] auto GetTransferFamily() const -> Uint32 { return m_TransferFamily; }
    [[nodiscard]] auto GetFramesInFlight() const -> Uint32 { return m_FramesInFlight; }
    [[nodiscard]] auto GetTimeline() const -> VulkanTimelineSemaphore& {
        return const_cast<VulkanTimelineSemaphore&>(m_Timeline);
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
        const bool DebugUtils = ConfigManager::Get().GetConfig().RhiVulkan.DebugUtils.value_or(true);

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

        if (DebugUtils) {
            auto DebugMessengerCI = CreateDebugMessengerCI();
            vk::StructureChain<vk::InstanceCreateInfo, vk::DebugUtilsMessengerCreateInfoEXT> InstanceChain{
                InstCI,
                DebugMessengerCI,
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

        if (DebugUtils) {
            auto DebugMessenger = m_Instance.createDebugUtilsMessengerEXT(CreateDebugMessengerCI(), nullptr);
            if (DebugMessenger.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage(
                    Format("Failed to create Vulkan debug messenger: {}", vk::to_string(DebugMessenger.result))));
            m_DebugMessenger = std::move(DebugMessenger.value);
        }
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
        auto AddUnique = [&](Uint32 Family) {
            if (std::ranges::find(UniqueFamilies, Family) == UniqueFamilies.end())
                UniqueFamilies.push_back(Family);
        };
        AddUnique(m_GraphicsFamily);
        AddUnique(m_ComputeFamily);
        AddUnique(m_TransferFamily);

        float QueuePriority = 1.0f;
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
        const auto& CtxDispatcher  = Context.getDispatcher();
        const auto& InstDispatcher = m_Instance.getDispatcher();
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

    [[nodiscard]] static auto CreateDebugMessengerCI() -> vk::DebugUtilsMessengerCreateInfoEXT {
        return vk::DebugUtilsMessengerCreateInfoEXT{
            .messageSeverity =
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
            .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                           vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
            .pfnUserCallback = &VulkanDebugCallback,
        };
    }

    static VKAPI_ATTR auto VKAPI_CALL VulkanDebugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT      MessageSeverity,
                                                           vk::DebugUtilsMessageTypeFlagsEXT             MessageTypes,
                                                           const vk::DebugUtilsMessengerCallbackDataEXT* CallbackData,
                                                           void*) -> vk::Bool32 {
        if (!CallbackData) {
            LogError("[Vulkan] Debug callback data is empty");
            return vk::True;
        }
        const StringView MessageId = CallbackData->pMessageIdName ? CallbackData->pMessageIdName : "UnknownMessage";
        const StringView Message   = CallbackData->pMessage ? CallbackData->pMessage : "No Vulkan debug message";
        const auto Types = vk::to_string(MessageTypes);
        String DetailedMessage{Message};
        if (CallbackData->objectCount > 0) {
            DetailedMessage += Format("\nObjects: {}", CallbackData->objectCount);
            for (Uint32 Index = 0; Index < CallbackData->objectCount; ++Index) {
                const auto& Object = CallbackData->pObjects[Index];
                DetailedMessage +=
                    Format("\n    [{}] Vk{} 0x{:x}", Index, vk::to_string(Object.objectType), Object.objectHandle);
                if (Object.pObjectName)
                    DetailedMessage += Format("[{}]", Object.pObjectName);
            }
        }
        switch (MessageSeverity) {
        case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
            LogDebug("[Vulkan][{}][{}] {}", Types, MessageId, DetailedMessage);
            break;
        case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
            // There are too much `eInfo` messages,
            // so we use `LogDebug`.
            LogDebug("[Vulkan][{}][{}] {}", Types, MessageId, DetailedMessage);
            break;
        case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
            LogWarning("[Vulkan][{}][{}] {}", Types, MessageId, DetailedMessage);
            break;
        case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
            LogError("[Vulkan][{}][{}] {}", Types, MessageId, DetailedMessage);
            break;
        }
        return vk::False;
    }

    UPtr<IVulkanSurfaceProvider>     m_SurfaceProvider = nullptr;
    vk::raii::Instance               m_Instance        = nullptr;
    vk::raii::DebugUtilsMessengerEXT m_DebugMessenger  = nullptr;
    vk::raii::SurfaceKHR             m_Surface         = nullptr;
    vk::raii::PhysicalDevice         m_PhysicalDevice  = nullptr;
    vk::raii::Device                 m_Device          = nullptr;
    VulkanDebugUtils                 m_DebugUtils;
    Uint32                           m_GraphicsFamily = vk::QueueFamilyIgnored;
    Uint32                           m_ComputeFamily  = vk::QueueFamilyIgnored;
    Uint32                           m_TransferFamily = vk::QueueFamilyIgnored;
    vk::raii::Queue                  m_GraphicsQueue  = nullptr;
    vk::raii::Queue                  m_ComputeQueue   = nullptr;
    vk::raii::Queue                  m_TransferQueue  = nullptr;
    VmaAllocator                     m_Allocator      = nullptr;
    VulkanImmediateContext           m_ImmediateContext;
    // ── Global descriptor manager ─────────────────────────────────────────
    VulkanImageTracker               m_ImageTracker;
    VulkanTimelineSemaphore           m_Timeline;
    Uint32                           m_FramesInFlight = 2;
};

} // namespace SoulEngine
