module;

#include <vk_mem_alloc.h>

export module RHIVulkan.RayTracingPipeline;

import RHI;
import Shader;
import TaskGraph;
import vulkan;
import std;

import RHIVulkan.Capability;
import RHIVulkan.Pipeline;
import RHIVulkan.ShaderBindingSet;
import RHIVulkan.Context;
import RHIVulkan.Debug;
import RHIVulkan.Staging;
import Core;

namespace SoulEngine {

/// Backend-independent layout for the immutable shader-binding-table payload.
export struct VulkanShaderBindingTableRegionLayout {
    Uint64 Offset      = 0;
    Uint64 Stride      = 0;
    Uint64 Size        = 0;
    Uint32 RecordCount = 0;
};

/// SBT allocation layout derived from Vulkan ray-tracing pipeline properties.
export struct VulkanShaderBindingTableLayout {
    Uint64                               HandleSize    = 0;
    Uint64                               RecordStride  = 0;
    Uint64                               TotalSize     = 0;
    VulkanShaderBindingTableRegionLayout RayGeneration = {};
    VulkanShaderBindingTableRegionLayout Miss          = {};
    VulkanShaderBindingTableRegionLayout Hit           = {};
    VulkanShaderBindingTableRegionLayout Callable      = {};
};

/// Calculate SBT offsets while preserving Vulkan's handle and base-alignment invariants.
export [[nodiscard]] auto VulkanCreateShaderBindingTableLayout(Uint32 HandleSize,
                                                               Uint32 HandleAlignment,
                                                               Uint32 BaseAlignment,
                                                               Uint32 MaxShaderGroupStride,
                                                               Uint32 RayGenerationRecordCount,
                                                               Uint32 MissRecordCount,
                                                               Uint32 HitRecordCount,
                                                               Uint32 CallableRecordCount)
    -> std::expected<VulkanShaderBindingTableLayout, ErrorMessage>;

/// @brief Vulkan realization of a ray-tracing pipeline and its immutable shader-binding table.
///
/// The SBT lives in self-owned device-local storage created with the pipeline (the base address is aligned to
/// shaderGroupBaseAlignment at creation, so the strided regions bake synchronously); the packed bytes stage
/// through the device-owned staging belt via a frame-affined task and land in the frame-start upload batch,
/// so a same-frame trace pass is safe by submission order (the GpuOnly buffer contract).
export class VulkanRayTracingPipeline final : public RHIRayTracingPipeline {
  public:
    explicit VulkanRayTracingPipeline(String Name, const RHIRayTracingPipelineDesc& Desc)
        : RHIRayTracingPipeline(std::move(Name), Desc.BindingSet) {}

    /// @brief The SBT buffer's native destruction rides the backend deferred deletion queue.
    ///
    /// A virtual timeline event keeps it valid until the submission that referenced it has drained.
    ~VulkanRayTracingPipeline() override;

    VulkanRayTracingPipeline(const VulkanRayTracingPipeline&)                    = delete;
    auto operator=(const VulkanRayTracingPipeline&) -> VulkanRayTracingPipeline& = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext&     Context,
                                     VulkanStagingBelt&               Staging,
                                     StringView                       Name,
                                     const RHIRayTracingPipelineDesc& Desc)
        -> std::expected<UPtr<VulkanRayTracingPipeline>, ErrorMessage>;

    [[nodiscard]] auto Get() const -> vk::Pipeline {
        return *(*m_VkPipeline);
    }

    [[nodiscard]] auto GetRayGenerationRegion() const -> const vk::StridedDeviceAddressRegionKHR& {
        return m_RayGenerationRegion;
    }

    [[nodiscard]] auto GetMissRegion() const -> const vk::StridedDeviceAddressRegionKHR& {
        return m_MissRegion;
    }

    [[nodiscard]] auto GetHitRegion() const -> const vk::StridedDeviceAddressRegionKHR& {
        return m_HitRegion;
    }

    [[nodiscard]] auto GetCallableRegion() const -> const vk::StridedDeviceAddressRegionKHR& {
        return m_CallableRegion;
    }

  private:
    SPtr<vk::raii::Pipeline>                      m_VkPipeline = nullptr;
    std::vector<VulkanReflectedDescriptorBinding> m_Bindings   = {};
    VulkanDeferredDeletionQueue*      m_DeferredQueue          = nullptr; ///< borrowed; owned by the resource context
    vk::Buffer                        m_SbtBuffer              = nullptr; ///< device-local shader binding table storage
    VmaAllocation                     m_SbtAllocation          = nullptr;
    vk::StridedDeviceAddressRegionKHR m_RayGenerationRegion    = {};
    vk::StridedDeviceAddressRegionKHR m_MissRegion             = {};
    vk::StridedDeviceAddressRegionKHR m_HitRegion              = {};
    vk::StridedDeviceAddressRegionKHR m_CallableRegion         = {};
    Uint32                            m_DynamicOffsetCount     = 0;
};

} // namespace SoulEngine
