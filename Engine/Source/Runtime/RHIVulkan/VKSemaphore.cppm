module;

export module RHIVulkan.Semaphore;

import Core;
import RHIVulkan.Capability;
import RHIVulkan.Debug;

import vulkan;
import std;

namespace SoulEngine {

// VulkanTimelineSemaphore

/// @brief Thin wrapper around a single per-device VkSemaphore of type VK_SEMAPHORE_TYPE_TIMELINE.
///
/// Owns the monotonic CPU signal counter and provides blocking CPU waits / non-blocking completion queries.
export class VulkanTimelineSemaphore {
  public:
    VulkanTimelineSemaphore() = default;

    VulkanTimelineSemaphore(VulkanTimelineSemaphore&& Other) noexcept;

    auto operator=(VulkanTimelineSemaphore&& Other) noexcept -> VulkanTimelineSemaphore&;

    VulkanTimelineSemaphore(const VulkanTimelineSemaphore&)                    = delete;
    auto operator=(const VulkanTimelineSemaphore&) -> VulkanTimelineSemaphore& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device& Device, VulkanDebugUtils* DebugUtils, StringView Name)
        -> std::expected<VulkanTimelineSemaphore, ErrorMessage>;

    /// @brief Advance the host-side monotonic counter and return the new value.
    ///
    /// The device-side semaphore reaches this value only after a submission explicitly signals it.
    [[nodiscard]] auto IncreaseHostValue() noexcept -> Uint64;

    /// Returns the device-side semaphore counter value (non-blocking). Calls vkGetSemaphoreCounterValue.
    [[nodiscard]] auto GetDeviceValue() -> Uint64;

    /// @brief Block the CPU until the semaphore reaches at least `Value`.
    ///
    /// Calls vkWaitSemaphores with the given timeout (default: infinite).
    auto Wait(Uint64 Value, Uint64 TimeoutNs = std::numeric_limits<Uint64>::max()) -> void;

    /// Returns a SemaphoreSubmitInfo for the next timeline value. Calls IncreaseHostValue internally.
    [[nodiscard]] auto GetSignalSubmitInfo(vk::PipelineStageFlagBits2 Stage = vk::PipelineStageFlagBits2::eNone)
        -> vk::SemaphoreSubmitInfo;

    [[nodiscard]] auto Get() const noexcept -> vk::Semaphore;

  private:
    vk::raii::Device*   m_Device    = nullptr;
    vk::raii::Semaphore m_Semaphore = nullptr;
    std::atomic<Uint64> m_HostValue = 0;
};

} // namespace SoulEngine
