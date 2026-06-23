module;

export module Vulkan:Descriptor;

import Core;
import vulkan;
import std;

import :Capability;
import :FrameContext;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// DescriptorManager
// ═════════════════════════════════════════════════════════════════════════════

/// Owns global descriptor resources for the Vulkan backend:
///
///   Set 0 — per-frame UniformBuffer (one descriptor set per frame-in-flight slot)
///   Set 1 — Immutable samplers (baked into layout at init time)
///   Set 2 — SampledImage bindless array (variable count, UPDATE_AFTER_BIND)
///
/// Pipelines all share the same 3-set pipeline layout.
/// Buffer bindless is deliberately omitted — future GPU buffer access uses BDA.
///
/// DescriptorSet 0 is duplicated per FramesInFlight so that each frame slot
/// has its own descriptor set, pre-wired at init time to point at that slot's
/// GlobalConstantBuffer.  This avoids vkUpdateDescriptorSets on a set that is
/// still referenced by a pending command buffer (VUID-vkUpdateDescriptorSets-None-03047).
class DescriptorManager {
  public:
    DescriptorManager() = default;

    DescriptorManager(DescriptorManager&&) noexcept                    = default;
    auto operator=(DescriptorManager&&) noexcept -> DescriptorManager& = default;

    DescriptorManager(const DescriptorManager&)                    = delete;
    auto operator=(const DescriptorManager&) -> DescriptorManager& = delete;

    /// @param Device           Vulkan device handle.
    /// @param FramesInFlight   Number of frame slots.
    /// @param FrameContexts    Per-frame state (must have FramesInFlight entries).
    ///                         GlobalConstantBuffer is wired to Set 0 at init time.
    /// @param Samplers         Raw sampler handles for Set 1 immutable bindings.
    ///                         Index 0 = linear-repeat, Index 1 = linear-repeat-aniso.
    [[nodiscard]] static auto Create(vk::raii::Device&               Device,
                                     Uint32                          FramesInFlight,
                                     std::span<FrameContext>         FrameContexts,
                                     std::span<const vk::Sampler, 2> Samplers)
        -> std::expected<DescriptorManager, ErrorMessage> {
        // ── Verify bindless features are supported ──────────────────────
        const auto& V12 = Capability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>();
        if (!V12.descriptorIndexing || !V12.descriptorBindingPartiallyBound ||
            !V12.descriptorBindingVariableDescriptorCount || !V12.runtimeDescriptorArray ||
            !V12.descriptorBindingSampledImageUpdateAfterBind)
            return std::unexpected(
                ErrorMessage("DescriptorManager: required Vulkan 1.2 bindless features not supported by device"));

        const auto&       VkCfg = ConfigManager::Get().GetConfig().RhiVulkan;
        DescriptorManager Mgr;
        Mgr.m_Device         = &Device;
        Mgr.m_MaxTextures    = VkCfg.MaxTextures.value_or(Mgr.m_MaxTextures);
        Mgr.m_FramesInFlight = FramesInFlight;

        // ── Descriptor pool ─────────────────────────────────────────────
        // Need FramesInFlight UBO descriptors (one per Set 0) +
        // 2 sampler descriptors (Set 1 immutable) + MaxTextures sampled images (Set 2).
        std::array PoolSizes = {
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, FramesInFlight},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 2},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, Mgr.m_MaxTextures},
        };
        // maxSets = FramesInFlight (Set 0 copies) + 1 (Set 1) + 1 (Set 2)
        Uint32                       MaxSets = FramesInFlight + 2;
        vk::DescriptorPoolCreateInfo PoolCI{
            // eFreeDescriptorSet is required because m_Set0s/m_Set2 are
            // vk::raii::DescriptorSet, whose destructors call vkFreeDescriptorSets.
            .flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind |
                             vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets       = MaxSets,
            .poolSizeCount = static_cast<Uint32>(PoolSizes.size()),
            .pPoolSizes    = PoolSizes.data(),
        };
        auto PoolRes = Device.createDescriptorPool(PoolCI);
        if (PoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("DescriptorManager: failed to create descriptor pool"));
        Mgr.m_Pool = std::move(PoolRes.value);

        // ── Set 0 layout: per-frame UniformBuffer (single, not bindless) ──
        {
            std::array                        Bindings = {vk::DescriptorSetLayoutBinding{
                .binding         = 0,
                .descriptorType  = vk::DescriptorType::eUniformBuffer,
                .descriptorCount = 1,
                .stageFlags      = vk::ShaderStageFlagBits::eAllGraphics,
            }};
            vk::DescriptorSetLayoutCreateInfo LayoutCI{
                .bindingCount = static_cast<Uint32>(Bindings.size()),
                .pBindings    = Bindings.data(),
            };
            auto Res = Device.createDescriptorSetLayout(LayoutCI);
            if (Res.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("DescriptorManager: failed to create Set 0 layout"));
            Mgr.m_SetLayout0 = std::move(Res.value);
        }

        // ── Set 1 layout: Immutable samplers ────────────────────────────
        // Binding 0 = linear-repeat, Binding 1 = linear-repeat-anisotropic.
        // Samplers are baked into the layout and never change at runtime.
        {
            std::array SamplersArr = {Samplers[0], Samplers[1]};
            std::array Bindings    = {
                vk::DescriptorSetLayoutBinding{
                    .binding            = 0,
                    .descriptorType     = vk::DescriptorType::eSampler,
                    .descriptorCount    = 1,
                    .stageFlags         = vk::ShaderStageFlagBits::eAllGraphics,
                    .pImmutableSamplers = &SamplersArr[0],
                },
                vk::DescriptorSetLayoutBinding{
                    .binding            = 1,
                    .descriptorType     = vk::DescriptorType::eSampler,
                    .descriptorCount    = 1,
                    .stageFlags         = vk::ShaderStageFlagBits::eAllGraphics,
                    .pImmutableSamplers = &SamplersArr[1],
                },
            };
            vk::DescriptorSetLayoutCreateInfo LayoutCI{
                .bindingCount = static_cast<Uint32>(Bindings.size()),
                .pBindings    = Bindings.data(),
            };
            auto Res = Device.createDescriptorSetLayout(LayoutCI);
            if (Res.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("DescriptorManager: failed to create Set 1 layout"));
            Mgr.m_SetLayout1 = std::move(Res.value);
        }

        // ── Set 2 layout: SampledImage bindless ──────────────────────────
        {
            std::array Bindings     = {vk::DescriptorSetLayoutBinding{
                .binding         = 0,
                .descriptorType  = vk::DescriptorType::eSampledImage,
                .descriptorCount = Mgr.m_MaxTextures,
                .stageFlags      = vk::ShaderStageFlagBits::eAllGraphics,
            }};
            std::array BindingFlags = {vk::DescriptorBindingFlagBits::eUpdateAfterBind |
                                       vk::DescriptorBindingFlagBits::ePartiallyBound |
                                       vk::DescriptorBindingFlagBits::eVariableDescriptorCount};
            vk::DescriptorSetLayoutBindingFlagsCreateInfo FlagsCI{
                .bindingCount  = static_cast<Uint32>(BindingFlags.size()),
                .pBindingFlags = BindingFlags.data(),
            };
            vk::DescriptorSetLayoutCreateInfo LayoutCI{
                .pNext        = &FlagsCI,
                .flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
                .bindingCount = static_cast<Uint32>(Bindings.size()),
                .pBindings    = Bindings.data(),
            };
            auto Res = Device.createDescriptorSetLayout(LayoutCI);
            if (Res.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("DescriptorManager: failed to create Set 2 layout"));
            Mgr.m_SetLayout2 = std::move(Res.value);
        }

        // ── Allocate Set 0 descriptor sets (one per frame slot) ──────────
        {
            auto                                 Layout0 = *Mgr.m_SetLayout0;
            std::vector<vk::DescriptorSetLayout> Layouts(FramesInFlight, Layout0);
            vk::DescriptorSetAllocateInfo        AllocInfo{
                .descriptorPool     = *Mgr.m_Pool,
                .descriptorSetCount = FramesInFlight,
                .pSetLayouts        = Layouts.data(),
            };
            auto Res = Device.allocateDescriptorSets(AllocInfo);
            if (Res.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("DescriptorManager: failed to allocate Set 0 descriptor sets"));
            Mgr.m_Set0s = std::move(Res.value);
        }

        // ── Wire each Set 0 to its frame's GlobalConstantBuffer ────────────
        for (Uint32 i = 0; i < FramesInFlight; ++i) {
            auto*                    CB    = FrameContexts[i].GlobalConstantBuffer.get();
            auto                     Buf   = CB->GetVkBuffer();
            auto                     Range = CB->GetSize();
            vk::DescriptorBufferInfo BufInfo{
                .buffer = Buf,
                .offset = 0,
                .range  = Range,
            };
            vk::WriteDescriptorSet Write{
                .dstSet          = *Mgr.m_Set0s[i],
                .dstBinding      = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType  = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo     = &BufInfo,
            };
            Device.updateDescriptorSets(Write, {});
        }

        // ── Allocate Set 1 (immutable samplers — no writes needed) ──────
        // Immutable samplers are baked into the layout; the set just needs to exist.
        {
            auto                          Layout1 = *Mgr.m_SetLayout1;
            vk::DescriptorSetAllocateInfo AllocInfo1{
                .descriptorPool     = *Mgr.m_Pool,
                .descriptorSetCount = 1,
                .pSetLayouts        = &Layout1,
            };
            auto Res1 = Device.allocateDescriptorSets(AllocInfo1);
            if (Res1.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("DescriptorManager: failed to allocate Set 1"));
            Mgr.m_Set1 = std::move(Res1.value[0]);
        }

        // ── Allocate Set 2 (bindless textures) ──────────────────────────
        {
            auto                                                 Layout2   = *Mgr.m_SetLayout2;
            Uint32                                               VarCount2 = Mgr.m_MaxTextures;
            vk::DescriptorSetVariableDescriptorCountAllocateInfo VarInfo2{
                .descriptorSetCount = 1,
                .pDescriptorCounts  = &VarCount2,
            };
            vk::DescriptorSetAllocateInfo AllocInfo2{
                .pNext              = &VarInfo2,
                .descriptorPool     = *Mgr.m_Pool,
                .descriptorSetCount = 1,
                .pSetLayouts        = &Layout2,
            };
            auto Res2 = Device.allocateDescriptorSets(AllocInfo2);
            if (Res2.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("DescriptorManager: failed to allocate Set 2"));
            Mgr.m_Set2 = std::move(Res2.value[0]);
        }

        // ── Shared pipeline layout ───────────────────────────────────────
        {
            std::array            DSLs = {*Mgr.m_SetLayout0, *Mgr.m_SetLayout1, *Mgr.m_SetLayout2};
            vk::PushConstantRange PCRange{
                .stageFlags = vk::ShaderStageFlagBits::eAllGraphics,
                .offset     = 0,
                .size       = 4,
            };
            vk::PipelineLayoutCreateInfo BaseLayoutCI{
                .setLayoutCount         = static_cast<Uint32>(DSLs.size()),
                .pSetLayouts            = DSLs.data(),
                .pushConstantRangeCount = 1,
                .pPushConstantRanges    = &PCRange,
            };
            auto LayoutRes = Device.createPipelineLayout(BaseLayoutCI);
            if (LayoutRes.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("DescriptorManager: failed to create pipeline layout"));
            Mgr.m_BaseLayout = std::move(LayoutRes.value);
        }

        return Mgr;
    }

    // ── Binding ─────────────────────────────────────────────────────────

    /// Bind all three descriptor sets to a command buffer for a given frame slot.
    /// Called by CommandList::Begin().  FrameIndex selects which Set 0 copy
    /// to bind (pre-wired to that frame's GlobalConstantBuffer at init time).
    auto BindTo(Uint32 FrameIndex, vk::raii::CommandBuffer& CmdBuf) const -> void {
        std::array<vk::DescriptorSet, 3> Sets = {*m_Set0s[FrameIndex], *m_Set1, *m_Set2};
        CmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_BaseLayout, 0, Sets, {});
    }

    // ── Pipeline layout (used internally by GraphicsPipeline::Create) ──

    [[nodiscard]] auto GetPipelineLayout() const -> vk::PipelineLayout {
        return *m_BaseLayout;
    }

    // ── Set layouts (for pipeline creation) ────────────────────────────

    [[nodiscard]] auto GetSetLayouts() const -> std::array<vk::DescriptorSetLayout, 3> {
        return {*m_SetLayout0, *m_SetLayout1, *m_SetLayout2};
    }

    // ── Texture slot management ────────────────────────────────────────

    [[nodiscard]] auto AllocateTexture() -> Uint32 {
        if (!m_TextureFreeList.empty()) {
            Uint32 Index = m_TextureFreeList.back();
            m_TextureFreeList.pop_back();
            return Index;
        }
        return m_TextureNext++;
    }

    auto FreeTexture(Uint32 Index) -> void {
        m_TextureFreeList.push_back(Index);
    }

    /// Write a texture descriptor into Set 2 at the given index.
    auto WriteTextureSlot(Uint32 Index, vk::ImageView ImageView, vk::ImageLayout Layout) -> void {
        vk::DescriptorImageInfo ImageInfo{
            .sampler     = nullptr,
            .imageView   = ImageView,
            .imageLayout = Layout,
        };
        vk::WriteDescriptorSet Write{
            .dstSet          = *m_Set2,
            .dstBinding      = 0,
            .dstArrayElement = Index,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eSampledImage,
            .pImageInfo      = &ImageInfo,
        };
        m_Device->updateDescriptorSets(Write, {});
    }

    // ── Members ─────────────────────────────────────────────────────────

    vk::raii::Device* m_Device         = nullptr;
    Uint32            m_MaxTextures    = 4096;
    Uint32            m_FramesInFlight = 2;

    // Slot freelist & bump counter (texture only)
    std::vector<Uint32> m_TextureFreeList;
    Uint32              m_TextureNext = 0;

    vk::raii::DescriptorPool             m_Pool       = nullptr;
    vk::raii::DescriptorSetLayout        m_SetLayout0 = nullptr; ///< Set 0: per-frame UBO
    vk::raii::DescriptorSetLayout        m_SetLayout1 = nullptr; ///< Set 1: immutable samplers
    vk::raii::DescriptorSetLayout        m_SetLayout2 = nullptr; ///< Set 2: bindless textures
    std::vector<vk::raii::DescriptorSet> m_Set0s;                ///< One per frame slot
    vk::raii::DescriptorSet              m_Set1       = nullptr; ///< Immutable sampler set
    vk::raii::DescriptorSet              m_Set2       = nullptr; ///< Bindless texture set
    vk::raii::PipelineLayout             m_BaseLayout = nullptr;
};

} // namespace SoulEngine::RHI::Vulkan
