module;

#include <magic_enum/magic_enum_flags.hpp>

export module Vulkan:Descriptor;

import Core;
import RHI;
import Shader;
import vulkan;
import std;
import TaskGraph;

import :Capability;
import :Debug;
import :AccelerationStructure;
import :Buffer;
import :Sampler;
import :Texture;

namespace SoulEngine {

inline constexpr Uint32 BindlessTextureBinding = 2;

[[nodiscard]] auto GetBindlessDescriptorCount() -> Uint32;

struct DescriptorSetCacheKey {
    vk::DescriptorSetLayout Layout                                                 = nullptr;
    std::vector<Uint64>     ResourceIds                                            = {};
    auto                    operator==(const DescriptorSetCacheKey&) const -> bool = default;
};

struct DescriptorSetCacheKeyHash {
    auto operator()(const DescriptorSetCacheKey& Key) const -> std::size_t;
};

// Descriptor write construction Descriptor writes are a binding-layer concern: resources expose native
// accessors, and these helpers compose each write from the bound view (role, range, direction) plus the
// slot's reflected (type, access) descriptor kind.

[[nodiscard]] auto ToVkDescriptorType(ShaderResourceType Type, ShaderBindingAccess Access) -> vk::DescriptorType;

/// @brief Accumulates descriptor writes together with the pointee storage their pointers reference.
///
/// vk::WriteDescriptorSet holds raw pointers (pImageInfo/pBufferInfo/pNext); the deques keep every payload at
/// a stable address until updateDescriptorSets consumes the batch.
struct VulkanDescriptorWriteBatch {
    std::vector<vk::WriteDescriptorSet>                        Writes      = {};
    std::deque<vk::DescriptorImageInfo>                        ImageInfos  = {};
    std::deque<vk::DescriptorBufferInfo>                       BufferInfos = {};
    std::deque<vk::AccelerationStructureKHR>                   AsHandles   = {};
    std::deque<vk::WriteDescriptorSetAccelerationStructureKHR> AsInfos     = {};

    auto Apply(const vk::raii::Device& Device) const -> void;

    auto AppendTextureWrite(vk::DescriptorSet     Set,
                            Uint32                BindingIndex,
                            Uint32                ArrayElement,
                            const VulkanTexture&  Texture,
                            const RHITextureView& View,
                            vk::DescriptorType    Type) -> void;

    auto
    AppendBufferWrite(vk::DescriptorSet Set, Uint32 BindingIndex, const VulkanBuffer& Buffer, vk::DescriptorType Type)
        -> void;

    auto AppendSamplerWrite(vk::DescriptorSet Set, Uint32 BindingIndex, const VulkanSampler& Sampler) -> void;

    auto AppendAccelerationStructureWrite(vk::DescriptorSet                          Set,
                                          Uint32                                     BindingIndex,
                                          const VulkanTopLevelAccelerationStructure& Tlas) -> void;
};

/// @brief Global descriptor pool, persistent descriptor set allocation, and descriptor write primitives.
///
/// Descriptor set layouts and pipeline layouts are created from shader reflection by VulkanShaderBindingSet.
class VulkanDescriptorManager {
  public:
    VulkanDescriptorManager() = default;

    [[nodiscard]] auto GetBindlessDescriptorSetLayout() const -> vk::DescriptorSetLayout;

    [[nodiscard]] auto GetBindlessDescriptorSet() const -> vk::DescriptorSet;

    // Custom moves: m_PoolMutex is not movable.
    VulkanDescriptorManager(VulkanDescriptorManager&& Other) noexcept;
    auto operator=(VulkanDescriptorManager&& Other) noexcept -> VulkanDescriptorManager&;

    VulkanDescriptorManager(const VulkanDescriptorManager&)                    = delete;
    auto operator=(const VulkanDescriptorManager&) -> VulkanDescriptorManager& = delete;

    /// @param Device           Vulkan device handle.
    [[nodiscard]] static auto Create(vk::raii::Device& Device, VulkanDebugUtils& DebugUtils)
        -> std::expected<VulkanDescriptorManager, ErrorMessage>;

    [[nodiscard]] auto GetDescriptorPool() const -> vk::DescriptorPool;

    /// Releases cached descriptor sets before the owning Vulkan device is destroyed.
    auto Shutdown() -> void;

  public:
    [[nodiscard]] auto RegisterBindlessTextures(std::span<const RHIRef<RHITexture>> Textures)
        -> std::expected<void, ErrorMessage>;

    [[nodiscard]] auto FindBindlessTextureIndex(const RHIRef<RHITexture>& Texture) const -> std::optional<Uint32>;

  private:
    auto RegisterBindlessTexturesOnRhiThread(std::span<const RHIRef<RHITexture>> Textures) -> void;

    /// @brief Allocates one descriptor set.
    ///
    /// VariableCount (when set) feeds VkDescriptorSetVariableDescriptorCountAllocateInfo for layouts whose
    /// last binding is an unbounded array.
    [[nodiscard]] auto AllocateDescriptorSets(std::span<const vk::DescriptorSetLayout> Layouts,
                                              std::optional<Uint32>                    VariableCount)
        -> std::expected<std::vector<vk::raii::DescriptorSet>, ErrorMessage>;

  public:
    /// @brief Apply descriptor writes to already allocated descriptor sets.
    ///
    /// Returns the cached descriptor set for `Key`, allocating it on a miss. The returned handle is owned by
    /// the descriptor cache and remains valid until the entry is removed by `Tick()`.
    [[nodiscard]] auto AcquireDescriptorSet(const DescriptorSetCacheKey&               Key,
                                            std::span<const RHIShaderBindingSet::Slot> Bindings) -> vk::DescriptorSet;

    auto Tick() -> void;
    // Members

    vk::raii::Device* m_Device            = nullptr;
    VulkanDebugUtils* m_DebugUtils        = nullptr;
    Uint32            m_NextPersistentSet = 0;

    struct CacheEntry {
        vk::raii::DescriptorSet Set    = nullptr;
        Uint64                  LruAge = 0;
    };

    vk::raii::DescriptorPool                                                         m_Pool                   = nullptr;
    vk::raii::DescriptorSetLayout                                                    m_BindlessSetLayout      = nullptr;
    std::vector<vk::raii::DescriptorSet>                                             m_BindlessSets           = {};
    std::mutex                                                                       m_PoolMutex              = {};
    mutable std::mutex                                                               m_BindlessMutex          = {};
    Uint32                                                                           m_NextBindlessTexture    = 1;
    std::unordered_map<Uint64, Uint32>                                               m_BindlessTextureIndices = {};
    std::unordered_set<Uint64>                                                       m_BindlessPendingIds     = {};
    std::unordered_map<Uint64, RHIRef<RHITexture>>                                   m_BindlessTextures       = {};
    std::unordered_map<DescriptorSetCacheKey, CacheEntry, DescriptorSetCacheKeyHash> m_SetCache               = {};
    static constexpr Uint64                                                          s_CacheLifetimeTicks     = 10;
};

} // namespace SoulEngine
