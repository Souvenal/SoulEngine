module;

export module Vulkan:Descriptor;

import Core;
import RHI;
import Shader;
import vulkan;
import std;

import :Capability;
import :Debug;
import :AccelerationStructure;
import :Buffer;
import :Sampler;
import :Texture;

namespace SoulEngine {

struct DescriptorSetCacheKey {
    vk::DescriptorSetLayout Layout = nullptr;
    std::vector<Uint64> ResourceIds = {};
    auto operator==(const DescriptorSetCacheKey&) const -> bool = default;
};

struct DescriptorSetCacheKeyHash {
    auto operator()(const DescriptorSetCacheKey& Key) const -> std::size_t {
        std::size_t Hash = std::hash<vk::DescriptorSetLayout>{}(Key.Layout);
        for (const auto ResourceId : Key.ResourceIds)
            Hash ^= std::hash<Uint64>{}(ResourceId) + 0x9e3779b9U + (Hash << 6U) + (Hash >> 2U);
        return Hash;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanDescriptorManager
// ═════════════════════════════════════════════════════════════════════════════

/// Owns the global descriptor pool, persistent descriptor set allocation, and
/// descriptor write primitives. Descriptor set layouts and pipeline layouts are
/// created from shader reflection by VulkanShaderBindingSet.
class VulkanDescriptorManager {
  public:
    VulkanDescriptorManager() = default;

    // Custom moves: m_PoolMutex is not movable.
    VulkanDescriptorManager(VulkanDescriptorManager&& Other) noexcept
        : m_Device(Other.m_Device),
          m_DebugUtils(Other.m_DebugUtils),
          m_NextPersistentSet(Other.m_NextPersistentSet),
          m_Pool(std::move(Other.m_Pool)) {}
    auto operator=(VulkanDescriptorManager&& Other) noexcept -> VulkanDescriptorManager& {
        m_Device             = Other.m_Device;
        m_DebugUtils         = Other.m_DebugUtils;
        m_NextPersistentSet  = Other.m_NextPersistentSet;
        m_Pool               = std::move(Other.m_Pool);
        return *this;
    }

    VulkanDescriptorManager(const VulkanDescriptorManager&)                    = delete;
    auto operator=(const VulkanDescriptorManager&) -> VulkanDescriptorManager& = delete;

    /// @param Device           Vulkan device handle.
    [[nodiscard]] static auto Create(vk::raii::Device& Device, VulkanDebugUtils& DebugUtils)
        -> std::expected<VulkanDescriptorManager, ErrorMessage> {
        // ── Verify descriptor indexing features are supported ───────────
        const auto& V12 = VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>();
        if (!V12.descriptorIndexing || !V12.descriptorBindingPartiallyBound ||
            !V12.runtimeDescriptorArray || !V12.descriptorBindingSampledImageUpdateAfterBind ||
            !V12.descriptorBindingVariableDescriptorCount)
            return std::unexpected(
                ErrorMessage("VulkanDescriptorManager: required Vulkan 1.2 descriptor indexing features not supported by device"));

        VulkanDescriptorManager Mgr;
        Mgr.m_Device         = &Device;
        Mgr.m_DebugUtils     = &DebugUtils;

        // ── Descriptor pool ─────────────────────────────────────────────
        constexpr Uint32 ScratchDescriptorCount           = 4096;
        constexpr Uint32 ImGuiDescriptorCount             = 4096;
        constexpr Uint32 SharedDescriptorCount            = ScratchDescriptorCount + ImGuiDescriptorCount;
        constexpr Uint32 PersistentSampledImageSetBudget = 4;
        constexpr Uint32 SampledImageDescriptorCount =
            ScratchDescriptorCount * (PersistentSampledImageSetBudget + 1) + ImGuiDescriptorCount;
        std::vector<vk::DescriptorPoolSize> PoolSizes{
            vk::DescriptorPoolSize{vk::DescriptorType::eSampler, SharedDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, SampledImageDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, SharedDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformTexelBuffer, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageTexelBuffer, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, SharedDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBufferDynamic, SharedDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBufferDynamic, ImGuiDescriptorCount},
            vk::DescriptorPoolSize{vk::DescriptorType::eInputAttachment, ImGuiDescriptorCount},
        };
        if (VulkanCapability::Get().IsRayTracingAvailable())
            PoolSizes.emplace_back(vk::DescriptorType::eAccelerationStructureKHR, ScratchDescriptorCount);
        Uint32 MaxSets = SharedDescriptorCount + 1;
        vk::DescriptorPoolCreateInfo PoolCI{
            // eFreeDescriptorSet is required because allocated sets are
            // vk::raii::DescriptorSet, whose destructors call vkFreeDescriptorSets.
            .flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind |
                             vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            .maxSets       = MaxSets,
            .poolSizeCount = static_cast<Uint32>(PoolSizes.size()),
            .pPoolSizes    = PoolSizes.data(),
        };
        auto PoolRes = Device.createDescriptorPool(PoolCI);
        if (PoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("VulkanDescriptorManager: failed to create descriptor pool"));
        DebugUtils.SetObjectName(*PoolRes.value, "Internal/DescriptorPool/Global");
        Mgr.m_Pool = std::move(PoolRes.value);
        return Mgr;
    }

    // ── Binding ─────────────────────────────────────────────────────────

    [[nodiscard]] auto GetDescriptorPool() const -> vk::DescriptorPool {
        return *m_Pool;
    }

    /// Releases cached descriptor sets before the owning Vulkan device is destroyed.
    auto Shutdown() -> void {
        m_SetCache.clear();
        m_Pool = nullptr;
        m_Device = nullptr;
        m_DebugUtils = nullptr;
    }

  private:
    /// Allocates one descriptor set; VariableCount (when set) feeds
    /// VkDescriptorSetVariableDescriptorCountAllocateInfo for layouts whose
    /// last binding is an unbounded array (WT2).
    [[nodiscard]] auto AllocateDescriptorSets(std::span<const vk::DescriptorSetLayout> Layouts,
                                              std::optional<Uint32>                       VariableCount)
        -> std::expected<std::vector<vk::raii::DescriptorSet>, ErrorMessage> {
        if (Layouts.empty())
            return std::unexpected(ErrorMessage("VulkanDescriptorManager: descriptor set layout list is empty"));

        // Pool allocation requires external synchronization; binding sets may be
        // created on the render thread (per-view sets) while the RHI thread
        // allocates for pipeline binding sets.
        std::scoped_lock Lock(m_PoolMutex);
        vk::DescriptorSetAllocateInfo AllocateInfo{
            .descriptorPool     = *m_Pool,
            .descriptorSetCount = static_cast<Uint32>(Layouts.size()),
            .pSetLayouts        = Layouts.data(),
        };
        std::expected<std::vector<vk::raii::DescriptorSet>, ErrorMessage> Result = std::unexpected(
            ErrorMessage("VulkanDescriptorManager: failed to allocate descriptor sets"));

        if (VariableCount) {
            const Uint32 Counts[] = {*VariableCount};
            vk::DescriptorSetVariableDescriptorCountAllocateInfo VariableCountsInfo{
                .descriptorSetCount = 1,
                .pDescriptorCounts  = Counts,
            };
            vk::StructureChain<vk::DescriptorSetAllocateInfo,
                               vk::DescriptorSetVariableDescriptorCountAllocateInfo>
                Chain{AllocateInfo, VariableCountsInfo};
            auto Res = m_Device->allocateDescriptorSets(Chain.get<vk::DescriptorSetAllocateInfo>());
            if (Res.result == vk::Result::eSuccess && Res.value.size() == Layouts.size())
                Result = std::move(Res.value);
        } else {
            auto Res = m_Device->allocateDescriptorSets(AllocateInfo);
            if (Res.result == vk::Result::eSuccess && Res.value.size() == Layouts.size())
                Result = std::move(Res.value);
        }
        if (!Result)
            return Result;

        std::vector<vk::raii::DescriptorSet> Sets;
        Sets.reserve(Result->size());
        for (auto& Set : *Result) {
            if (m_DebugUtils)
                m_DebugUtils->SetObjectName(
                    *Set, Format("Internal/DescriptorSet/Persistent/Set{}", m_NextPersistentSet++));
            Sets.push_back(std::move(Set));
        }
        return Sets;
    }

  public:
    /// Applies descriptor writes to already allocated descriptor sets.
    auto WriteDescriptorSets(std::span<const vk::WriteDescriptorSet> Writes) -> void {
        if (!Writes.empty())
            m_Device->updateDescriptorSets(Writes, {});
    }

    /// Returns the cached descriptor set for `Key`, allocating it on a miss.
    /// The returned handle is owned by the descriptor cache and remains valid
    /// until the entry is removed by `Tick()`.
    [[nodiscard]] auto AcquireDescriptorSet(const DescriptorSetCacheKey& Key,
                                             std::span<const RHIShaderBindingSet::Slot> Bindings) -> vk::DescriptorSet {
        // Step 1: Check cache for existing set. Refresh LRU and return if found
        if (const auto It = m_SetCache.find(Key); It != m_SetCache.end()) {
            It->second.LruAge = 0;
            return *It->second.Set;
        }

        // Step 2: Cache miss, so allocate a new descriptor set and write all bindings.
        // Unbounded array layouts allocate with a variable descriptor count equal to
        // the highest bound element index + 1 (partially bound covers the rest).
        std::optional<Uint32> VariableCount = std::nullopt;
        for (const auto& Binding : Bindings) {
            if (!Binding.IsVariableCountArray)
                continue;
            Uint32 Count = 1;
            for (const auto& Element : Binding.ArrayElements)
                Count = (std::max)(Count, Element.ArrayIndex + 1);
            VariableCount = Count;
            break;
        }
        const auto Layout = Key.Layout;
        auto Allocated = AllocateDescriptorSets(std::span{&Layout, 1}, VariableCount);
        if (!Allocated) {
            LogError("{}", Allocated.error().ToString());
            return nullptr;
        }

        // Step 3: Write the new descriptor sets
        auto Set = std::move((*Allocated)[0]);
        const auto Handle = *Set;
        std::vector<vk::WriteDescriptorSet> Writes;
        Writes.reserve(Bindings.size());
        for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
            const auto& Binding = Bindings[Index];
            // Array slots write one descriptor per bound element (WT2).
            if (!Binding.ArrayElements.empty()) {
                for (const auto& Element : Binding.ArrayElements) {
                    switch (Binding.Info.Type) {
                    case ShaderResourceType::SampledTexture:
                        if (Element.IsRenderTarget) {
                            const auto* Target = static_cast<const VulkanRenderTarget*>(Element.Resource);
                            // D16: an out-of-range mip selects the whole-chain view.
                            const auto MipIndex = Element.Mip >= Target->GetMipLevelCount()
                                                      ? VulkanRenderTarget::WholeChain
                                                      : Element.Mip;
                            Writes.push_back(Target->GetWriteDescriptorSet(
                                Handle, Binding.Info.BindingIndex, Element.ArrayIndex, Binding.IsReadOnly, MipIndex));
                        } else {
                            const auto* Texture = static_cast<const VulkanSampledTexture*>(Element.Resource);
                            Writes.push_back(Texture->GetWriteDescriptorSet(
                                Handle, Binding.Info.BindingIndex, Element.ArrayIndex, Binding.IsReadOnly));
                        }
                        break;
                    case ShaderResourceType::StorageTexture:
                        if (const auto* Target = static_cast<const VulkanRenderTarget*>(Element.Resource)) {
                            const auto MipIndex = Element.Mip >= Target->GetMipLevelCount()
                                                      ? VulkanRenderTarget::WholeChain
                                                      : Element.Mip;
                            Writes.push_back(Target->GetWriteDescriptorSet(
                                Handle, Binding.Info.BindingIndex, Element.ArrayIndex, Binding.IsReadOnly, MipIndex));
                        }
                        break;
                    default:
                        // Arrays of buffers/samplers are not used yet; extend here.
                        break;
                    }
                }
                continue;
            }
            switch (Binding.Info.Type) {
            case ShaderResourceType::ConstantBuffer:
                if (const auto* Buffer = static_cast<const VulkanTransientConstantBuffer*>(Binding.Resource))
                    Writes.push_back(Buffer->GetWriteDescriptorSet(Handle, Binding.Info.BindingIndex, Binding.IsReadOnly));
                break;
            case ShaderResourceType::StorageBuffer:
                if (const auto* Buffer = static_cast<const VulkanTransientShaderStorageBuffer*>(Binding.Resource))
                    Writes.push_back(Buffer->GetWriteDescriptorSet(Handle, Binding.Info.BindingIndex, Binding.IsReadOnly));
                break;
            case ShaderResourceType::SampledTexture:
                if (Binding.IsRenderTarget) {
                    const auto* Target = static_cast<const VulkanRenderTarget*>(Binding.Resource);
                    Writes.push_back(Target->GetWriteDescriptorSet(Handle, Binding.Info.BindingIndex, 0, Binding.IsReadOnly));
                } else {
                    const auto* Texture = static_cast<const VulkanSampledTexture*>(Binding.Resource);
                    Writes.push_back(Texture->GetWriteDescriptorSet(Handle, Binding.Info.BindingIndex, 0, Binding.IsReadOnly));
                }
                break;
            case ShaderResourceType::StorageTexture:
                if (const auto* Target = static_cast<const VulkanRenderTarget*>(Binding.Resource))
                    Writes.push_back(Target->GetWriteDescriptorSet(Handle, Binding.Info.BindingIndex, 0, Binding.IsReadOnly));
                break;
            case ShaderResourceType::Sampler:
                if (const auto* Sampler = static_cast<const VulkanSampler*>(Binding.Resource))
                    Writes.push_back(Sampler->GetWriteDescriptorSet(Handle, Binding.Info.BindingIndex, Binding.IsReadOnly));
                break;
            case ShaderResourceType::AccelerationStructure:
                if (const auto* Tlas = static_cast<const VulkanTopLevelAccelerationStructure*>(Binding.Resource))
                    Writes.push_back(Tlas->GetWriteDescriptorSet(Handle, Binding.Info.BindingIndex, Binding.IsReadOnly));
                break;
            case ShaderResourceType::Unknown:
                break;
            }
        }
        if (!Writes.empty())
            m_Device->updateDescriptorSets(Writes, {});
        m_SetCache.emplace(Key, CacheEntry{
                                     .Set = std::move(Set),
                                     .LruAge = 0,
                                 });
        return Handle;
    }

    auto Tick() -> void {
        for (auto It = m_SetCache.begin(); It != m_SetCache.end();) {
            ++It->second.LruAge;
            if (It->second.LruAge >= s_CacheLifetimeTicks)
                It = m_SetCache.erase(It);
            else
                ++It;
        }
    }

    // ── Members ─────────────────────────────────────────────────────────

    vk::raii::Device* m_Device              = nullptr;
    VulkanDebugUtils* m_DebugUtils          = nullptr;
    Uint32            m_NextPersistentSet   = 0;

    struct CacheEntry {
        vk::raii::DescriptorSet Set = nullptr;
        Uint64 LruAge = 0;
    };

    vk::raii::DescriptorPool m_Pool = nullptr;
    std::mutex               m_PoolMutex = {};
    std::unordered_map<DescriptorSetCacheKey, CacheEntry, DescriptorSetCacheKeyHash> m_SetCache = {};
    static constexpr Uint64 s_CacheLifetimeTicks = 10;
};

} // namespace SoulEngine
