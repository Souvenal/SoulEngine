module;

export module Vulkan:Semaphore;

import Core;
import :Capability;

import vulkan;
import std;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// TimelineSemaphore
// ═════════════════════════════════════════════════════════════════════════════

/// Thin wrapper around a single per-device VkSemaphore of type
/// VK_SEMAPHORE_TYPE_TIMELINE. Owns the monotonic CPU signal counter and
/// provides blocking CPU waits / non-blocking completion queries.
class TimelineSemaphore {
  public:
    TimelineSemaphore() = default;

    TimelineSemaphore(TimelineSemaphore&& Other) noexcept
        : m_Device(std::exchange(Other.m_Device, nullptr)),
          m_Semaphore(std::move(Other.m_Semaphore)),
          m_NextValue(Other.m_NextValue.load()) {}

    auto operator=(TimelineSemaphore&& Other) noexcept -> TimelineSemaphore& {
        if (this != &Other) {
            m_Device    = std::exchange(Other.m_Device, nullptr);
            m_Semaphore = std::move(Other.m_Semaphore);
            m_NextValue.store(Other.m_NextValue.load());
        }
        return *this;
    }

    TimelineSemaphore(const TimelineSemaphore&)                    = delete;
    auto operator=(const TimelineSemaphore&) -> TimelineSemaphore& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device& Device) -> std::expected<TimelineSemaphore, ErrorMessage> {
        TimelineSemaphore Result;
        Result.m_Device = &Device;

        if (!Capability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>().timelineSemaphore)
            return std::unexpected(
                ErrorMessage("TimelineSemaphore: timelineSemaphore feature not supported by device"));

        vk::StructureChain<vk::SemaphoreCreateInfo, vk::SemaphoreTypeCreateInfo> Chain = {
            {}, {.semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0}};
        auto Res = Device.createSemaphore(Chain.get<vk::SemaphoreCreateInfo>());
        if (Res.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create timeline semaphore"));
        Result.m_Semaphore = std::move(Res.value);
        return Result;
    }

    /// Advances the monotonic counter and returns the new value.
    /// Callers pass this value to queue submit as the signal value, then
    /// retain it to check completion later.
    [[nodiscard]] auto NextValue() noexcept -> Uint64 {
        return ++m_NextValue;
    }

    /// Returns semaphore's current GPU-side counter value (non-blocking).
    /// Calls vkGetSemaphoreCounterValue.
    [[nodiscard]] auto GetCurrentValue() -> std::expected<Uint64, ErrorMessage> {
        auto Res = m_Semaphore.getCounterValue();
        if (Res.result != vk::Result::eSuccess) {
            return std::unexpected(
                ErrorMessage(Core::Format("vkGetSemaphoreCounterValue failed: {}", vk::to_string(Res.result))));
        }
        return Res.value;
    }

    /// Blocks CPU until semaphore reaches at least `Value`.
    /// Calls vkWaitSemaphores with the given timeout (default: infinite).
    [[nodiscard]] auto Wait(Uint64 Value, Uint64 TimeoutNs = std::numeric_limits<Uint64>::max())
        -> std::expected<void, ErrorMessage> {
        vk::Semaphore         Sem = *m_Semaphore;
        vk::SemaphoreWaitInfo WaitInfo{
            .semaphoreCount = 1,
            .pSemaphores    = &Sem,
            .pValues        = &Value,
        };
        if (auto R = m_Device->waitSemaphores(WaitInfo, TimeoutNs); R != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Core::Format("vkWaitSemaphores failed: {}", vk::to_string(R))));
        }
        return {};
    }

    /// Returns a SemaphoreSubmitInfo for the next timeline value.
    /// Calls NextValue internally.
    [[nodiscard]] auto GetSignalSubmitInfo(vk::PipelineStageFlagBits2 Stage = vk::PipelineStageFlagBits2::eNone)
        -> vk::SemaphoreSubmitInfo {
        return vk::SemaphoreSubmitInfo{
            .semaphore = *m_Semaphore,
            .value     = NextValue(),
            .stageMask = Stage,
        };
    }

    [[nodiscard]] auto Get() const noexcept -> vk::Semaphore {
        return *m_Semaphore;
    }

  private:
    vk::raii::Device*   m_Device    = nullptr;
    vk::raii::Semaphore m_Semaphore = nullptr;
    std::atomic<Uint64> m_NextValue = 0;
};

} // namespace SoulEngine::RHI::Vulkan
