module;

#include <magic_enum/magic_enum_flags.hpp>

module Vulkan:Descriptor.impl;

import Core;
import RHI;
import Shader;
import vulkan;
import std;
import TaskGraph;

import :Descriptor;
import :Capability;
import :Debug;
import :AccelerationStructure;
import :Buffer;
import :Sampler;
import :Texture;

namespace SoulEngine {

auto GetBindlessDescriptorCount() -> Uint32 {
    const auto& Limits = VulkanCapability::Get().GetProperties<vk::PhysicalDeviceVulkan12Properties>();
    return (std::min)(Uint32{4096},
                      (std::min)(Limits.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                 Limits.maxDescriptorSetUpdateAfterBindSampledImages));
}

auto DescriptorSetCacheKeyHash::operator()(const DescriptorSetCacheKey& Key) const -> std::size_t {
    std::size_t Hash = std::hash<vk::DescriptorSetLayout>{}(Key.Layout);
    for (const auto ResourceId : Key.ResourceIds)
        Hash ^= std::hash<Uint64>{}(ResourceId) + 0x9e3779b9U + (Hash << 6U) + (Hash >> 2U);
    return Hash;
}

auto ToVkDescriptorType(ShaderResourceType Type, ShaderBindingAccess Access) -> vk::DescriptorType {
    switch (Type) {
    case ShaderResourceType::ConstantBuffer:
        return vk::DescriptorType::eUniformBufferDynamic;
    case ShaderResourceType::StorageBuffer:
        // Descriptor-bound storage buffers are always transient belt slices; persistent buffers use vertex
        // input or device addresses instead.
        return vk::DescriptorType::eStorageBufferDynamic;
    case ShaderResourceType::Texture:
        // The image descriptor kind follows the binding's shader-declared access: read-only bindings take
        // sampled images, writable bindings storage images.
        return Access == ShaderBindingAccess::ReadWrite ? vk::DescriptorType::eStorageImage
                                                        : vk::DescriptorType::eSampledImage;
    case ShaderResourceType::Sampler:
        return vk::DescriptorType::eSampler;
    case ShaderResourceType::AccelerationStructure:
        return vk::DescriptorType::eAccelerationStructureKHR;
    default:
        return {};
    }
}

auto VulkanDescriptorWriteBatch::Apply(const vk::raii::Device& Device) const -> void {
    if (!Writes.empty())
        Device.updateDescriptorSets(Writes, {});
}

auto VulkanDescriptorWriteBatch::AppendTextureWrite(vk::DescriptorSet     Set,
                                                    Uint32                BindingIndex,
                                                    Uint32                ArrayElement,
                                                    const VulkanTexture&  Texture,
                                                    const RHITextureView& View,
                                                    vk::DescriptorType    Type) -> void {
    const bool IsStorage = View.Usage == RHITextureUsage::Storage;
    auto&      Info      = ImageInfos.emplace_back(vk::DescriptorImageInfo{
        .sampler     = nullptr,
        .imageView   = View.Range.MipLevelCount == kRHIRemainingSubresources
                           ? Texture.GetVkImageView()
                           : Texture.GetVkImageView(View.Range.BaseMipLevel),
        .imageLayout = IsStorage ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal,
    });
    Writes.push_back(vk::WriteDescriptorSet{.dstSet          = Set,
                                            .dstBinding      = BindingIndex,
                                            .dstArrayElement = ArrayElement,
                                            .descriptorCount = 1,
                                            .descriptorType  = Type,
                                            .pImageInfo      = &Info});
}

auto VulkanDescriptorWriteBatch::AppendBufferWrite(vk::DescriptorSet   Set,
                                                   Uint32              BindingIndex,
                                                   const VulkanBuffer& Buffer,
                                                   vk::DescriptorType  Type) -> void {
    auto& Info = BufferInfos.emplace_back(
        vk::DescriptorBufferInfo{.buffer = Buffer.GetVkBuffer(), .offset = 0, .range = Buffer.GetSize()});
    Writes.push_back(vk::WriteDescriptorSet{
        .dstSet = Set, .dstBinding = BindingIndex, .descriptorCount = 1, .descriptorType = Type, .pBufferInfo = &Info});
}

auto VulkanDescriptorWriteBatch::AppendSamplerWrite(vk::DescriptorSet    Set,
                                                    Uint32               BindingIndex,
                                                    const VulkanSampler& Sampler) -> void {
    auto& Info = ImageInfos.emplace_back(vk::DescriptorImageInfo{.sampler = Sampler.GetVkSampler()});
    Writes.push_back(vk::WriteDescriptorSet{.dstSet          = Set,
                                            .dstBinding      = BindingIndex,
                                            .descriptorCount = 1,
                                            .descriptorType  = vk::DescriptorType::eSampler,
                                            .pImageInfo      = &Info});
}

auto VulkanDescriptorWriteBatch::AppendAccelerationStructureWrite(vk::DescriptorSet Set,
                                                                  Uint32            BindingIndex,
                                                                  const VulkanTopLevelAccelerationStructure& Tlas)
    -> void {
    auto&                  AsHandle = AsHandles.emplace_back(Tlas.GetAccelerationStructure());
    auto&                  AsInfo   = AsInfos.emplace_back(vk::WriteDescriptorSetAccelerationStructureKHR{
        .accelerationStructureCount = 1, .pAccelerationStructures = &AsHandle});
    vk::WriteDescriptorSet Write{.dstSet          = Set,
                                 .dstBinding      = BindingIndex,
                                 .descriptorCount = 1,
                                 .descriptorType  = vk::DescriptorType::eAccelerationStructureKHR};
    Write.pNext = &AsInfo;
    Writes.push_back(Write);
}

auto VulkanDescriptorManager::GetBindlessDescriptorSetLayout() const -> vk::DescriptorSetLayout {
    return *m_BindlessSetLayout;
}

auto VulkanDescriptorManager::GetBindlessDescriptorSet() const -> vk::DescriptorSet {
    return m_BindlessSets.empty() ? vk::DescriptorSet{} : *m_BindlessSets.front();
}

VulkanDescriptorManager::VulkanDescriptorManager(VulkanDescriptorManager&& Other) noexcept
    : m_Device(std::exchange(Other.m_Device, nullptr)),
      m_DebugUtils(std::exchange(Other.m_DebugUtils, nullptr)),
      m_NextPersistentSet(std::exchange(Other.m_NextPersistentSet, 0)),
      m_Pool(std::exchange(Other.m_Pool, nullptr)),
      m_BindlessSetLayout(std::exchange(Other.m_BindlessSetLayout, nullptr)),
      m_BindlessSets(std::exchange(Other.m_BindlessSets, {})),
      m_BindlessTextureIndices(std::exchange(Other.m_BindlessTextureIndices, {})),
      m_BindlessPendingIds(std::exchange(Other.m_BindlessPendingIds, {})),
      m_BindlessTextures(std::exchange(Other.m_BindlessTextures, {})),
      m_SetCache(std::exchange(Other.m_SetCache, {})) {}

auto VulkanDescriptorManager::operator=(VulkanDescriptorManager&& Other) noexcept -> VulkanDescriptorManager& {
    if (this == std::addressof(Other))
        return *this;
    std::swap(m_Device, Other.m_Device);
    std::swap(m_DebugUtils, Other.m_DebugUtils);
    std::swap(m_NextPersistentSet, Other.m_NextPersistentSet);
    std::swap(m_Pool, Other.m_Pool);
    std::swap(m_BindlessSetLayout, Other.m_BindlessSetLayout);
    std::swap(m_BindlessSets, Other.m_BindlessSets);
    std::swap(m_BindlessTextureIndices, Other.m_BindlessTextureIndices);
    std::swap(m_BindlessPendingIds, Other.m_BindlessPendingIds);
    std::swap(m_BindlessTextures, Other.m_BindlessTextures);
    std::swap(m_SetCache, Other.m_SetCache);
    return *this;
}

auto VulkanDescriptorManager::Create(vk::raii::Device& Device, VulkanDebugUtils& DebugUtils)
    // Verify descriptor indexing features are supported
    -> std::expected<VulkanDescriptorManager, ErrorMessage> {
    const auto& V12 = VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>();
    if (!V12.descriptorIndexing || !V12.descriptorBindingPartiallyBound || !V12.runtimeDescriptorArray ||
        !V12.descriptorBindingSampledImageUpdateAfterBind || !V12.descriptorBindingVariableDescriptorCount)
        return std::unexpected(ErrorMessage(
            "VulkanDescriptorManager: required Vulkan 1.2 descriptor indexing features not supported by device"));

    VulkanDescriptorManager Mgr;
    Mgr.m_Device     = &Device;
    Mgr.m_DebugUtils = &DebugUtils;

    // Descriptor pool
    constexpr Uint32 ScratchDescriptorCount          = 4096;
    constexpr Uint32 ImGuiDescriptorCount            = 4096;
    constexpr Uint32 SharedDescriptorCount           = ScratchDescriptorCount + ImGuiDescriptorCount;
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
    Uint32                       MaxSets = SharedDescriptorCount + 2;
    vk::DescriptorPoolCreateInfo PoolCI{
        // eFreeDescriptorSet is required because allocated sets are vk::raii::DescriptorSet, whose
        // destructors call vkFreeDescriptorSets.
        .flags =
            vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind | vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets       = MaxSets,
        .poolSizeCount = static_cast<Uint32>(PoolSizes.size()),
        .pPoolSizes    = PoolSizes.data(),
    };
    auto PoolRes = Device.createDescriptorPool(PoolCI);
    if (PoolRes.result != vk::Result::eSuccess)
        return std::unexpected(ErrorMessage("VulkanDescriptorManager: failed to create descriptor pool"));
    DebugUtils.SetObjectName(*PoolRes.value, "Internal/DescriptorPool/Global");
    Mgr.m_Pool = std::move(PoolRes.value);

    const Uint32                   DescriptorCount = GetBindlessDescriptorCount();
    vk::DescriptorSetLayoutBinding BindlessBinding{
        .binding            = BindlessTextureBinding,
        .descriptorType     = vk::DescriptorType::eSampledImage,
        .descriptorCount    = DescriptorCount,
        .stageFlags         = vk::ShaderStageFlagBits::eAll,
        .pImmutableSamplers = nullptr,
    };
    auto BindlessFlags =
        vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    if (V12.descriptorBindingUpdateUnusedWhilePending)
        BindlessFlags |= vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
    vk::DescriptorSetLayoutBindingFlagsCreateInfo FlagsInfo{
        .bindingCount  = 1,
        .pBindingFlags = &BindlessFlags,
    };
    vk::StructureChain<vk::DescriptorSetLayoutCreateInfo, vk::DescriptorSetLayoutBindingFlagsCreateInfo> LayoutChain{
        vk::DescriptorSetLayoutCreateInfo{
            .flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
            .bindingCount = 1,
            .pBindings    = &BindlessBinding,
        },
        FlagsInfo,
    };
    auto BindlessLayout = Device.createDescriptorSetLayout(LayoutChain.get<vk::DescriptorSetLayoutCreateInfo>());
    if (BindlessLayout.result != vk::Result::eSuccess)
        return std::unexpected(
            ErrorMessage("VulkanDescriptorManager: failed to create bindless descriptor set layout"));
    DebugUtils.SetObjectName(*BindlessLayout.value, "Internal/DescriptorSetLayout/Bindless");
    Mgr.m_BindlessSetLayout = std::move(BindlessLayout.value);

    auto BindlessSets = Mgr.AllocateDescriptorSets(
        std::span<const vk::DescriptorSetLayout>{&*Mgr.m_BindlessSetLayout, 1}, std::nullopt);
    if (!BindlessSets || BindlessSets->size() != 1)
        return std::unexpected(BindlessSets ? ErrorMessage("VulkanDescriptorManager: invalid bindless set allocation")
                                            : BindlessSets.error());
    Mgr.m_BindlessSets = std::move(*BindlessSets);
    DebugUtils.SetObjectName(*Mgr.m_BindlessSets.front(), "Internal/DescriptorSet/Bindless");
    return Mgr;
}

auto VulkanDescriptorManager::GetDescriptorPool() const -> vk::DescriptorPool {
    return *m_Pool;
}

auto VulkanDescriptorManager::Shutdown() -> void {
    m_SetCache.clear();
    m_BindlessTextureIndices.clear();
    m_BindlessPendingIds.clear();
    m_BindlessTextures.clear();
    m_BindlessSets.clear();
    m_BindlessSetLayout = nullptr;
    m_Pool              = nullptr;
    m_Device            = nullptr;
    m_DebugUtils        = nullptr;
}

auto VulkanDescriptorManager::RegisterBindlessTextures(std::span<const RHIRef<RHITexture>> Textures)
    -> std::expected<void, ErrorMessage> {
    if (Textures.empty())
        return {};
    std::vector<RHIRef<RHITexture>> Snapshot{Textures.begin(), Textures.end()};
    auto                            EnqueueResult =
        TaskGraph::Get().EnqueueFrameTask(ThreadQueue::RHI, [this, Textures = std::move(Snapshot)]() mutable {
            RegisterBindlessTexturesOnRhiThread(Textures);
        });
    if (!EnqueueResult)
        return std::unexpected(EnqueueResult.error().Append("Bindless texture registration task enqueue failed"));
    return {};
}

auto VulkanDescriptorManager::FindBindlessTextureIndex(const RHIRef<RHITexture>& Texture) const
    -> std::optional<Uint32> {
    if (!Texture)
        return std::nullopt;
    std::scoped_lock Lock(m_BindlessMutex);
    if (const auto It = m_BindlessTextureIndices.find(Texture->GetId()); It != m_BindlessTextureIndices.end())
        return It->second;
    return std::nullopt;
}

auto VulkanDescriptorManager::RegisterBindlessTexturesOnRhiThread(std::span<const RHIRef<RHITexture>> Textures)
    -> void {
    VulkanDescriptorWriteBatch             Batch     = {};
    std::vector<std::pair<Uint64, Uint32>> Published = {};
    {
        std::scoped_lock Lock(m_BindlessMutex);
        for (const auto& Texture : Textures) {
            auto* Payload = Texture.TryGet();
            if (!Payload || !magic_enum::enum_flags_test(Payload->GetUsage(), RHITextureUsage::Sampled))
                continue;
            const Uint64 Id = Payload->GetId();
            if (m_BindlessTextureIndices.contains(Id) || m_BindlessPendingIds.contains(Id))
                continue;
            if (m_NextBindlessTexture >= GetBindlessDescriptorCount()) {
                LogError("VulkanDescriptorManager: bindless texture capacity exhausted");
                continue;
            }
            const Uint32 Index = m_NextBindlessTexture++;
            m_BindlessPendingIds.insert(Id);
            RHITextureView View{.Texture = Texture, .Usage = RHITextureUsage::Sampled};
            Batch.AppendTextureWrite(GetBindlessDescriptorSet(),
                                     BindlessTextureBinding,
                                     Index,
                                     static_cast<const VulkanTexture&>(*Payload),
                                     View,
                                     vk::DescriptorType::eSampledImage);
            m_BindlessTextures.emplace(Id, Texture);
            Published.emplace_back(Id, Index);
        }
    }
    if (!Batch.Writes.empty())
        Batch.Apply(*m_Device);
    {
        std::scoped_lock Lock(m_BindlessMutex);
        for (const auto& [Id, Index] : Published) {
            m_BindlessTextureIndices.emplace(Id, Index);
            m_BindlessPendingIds.erase(Id);
        }
    }
}

auto VulkanDescriptorManager::AllocateDescriptorSets(std::span<const vk::DescriptorSetLayout> Layouts,
                                                     std::optional<Uint32>                    VariableCount)
    -> std::expected<std::vector<vk::raii::DescriptorSet>, ErrorMessage> {
    if (Layouts.empty())
        return std::unexpected(ErrorMessage("VulkanDescriptorManager: descriptor set layout list is empty"));

    // Pool allocation requires external synchronization; binding sets may be created on the render thread
    // (per-view sets) while the RHI thread allocates for pipeline binding sets.
    std::scoped_lock              Lock(m_PoolMutex);
    vk::DescriptorSetAllocateInfo AllocateInfo{
        .descriptorPool     = *m_Pool,
        .descriptorSetCount = static_cast<Uint32>(Layouts.size()),
        .pSetLayouts        = Layouts.data(),
    };
    std::expected<std::vector<vk::raii::DescriptorSet>, ErrorMessage> Result =
        std::unexpected(ErrorMessage("VulkanDescriptorManager: failed to allocate descriptor sets"));

    if (VariableCount) {
        const Uint32                                         Counts[] = {*VariableCount};
        vk::DescriptorSetVariableDescriptorCountAllocateInfo VariableCountsInfo{
            .descriptorSetCount = 1,
            .pDescriptorCounts  = Counts,
        };
        vk::StructureChain<vk::DescriptorSetAllocateInfo, vk::DescriptorSetVariableDescriptorCountAllocateInfo> Chain{
            AllocateInfo, VariableCountsInfo};
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
            m_DebugUtils->SetObjectName(*Set, Format("Internal/DescriptorSet/Persistent/Set{}", m_NextPersistentSet++));
        Sets.push_back(std::move(Set));
    }
    return Sets;
}

auto VulkanDescriptorManager::AcquireDescriptorSet(const DescriptorSetCacheKey&               Key,
                                                   std::span<const RHIShaderBindingSet::Slot> Bindings)
    -> vk::DescriptorSet {
    // Step 1: Check cache for existing set. Refresh LRU and return if found
    if (const auto It = m_SetCache.find(Key); It != m_SetCache.end()) {
        It->second.LruAge = 0;
        return *It->second.Set;
    }

    // Step 2: Cache miss, so allocate a new descriptor set and write all bindings. Unbounded array layouts
    // allocate with a variable descriptor count equal to the highest bound element index + 1 (partially bound
    // covers the rest).
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
    const auto Layout    = Key.Layout;
    auto       Allocated = AllocateDescriptorSets(std::span{&Layout, 1}, VariableCount);
    if (!Allocated) {
        LogError("{}", Allocated.error().ToString());
        return nullptr;
    }

    // Step 3: Write the new descriptor sets. The batch owns every payload its writes point at until
    // updateDescriptorSets consumes them.
    auto                       Set    = std::move((*Allocated)[0]);
    const auto                 Handle = *Set;
    VulkanDescriptorWriteBatch Batch;
    Batch.Writes.reserve(Bindings.size());
    for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
        const auto& Binding = Bindings[Index];
        const auto  Type    = ToVkDescriptorType(Binding.Info.Type, Binding.Info.Access);
        // Array slots write one descriptor per bound element.
        if (!Binding.ArrayElements.empty()) {
            for (const auto& Element : Binding.ArrayElements) {
                switch (Binding.Info.Type) {
                case ShaderResourceType::Texture:
                    if (const auto* Texture = static_cast<const VulkanTexture*>(Element.View.Texture.TryGet()))
                        Batch.AppendTextureWrite(
                            Handle, Binding.Info.BindingIndex, Element.ArrayIndex, *Texture, Element.View, Type);
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
        case ShaderResourceType::StorageBuffer:
            if (const auto* View = std::get_if<RHIBufferView>(&Binding.Bound))
                if (const auto* Buffer = static_cast<const VulkanBuffer*>(View->Buffer.TryGet()))
                    Batch.AppendBufferWrite(Handle, Binding.Info.BindingIndex, *Buffer, Type);
            break;
        case ShaderResourceType::Texture:
            if (const auto* View = std::get_if<RHITextureView>(&Binding.Bound))
                if (const auto* Texture = static_cast<const VulkanTexture*>(View->Texture.TryGet()))
                    Batch.AppendTextureWrite(Handle, Binding.Info.BindingIndex, 0, *Texture, *View, Type);
            break;
        case ShaderResourceType::Sampler:
            if (const auto* View = std::get_if<RHISamplerView>(&Binding.Bound))
                if (const auto* Sampler = static_cast<const VulkanSampler*>(View->Sampler.TryGet()))
                    Batch.AppendSamplerWrite(Handle, Binding.Info.BindingIndex, *Sampler);
            break;
        case ShaderResourceType::AccelerationStructure:
            if (const auto* View = std::get_if<RHITlasView>(&Binding.Bound))
                if (const auto* Tlas = static_cast<const VulkanTopLevelAccelerationStructure*>(View->Tlas.TryGet()))
                    Batch.AppendAccelerationStructureWrite(Handle, Binding.Info.BindingIndex, *Tlas);
            break;
        case ShaderResourceType::Unknown:
            break;
        }
    }
    if (!Batch.Writes.empty())
        Batch.Apply(*m_Device);
    m_SetCache.emplace(Key,
                       CacheEntry{
                           .Set    = std::move(Set),
                           .LruAge = 0,
                       });
    return Handle;
}

auto VulkanDescriptorManager::Tick() -> void {
    for (auto It = m_SetCache.begin(); It != m_SetCache.end();) {
        ++It->second.LruAge;
        if (It->second.LruAge >= s_CacheLifetimeTicks)
            It = m_SetCache.erase(It);
        else
            ++It;
    }
}

} // namespace SoulEngine
