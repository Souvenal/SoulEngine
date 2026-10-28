export module RHIVulkan.Command;

import Core;
import RHI;
import vulkan;
import std;

import RHIVulkan.Types;
import RHIVulkan.Context;
import RHIVulkan.Capability;
import RHIVulkan.Swapchain;
import RHIVulkan.Buffer;
import RHIVulkan.Readback;
import RHIVulkan.Pipeline;
import RHIVulkan.RayTracingPipeline;
import RHIVulkan.Sampler;
import RHIVulkan.Texture;
import RHIVulkan.Descriptor;
import RHIVulkan.AccelerationStructure;
import RHIVulkan.ShaderBindingSet;
import Shader;

namespace SoulEngine {

/// Shared Vulkan command execution context and operations.
export class VulkanCmdVisitor {
  public:
    VulkanCmdVisitor(const VulkanCmdVisitor&)                    = delete;
    auto operator=(const VulkanCmdVisitor&) -> VulkanCmdVisitor& = delete;
    VulkanCmdVisitor(VulkanCmdVisitor&&)                         = delete;
    auto operator=(VulkanCmdVisitor&&) -> VulkanCmdVisitor&      = delete;
    virtual ~VulkanCmdVisitor()                                  = default;

    template <typename T>
    auto operator()(const T&) -> void {
        Error = ErrorMessage("Command is not supported by this Vulkan pass visitor");
    }

    /// @brief Bind a pipeline-owned shader binding set for the current pass.
    ///
    /// Applies the committed dynamic-offset and push-constant snapshot.
    auto BindShaderBindingSet(VulkanShaderBindingSet& BindingSet, vk::PipelineBindPoint BindPoint) -> void;

    [[nodiscard]] auto GetError() const -> const std::optional<ErrorMessage>& {
        return Error;
    }

  protected:
    VulkanCmdVisitor(vk::raii::CommandBuffer& InBuffer, VulkanResourceContext& InResourceContext)
        : Buf(InBuffer), ResourceContext(InResourceContext) {}

    vk::raii::CommandBuffer&    Buf;
    VulkanResourceContext&      ResourceContext;
    std::optional<ErrorMessage> Error = std::nullopt;
};

export class VulkanGraphicsCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    ~VulkanGraphicsCmdVisitor();

    VulkanGraphicsCmdVisitor(VulkanGraphicsPipeline&       Pipeline,
                             const RHIGraphicsAttachments& Attachments,
                             vk::raii::CommandBuffer&      InBuffer,
                             VulkanResourceContext&        InResourceContext);

    auto BeginRendering(const RHIGraphicsAttachments& Attachments) -> void;

    auto operator()(const RHISetViewportCmd& Cmd) -> void;

    auto operator()(const RHISetScissorCmd& Cmd) -> void;

    auto operator()(const RHIDrawCmd& Cmd) -> void;

    auto operator()(const RHIDrawIndirectCmd& Cmd) -> void;
    auto operator()(const RHIBindIndexBufferCmd& Cmd) -> void;
    auto operator()(const RHIDrawIndexedIndirectCmd& Cmd) -> void;

  private:
    VulkanGraphicsPipeline& m_Pipeline;
    bool                    m_IsRendering         = false;
    vk::Extent2D            m_CurrentRenderExtent = {1, 1};
    std::vector<RHIFormat>  m_CurrentColorFormats = {};
    RHIFormat               m_CurrentDepthFormat  = RHIFormat::Unknown;
};

export class VulkanComputeCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    VulkanComputeCmdVisitor(VulkanComputePipeline&   Pipeline,
                            vk::raii::CommandBuffer& InBuffer,
                            VulkanResourceContext&   InResourceContext);

    auto operator()(const RHIDispatchCmd& Cmd) -> void;

  private:
    VulkanComputePipeline& m_Pipeline;
};

export class VulkanRayTracingCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    VulkanRayTracingCmdVisitor(VulkanRayTracingPipeline& Pipeline,
                               vk::raii::CommandBuffer&  InBuffer,
                               VulkanResourceContext&    InResourceContext);

    auto operator()(const RHITraceRaysCmd& Cmd) -> void;

  private:
    VulkanRayTracingPipeline& m_Pipeline;
};

export class VulkanTransferCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    VulkanTransferCmdVisitor(vk::raii::CommandBuffer& InBuffer,
                             VulkanResourceContext&   InResourceContext,
                             VulkanSwapchain&         InSwapchain)
        : VulkanCmdVisitor(InBuffer, InResourceContext), m_Swapchain(InSwapchain) {}

    auto operator()(const RHIBlitToSwapchainCmd& Cmd) -> void;

    auto operator()(const RHIReadbackTextureCmd& Cmd) -> void;

  private:
    VulkanSwapchain& m_Swapchain;
};

} // namespace SoulEngine
