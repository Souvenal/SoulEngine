module;

export module Vulkan:Semaphore;

import Core;
import :Capability;
import :Debug;

import vulkan;
import std;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// VulkanTimelineSemaphore
// ═════════════════════════════════════════════════════════════════════════════

/// Thin wrapper around a single per-device VkSemaphore of type
/// VK_SEMAPHORE_TYPE_TIMELINE. Owns the monotonic CPU signal counter and
/// provides blocking CPU waits / non-blocking completion queries.
class VulkanTimelineSemaphore {
  public:
    VulkanTimelineSemaphore() = default;

    VulkanTimelineSemaphore(VulkanTimelineSemaphore&& Other) noexcept
        : m_Device(std::exchange(Other.m_Device, nullptr)),
          m_Semaphore(std::move(Other.m_Semaphore)),
          m_HostValue(Other.m_HostValue.load()) {}

    auto operator=(VulkanTimelineSemaphore&& Other) noexcept -> VulkanTimelineSemaphore& {
        if (this != &Other) {
            m_Device    = std::exchange(Other.m_Device, nullptr);
            m_Semaphore = std::move(Other.m_Semaphore);
            m_HostValue.store(Other.m_HostValue.load());
        }
        return *this;
    }

    VulkanTimelineSemaphore(const VulkanTimelineSemaphore&)                    = delete;
    auto operator=(const VulkanTimelineSemaphore&) -> VulkanTimelineSemaphore& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device& Device,
                                     VulkanDebugUtils* DebugUtils,
                                     StringView         Name)
        -> std::expected<VulkanTimelineSemaphore, ErrorMessage> {
        VulkanTimelineSemaphore Result;
        Result.m_Device = &Device;

        if (!VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>().timelineSemaphore)
            return std::unexpected(
                ErrorMessage("VulkanTimelineSemaphore: timelineSemaphore feature not supported by device"));

        vk::StructureChain<vk::SemaphoreCreateInfo, vk::SemaphoreTypeCreateInfo> Chain = {
            {}, {.semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0}};
        auto Res = Device.createSemaphore(Chain.get<vk::SemaphoreCreateInfo>());
        if (Res.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create timeline semaphore"));
        if (DebugUtils)
            DebugUtils->SetObjectName(*Res.value, Name);
        Result.m_Semaphore = std::move(Res.value);
        return Result;
    }

    /// Advances the host-side monotonic counter and returns the new value.
    /// The device-side semaphore reaches this value only after a submission
    /// explicitly signals it.
    [[nodiscard]] auto IncreaseHostValue() noexcept -> Uint64 {
        return ++m_HostValue;
    }

    /// Returns the device-side semaphore counter value (non-blocking).
    /// Calls vkGetSemaphoreCounterValue.
    [[nodiscard]] auto GetDeviceValue() -> Uint64 {
        auto Res = m_Semaphore.getCounterValue();
        if (Res.result != vk::Result::eSuccess) {
            LogError("vkGetSemaphoreCounterValue failed: {}", vk::to_string(Res.result));
            return 0;
        }
        return Res.value;
    }

    /// Blocks CPU until semaphore reaches at least `Value`.
    /// Calls vkWaitSemaphores with the given timeout (default: infinite).
    auto Wait(Uint64 Value, Uint64 TimeoutNs = std::numeric_limits<Uint64>::max()) -> void {
        vk::Semaphore         Sem = *m_Semaphore;
        vk::SemaphoreWaitInfo WaitInfo{
            .semaphoreCount = 1,
            .pSemaphores    = &Sem,
            .pValues        = &Value,
        };
        if (auto R = m_Device->waitSemaphores(WaitInfo, TimeoutNs); R != vk::Result::eSuccess) {
            LogError("vkWaitSemaphores failed: {}", vk::to_string(R));
        }
    }

    /// Returns a SemaphoreSubmitInfo for the next timeline value.
    /// Calls IncreaseHostValue internally.
    [[nodiscard]] auto GetSignalSubmitInfo(vk::PipelineStageFlagBits2 Stage = vk::PipelineStageFlagBits2::eNone)
        -> vk::SemaphoreSubmitInfo {
        return vk::SemaphoreSubmitInfo{
            .semaphore = *m_Semaphore,
            .value     = IncreaseHostValue(),
            .stageMask = Stage,
        };
    }

    [[nodiscard]] auto Get() const noexcept -> vk::Semaphore {
        return *m_Semaphore;
    }

  private:
    vk::raii::Device*   m_Device    = nullptr;
    vk::raii::Semaphore m_Semaphore = nullptr;
    std::atomic<Uint64> m_HostValue = 0;
};

} // namespace SoulEngine
