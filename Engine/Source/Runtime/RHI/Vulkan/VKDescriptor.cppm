module;

export module Vulkan:Descriptor;

import Core;
import vulkan;
import std;

import :Capability;
import :FrameContext;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

struct DescriptorLayoutConfig {
    /// Raw sampler handles for Set 1 immutable bindings.
    /// Index 0 = linear-repeat, Index 1 = linear-repeat-aniso.
    std::array<vk::Sampler, 2> ImmutableSamplers = {};
    Uint32                     MaxTextures       = 4096;
};

// ═════════════════════════════════════════════════════════════════════════════
// DescriptorManager
// ═════════════════════════════════════════════════════════════════════════════

/// Owns descriptor allocation and long-lived global descriptor sets.
/// Pipeline-specific descriptor set layouts and pipeline layouts are created
/// from shader reflection by Vulkan::GraphicsPipeline.
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
    /// @param LayoutConfig     Shared descriptor layout ABI config.
    [[nodiscard]] static auto Create(vk::raii::Device&             Device,
                                     Uint32                        FramesInFlight,
                                     std::span<FrameContext>       FrameContexts,
                                     const DescriptorLayoutConfig& LayoutConfig)
        -> std::expected<DescriptorManager, ErrorMessage> {
        // ── Verify bindless features are supported ──────────────────────
        const auto& V12 = Capability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>();
        if (!V12.descriptorIndexing || !V12.descriptorBindingPartiallyBound ||
            !V12.descriptorBindingVariableDescriptorCount || !V12.runtimeDescriptorArray ||
            !V12.descriptorBindingSampledImageUpdateAfterBind)
            return std::unexpected(
                ErrorMessage("DescriptorManager: required Vulkan 1.2 bindless features not supported by device"));

        if (LayoutConfig.MaxTextures == 0)
            return std::unexpected(ErrorMessage("DescriptorManager: MaxTextures must be greater than zero"));

        DescriptorManager Mgr;
        Mgr.m_Device         = &Device;
        Mgr.m_FramesInFlight = FramesInFlight;

        // ── Descriptor pool ─────────────────────────────────────────────
        // Need FramesInFlight UBO descriptors (one per Set 0) +
        // 2 sampler descriptors (Set 1 immutable) + MaxTextures sampled images (Set 2).
        std::array PoolSizes = {
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBufferDynamic, FramesInFlight},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 2},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, LayoutConfig.MaxTextures},
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

        // ── Set 0 layout: per-frame dynamic UniformBuffer ────────────────
        {
            std::array                        Bindings = {vk::DescriptorSetLayoutBinding{
                .binding         = 0,
                .descriptorType  = vk::DescriptorType::eUniformBufferDynamic,
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
            std::array SamplersArr = LayoutConfig.ImmutableSamplers;
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
                .descriptorCount = LayoutConfig.MaxTextures,
                .stageFlags      = vk::ShaderStageFlagBits::eAllGraphics,
            }};
            std::array BindingFlags = {vk::DescriptorBindingFlagBits::eUpdateAfterBind |
                                       vk::DescriptorBindingFlagBits::ePartiallyBound |
                                       vk::DescriptorBindingFlagBits::eVariableDescriptorCount};
            vk::DescriptorSetLayoutBindingFlagsCreateInfo FlagsCI{
                .bindingCount  = static_cast<Uint32>(BindingFlags.size()),
                .pBindingFlags = BindingFlags.data(),
            };
            vk::StructureChain<vk::DescriptorSetLayoutCreateInfo, vk::DescriptorSetLayoutBindingFlagsCreateInfo>
                 LayoutChain = {
                     {.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
                      .bindingCount = static_cast<Uint32>(Bindings.size()),
                      .pBindings    = Bindings.data()},
                     FlagsCI,
                 };
            auto Res = Device.createDescriptorSetLayout(LayoutChain.get<vk::DescriptorSetLayoutCreateInfo>());
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
                .descriptorType  = vk::DescriptorType::eUniformBufferDynamic,
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
            Uint32                                               VarCount2 = LayoutConfig.MaxTextures;
            vk::DescriptorSetVariableDescriptorCountAllocateInfo VarInfo2{
                .descriptorSetCount = 1,
                .pDescriptorCounts  = &VarCount2,
            };
            vk::StructureChain<vk::DescriptorSetAllocateInfo, vk::DescriptorSetVariableDescriptorCountAllocateInfo>
                 AllocChain2 = {
                     {.descriptorPool = *Mgr.m_Pool, .descriptorSetCount = 1, .pSetLayouts = &Layout2},
                     VarInfo2,
                 };
            auto Res2 = Device.allocateDescriptorSets(AllocChain2.get<vk::DescriptorSetAllocateInfo>());
            if (Res2.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("DescriptorManager: failed to allocate Set 2"));
            Mgr.m_Set2 = std::move(Res2.value[0]);
        }

        return Mgr;
    }

    // ── Binding ─────────────────────────────────────────────────────────

    [[nodiscard]] auto GetDescriptorSets(Uint32 FrameIndex) const -> std::array<vk::DescriptorSet, 3> {
        return {*m_Set0s[FrameIndex], *m_Set1, *m_Set2};
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
};

} // namespace SoulEngine::RHI::Vulkan
