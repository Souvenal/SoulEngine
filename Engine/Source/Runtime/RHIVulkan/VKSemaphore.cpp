module RHIVulkan.Semaphore;

import Core;
import RHIVulkan.Capability;
import RHIVulkan.Debug;

import vulkan;
import std;

namespace SoulEngine {

VulkanTimelineSemaphore::VulkanTimelineSemaphore(VulkanTimelineSemaphore&& Other) noexcept
    : m_Device(std::exchange(Other.m_Device, nullptr)),
      m_Semaphore(std::move(Other.m_Semaphore)),
      m_HostValue(Other.m_HostValue.load()) {}

auto VulkanTimelineSemaphore::operator=(VulkanTimelineSemaphore&& Other) noexcept -> VulkanTimelineSemaphore& {
    if (this != &Other) {
        m_Device    = std::exchange(Other.m_Device, nullptr);
        m_Semaphore = std::move(Other.m_Semaphore);
        m_HostValue.store(Other.m_HostValue.load());
    }
    return *this;
}

auto VulkanTimelineSemaphore::Create(vk::raii::Device& Device, VulkanDebugUtils* DebugUtils, StringView Name)
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

auto VulkanTimelineSemaphore::IncreaseHostValue() noexcept -> Uint64 {
    return ++m_HostValue;
}

auto VulkanTimelineSemaphore::GetDeviceValue() -> Uint64 {
    auto Res = m_Semaphore.getCounterValue();
    if (Res.result != vk::Result::eSuccess) {
        LogError("vkGetSemaphoreCounterValue failed: {}", vk::to_string(Res.result));
        return 0;
    }
    return Res.value;
}

auto VulkanTimelineSemaphore::Wait(Uint64 Value, Uint64 TimeoutNs) -> void {
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

auto VulkanTimelineSemaphore::GetSignalSubmitInfo(vk::PipelineStageFlagBits2 Stage) -> vk::SemaphoreSubmitInfo {
    return vk::SemaphoreSubmitInfo{
        .semaphore = *m_Semaphore,
        .value     = IncreaseHostValue(),
        .stageMask = Stage,
    };
}

auto VulkanTimelineSemaphore::Get() const noexcept -> vk::Semaphore {
    return *m_Semaphore;
}

} // namespace SoulEngine
