module;

#include <vk_mem_alloc.h>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module Vulkan:Buffer;

import Core;
import TaskGraph;
import vulkan;
import RHI;
import std;

import :Capability;
import :Types;
import :Context;
import :Debug;
import :Semaphore;
import :RingBuffer;
import :Staging;

namespace SoulEngine {

/// @brief Transient uniform-buffer lane: a typed facade over VulkanRingBuffer.
///
/// Serves per-frame uniform data that must stay distinct between individual draws. Allocation is bump-fast
/// and grow-only (same contract as the staging ring); slices bind through dynamic descriptors with the ring
/// range's absolute offset. The belt bumps a generation counter at every frame-begin Tick and slices carry
/// their creation generation: a cross-frame upload — a transient ref retained past its creation frame, a
/// contract violation the frame-task ordinals cannot see because the misused upload's task ordinal still
/// matches the current frame — is detected and dropped instead of corrupting a recycled or in-flight range.
/// RHI-thread confined.
class VulkanUniformBelt {
  public:
    VulkanUniformBelt()                                                = default;
    VulkanUniformBelt(const VulkanUniformBelt&)                        = delete;
    auto operator=(const VulkanUniformBelt&) -> VulkanUniformBelt&     = delete;
    VulkanUniformBelt(VulkanUniformBelt&&) noexcept                    = default;
    auto operator=(VulkanUniformBelt&&) noexcept -> VulkanUniformBelt& = default;

    /// @brief Create the belt with its ring, aligned to minUniformBufferOffsetAlignment.
    /// @param Context   resource context providing the VMA allocator, debug utils, and the device timeline.
    /// @param Name      debug name applied to every ring instance (duplicates are legal).
    /// @param Capacity  total ring capacity in bytes (the RHI.Vulkan.UniformBeltSize CVar); the ring
    ///                  grows on demand, so this is a starting budget, not a hard limit.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, StringView Name, Uint64 Capacity)
        -> std::expected<VulkanUniformBelt, ErrorMessage> {
        const auto Alignment = std::max(
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minUniformBufferOffsetAlignment),
            VulkanRingBuffer::kMinAlignment);
        auto Ring =
            VulkanRingBuffer::Create(Context, Name, vk::BufferUsageFlagBits::eUniformBuffer, Capacity, Alignment);
        if (!Ring)
            return std::unexpected(Ring.error().Append("VulkanUniformBelt: ring creation failed"));
        VulkanUniformBelt Belt;
        Belt.m_Ring = std::move(*Ring);
        return Belt;
    }

    /// @brief Bump-allocate a range for the current frame, checked against maxUniformBufferRange.
    ///
    /// The offset must fit the Uint32 dynamic-offset range; with any sane belt capacity this is structurally
    /// guaranteed, so the check is a cheap invariant guard.
    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("VulkanUniformBelt::Allocate: size must be greater than zero"));
        const auto MaxRange = static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxUniformBufferRange);
        if (Size > MaxRange)
            return std::unexpected(ErrorMessage(
                Format("VulkanUniformBelt allocation exceeds maxUniformBufferRange ({} bytes > {})", Size, MaxRange)));
        auto Alloc = m_Ring.Allocate(Size);
        if (!Alloc)
            return std::unexpected(Alloc.error().Append("VulkanUniformBelt::Allocate: ring allocation failed"));
        if (Alloc->Offset > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanUniformBelt allocation offset exceeds dynamic offset range"));
        return *Alloc;
    }

    /// @brief Frame-begin relay: bump the generation (invalidating earlier slices), then retire the
    /// markers the device timeline has passed and seal the previous frame's pending allocations.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void {
        ++m_Generation;
        m_Ring.Tick(FrameBeginTimelineValue);
    }

    /// @brief Current frame generation; slices stamp it at creation, uploads compare against it.
    [[nodiscard]] auto GetGeneration() const -> Uint32 {
        return m_Generation;
    }

  private:
    VulkanRingBuffer m_Ring       = {}; ///< backing ring
    Uint32           m_Generation = 0;  ///< frame generation; a bump invalidates earlier slices
};

/// @brief Transient shader-storage lane: a typed facade over VulkanRingBuffer.
///
/// Serves per-frame storage/indirect data rebuilt during command execution. Same ring, growth,
/// dynamic-binding, and generation-guard contract as the uniform belt; instances additionally carry the
/// indirect-buffer usage bit for draw-indirect slices. RHI-thread confined.
class VulkanShaderStorageBelt {
  public:
    VulkanShaderStorageBelt()                                                      = default;
    VulkanShaderStorageBelt(const VulkanShaderStorageBelt&)                        = delete;
    auto operator=(const VulkanShaderStorageBelt&) -> VulkanShaderStorageBelt&     = delete;
    VulkanShaderStorageBelt(VulkanShaderStorageBelt&&) noexcept                    = default;
    auto operator=(VulkanShaderStorageBelt&&) noexcept -> VulkanShaderStorageBelt& = default;

    /// @brief Create the belt with its ring, aligned to minStorageBufferOffsetAlignment.
    /// @param Context   resource context providing the VMA allocator, debug utils, and the device timeline.
    /// @param Name      debug name applied to every ring instance (duplicates are legal).
    /// @param Capacity  total ring capacity in bytes (the RHI.Vulkan.ShaderStorageBeltSize CVar); the ring
    ///                  grows on demand, so this is a starting budget, not a hard limit.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, StringView Name, Uint64 Capacity)
        -> std::expected<VulkanShaderStorageBelt, ErrorMessage> {
        const auto Alignment = std::max(
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minStorageBufferOffsetAlignment),
            VulkanRingBuffer::kMinAlignment);
        auto Ring =
            VulkanRingBuffer::Create(Context,
                                     Name,
                                     vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
                                     Capacity,
                                     Alignment);
        if (!Ring)
            return std::unexpected(Ring.error().Append("VulkanShaderStorageBelt: ring creation failed"));
        VulkanShaderStorageBelt Belt;
        Belt.m_Ring = std::move(*Ring);
        return Belt;
    }

    /// @brief Bump-allocate a range for the current frame, checked against maxStorageBufferRange.
    ///
    /// The offset must fit the Uint32 dynamic-offset range; with any sane belt capacity this is structurally
    /// guaranteed, so the check is a cheap invariant guard.
    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("VulkanShaderStorageBelt::Allocate: size must be greater than zero"));
        const auto MaxRange = static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxStorageBufferRange);
        if (Size > MaxRange)
            return std::unexpected(ErrorMessage(Format(
                "VulkanShaderStorageBelt allocation exceeds maxStorageBufferRange ({} bytes > {})", Size, MaxRange)));
        auto Alloc = m_Ring.Allocate(Size);
        if (!Alloc)
            return std::unexpected(Alloc.error().Append("VulkanShaderStorageBelt::Allocate: ring allocation failed"));
        if (Alloc->Offset > std::numeric_limits<Uint32>::max())
            return std::unexpected(
                ErrorMessage("VulkanShaderStorageBelt allocation offset exceeds dynamic offset range"));
        return *Alloc;
    }

    /// @brief Frame-begin relay: bump the generation (invalidating earlier slices), then retire the
    /// markers the device timeline has passed and seal the previous frame's pending allocations.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void {
        ++m_Generation;
        m_Ring.Tick(FrameBeginTimelineValue);
    }

    /// @brief Current frame generation; slices stamp it at creation, uploads compare against it.
    [[nodiscard]] auto GetGeneration() const -> Uint32 {
        return m_Generation;
    }

  private:
    VulkanRingBuffer m_Ring       = {}; ///< backing ring
    Uint32           m_Generation = 0;  ///< frame generation; a bump invalidates earlier slices
};

/// Pure RHIBufferUsage → vk::BufferUsageFlags mapping: no implicit bits.
[[nodiscard]] inline auto ToVkBufferUsage(RHIBufferUsage Usage) -> vk::BufferUsageFlags {
    auto Out = vk::BufferUsageFlags{};
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Vertex))
        Out |= vk::BufferUsageFlagBits::eVertexBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Index))
        Out |= vk::BufferUsageFlagBits::eIndexBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Constant))
        Out |= vk::BufferUsageFlagBits::eUniformBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Storage))
        Out |= vk::BufferUsageFlagBits::eStorageBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Indirect))
        Out |= vk::BufferUsageFlagBits::eIndirectBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::TransferSrc))
        Out |= vk::BufferUsageFlagBits::eTransferSrc;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::TransferDst))
        Out |= vk::BufferUsageFlagBits::eTransferDst;
    // Geometry inputs feed acceleration-structure builds (old vertex/index classes did this unconditionally
    // when ray tracing is available).
    if ((magic_enum::enum_flags_test(Usage, RHIBufferUsage::Vertex) ||
         magic_enum::enum_flags_test(Usage, RHIBufferUsage::Index)) &&
        VulkanCapability::Get().IsRayTracingAvailable())
        Out |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
    return Out;
}

/// @brief Persistent-flavor state: the buffer owns its VMA allocation.
///
/// GpuOnly and CpuToGpu share this exact member shape — they differ only in values (baked device address or
/// zero) and behavior (upload path), never in layout.
struct PersistentBufferProp {
    VmaAllocator                 Allocator     = nullptr; ///< allocator that produced Allocation
    VulkanDeferredDeletionQueue* DeferredQueue = nullptr; ///< borrowed destruction queue, owned by the resource context
    vk::Buffer                   Buffer        = nullptr; ///< native handle
    VmaAllocation                Allocation    = nullptr; ///< owned VMA allocation
    vk::DeviceAddress            DeviceAddress = 0;       ///< baked device address (GpuOnly; 0 for CpuToGpu)
};

/// @brief Transient-flavor state: a borrowed belt-ring slice.
///
/// Owns nothing — the backing buffer belongs to the belt, which retires the range after the GPU passes it;
/// destruction is a no-op for this flavor. Belt instances are persistently mapped and contractually
/// host-coherent: callers write the range directly, no flush is needed.
struct TransientBufferProp {
    RingAllocation Allocation         = {}; ///< backing belt range (handle, mapped base, offset)
    /// Belt generation stamped at slice creation. A cross-frame upload targets a range the belt may already
    /// have recycled or handed to a new frame, so the upload task compares generations and rejects
    /// mismatches.
    Uint32         CreationGeneration = 0;
};

/// @brief Unified backend buffer: one identity, two lifetime flavors as variant state.
///
/// Persistent flavors (GpuOnly / CpuToGpu, split by Desc.MemoryUsage) own their allocation in
/// PersistentBufferProp; the transient belt-slice flavor borrows its range in TransientBufferProp. The
/// variant makes mixed-flavor state unrepresentable; GetProp<T>() is the only access path to flavor state.
/// The host-visible readback ring below is a separate class.
class VulkanBuffer final : public RHIBuffer {
  public:
    VulkanBuffer(const VulkanBuffer&)                    = delete;
    auto operator=(const VulkanBuffer&) -> VulkanBuffer& = delete;
    VulkanBuffer(VulkanBuffer&&)                         = delete;
    auto operator=(VulkanBuffer&&) -> VulkanBuffer&      = delete;

    /// @brief Persistent-flavor construction: pure assignment, cannot fail.
    ///
    /// All fallible work (VMA allocation, device-address bake, debug naming) lives in
    /// VulkanBufferManager::CreatePersistentBuffer; this constructor only adopts the finished Prop. Public
    /// for the manager's make_unique; m_Prop never mutates after construction.
    VulkanBuffer(String Name, const RHIBufferDesc& Desc, PersistentBufferProp Prop)
        : RHIBuffer(std::move(Name), Desc.SizeBytes, Desc.Usage, Desc.MemoryUsage, Desc.Lifetime) {
        m_Prop = std::move(Prop);
    }

    /// @brief Transient-flavor construction: pure assignment, cannot fail.
    ///
    /// Constructed by the manager's frame task after the belt resolves this frame's range. The Prop caches
    /// the whole RingAllocation — buffer handle, mapped base, and offset — so a mid-frame ring growth (two
    /// live instances) cannot make the range ambiguous; the descriptor range is exactly the allocation's size
    /// — a wider range would expose neighboring slices to validation.
    VulkanBuffer(String Name, const RHIBufferDesc& Desc, TransientBufferProp Prop)
        : RHIBuffer(std::move(Name), Desc.SizeBytes, Desc.Usage, Desc.MemoryUsage, Desc.Lifetime) {
        m_Prop = std::move(Prop);
    }

    /// Destruction rides the backend deferred deletion queue: a virtual timeline event keeps the handles
    /// valid until the next real submission completes (the last GPU use cannot be later than the destruction
    /// frame's own submission).
    ~VulkanBuffer() override {
        if (const auto* Prop = GetProp<PersistentBufferProp>(); Prop && Prop->Allocation)
            Prop->DeferredQueue->Enqueue(Prop->Buffer, Prop->Allocation);
    }

    /// @brief Flavor-state access: the requested Prop, or nullptr when the buffer is the other
    /// flavor. The only access path to flavor state.
    template <typename T>
    [[nodiscard]] auto GetProp() const -> const T* {
        return std::get_if<T>(&m_Prop);
    }

    /// @brief Native handle — the backing belt instance buffer for transient slices.
    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        if (const auto* Prop = GetProp<TransientBufferProp>())
            return Prop->Allocation.Buffer;
        return std::get<PersistentBufferProp>(m_Prop).Buffer;
    }
    [[nodiscard]] auto GetDeviceAddress() const noexcept -> Uint64 override {
        if (const auto* Prop = GetProp<PersistentBufferProp>())
            return static_cast<Uint64>(Prop->DeviceAddress);
        return 0;
    }

  private:
    std::variant<PersistentBufferProp, TransientBufferProp> m_Prop = {}; ///< lifetime-flavor state
};

/// @brief The single home of buffer concerns.
///
/// Desc validation and creation dispatch (persistent / transient belt slice), uploads, and the transient
/// belts. GpuOnly upload bytes stage through the device-owned staging belt. The render device owns one
/// instance and forwards; belts are RHI-thread confined (touched by frame tasks and Execute only).
class VulkanBufferManager final {
  public:
    VulkanBufferManager()                                              = default;
    VulkanBufferManager(const VulkanBufferManager&)                    = delete;
    auto operator=(const VulkanBufferManager&) -> VulkanBufferManager& = delete;
    VulkanBufferManager(VulkanBufferManager&&)                         = default;
    auto operator=(VulkanBufferManager&&) -> VulkanBufferManager&      = default;

    /// @brief Create the manager with its transient belts.
    /// @param Context                  resource context providing the VMA allocator, device, and debug utils.
    /// @param UniformBeltCapacity      total uniform belt ring capacity, in bytes.
    /// @param StorageBeltCapacity      total shader-storage belt ring capacity, in bytes.
    /// @param Staging                  device-owned staging belt, borrowed for GpuOnly upload staging.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context,
                                     Uint64                       UniformBeltCapacity,
                                     Uint64                       StorageBeltCapacity,
                                     VulkanStagingBelt& Staging) -> std::expected<VulkanBufferManager, ErrorMessage> {
        VulkanBufferManager Manager;
        Manager.m_Context = &Context;
        auto Uniform      = VulkanUniformBelt::Create(Context, "Internal/Buffer/UniformBelt", UniformBeltCapacity);
        if (!Uniform)
            return std::unexpected(Uniform.error().Append("VulkanBufferManager: uniform belt creation failed"));
        Manager.m_UniformBelt = std::move(*Uniform);
        auto Storage =
            VulkanShaderStorageBelt::Create(Context, "Internal/Buffer/ShaderStorageBelt", StorageBeltCapacity);
        if (!Storage)
            return std::unexpected(Storage.error().Append("VulkanBufferManager: shader storage belt creation failed"));
        Manager.m_StorageBelt = std::move(*Storage);
        Manager.m_Staging     = &Staging;
        return Manager;
    }

    /// @brief Release the VMA-backed belts while the allocator is still alive.
    ///
    /// Device shutdown ordering requires this to run before the allocator is destroyed.
    auto Shutdown() -> void {
        m_StorageBelt = {};
        m_UniformBelt = {};
        m_Staging     = nullptr;
        m_Context     = nullptr;
    }

    /// @brief Single creation entry: validate the Desc, then dispatch on Lifetime × MemoryUsage.
    ///
    /// The Desc contract is validated here at the top (backend-local: each backend decides its own
    /// tolerance). Persistent flavors allocate synchronously on the calling thread (VMA locks internally,
    /// vkCreateBuffer is device-level) and go Ready at return; transient slices materialize on the RHI
    /// thread's frame task.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  buffer description; Lifetime × MemoryUsage selects the flavor.
    [[nodiscard]] auto CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> {
        if (auto Valid = ValidateDesc(Desc); !Valid)
            return std::unexpected(Valid.error());
        if (Desc.Lifetime == RHILifetime::Transient)
            return CreateTransientBuffer(Name, Desc);
        return CreatePersistentBuffer(Name, Desc);
    }

    /// @brief Upload a full buffer: snapshot the bytes, then hand a frame-affined task to the RHI thread.
    ///
    /// The task (drained at the matching RHI frame, before Execute) writes host-visible buffers directly and
    /// stages GpuOnly uploads for the frame-start batch; Execute records the whole batch at frame start.
    /// Frame affinity makes the batch atomic per producer frame: all of frame N's uploads land together.
    /// @param Buffer  destination buffer ref.
    /// @param Data    full-buffer byte snapshot; its size must equal the buffer size.
    [[nodiscard]] auto Upload(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> {
        if (Data.empty())
            return std::unexpected(ErrorMessage("UploadBuffer: data is empty"));
        // Fail-fast validation when the payload already exists (persistent buffers are Ready at creation).
        // Transient slices materialize on the RHI thread, so their validation is deferred to the upload task
        // — the frame task queue is FIFO, so the creation task of a same-frame create+upload pair always runs
        // first.
        if (const auto* Payload = Buffer.TryGet()) {
            if (Data.size_bytes() != Payload->GetSize())
                return std::unexpected(ErrorMessage(Format("UploadBuffer: data size {} does not match buffer size {}",
                                                           Data.size_bytes(),
                                                           Payload->GetSize())));
        }

        auto EnqueueResult = TaskGraph::Get().EnqueueFrameTask(
            ThreadQueue::RHI,
            [this, Buffer = std::move(Buffer), Snapshot = std::vector<std::byte>{Data.begin(), Data.end()}]() mutable {
                auto* TaskPayload = Buffer.TryGet();
                if (!TaskPayload) {
                    LogWarning("UploadBuffer: buffer ref is not Ready when the upload task ran");
                    return;
                }
                if (Snapshot.size() != TaskPayload->GetSize()) {
                    LogWarning("UploadBuffer: data size {} does not match buffer size {}",
                               Snapshot.size(),
                               TaskPayload->GetSize());
                    return;
                }
                auto& VulkanBuf = static_cast<VulkanBuffer&>(*TaskPayload);
                if (VulkanBuf.GetLifetime() == RHILifetime::Transient) {
                    UploadTransientBuffer(VulkanBuf, Snapshot);
                    return;
                }
                UploadPersistentBuffer(VulkanBuf, Snapshot);
            });
        if (!EnqueueResult)
            return std::unexpected(EnqueueResult.error().Append("UploadBuffer: frame task enqueue failed"));
        return {};
    }

    /// @brief Tick-chain relay: forward the frame-begin timeline value to the transient belts.
    ///
    /// Each belt bumps its generation (invalidating earlier slices), retires the ring markers the device
    /// timeline has passed — destroying superseded instances whose stamp has passed — and seals the previous
    /// frame's pending allocations against the frame-begin value.
    /// @param FrameBeginTimelineValue  host-reserved frame-begin value, issued by the device Tick.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void {
        m_UniformBelt.Tick(FrameBeginTimelineValue);
        m_StorageBelt.Tick(FrameBeginTimelineValue);
    }

  private:
    /// @brief Transient-slice upload: write the belt range in place through its persistent mapping.
    ///
    /// The generation guard rejects cross-frame uploads: a transient ref retained past its creation frame (a
    /// contract violation the frame-task ordinals cannot see) would otherwise corrupt a recycled or in-flight
    /// range. Belts are contractually coherent, so no flush is needed.
    /// @param Buffer  transient-flavor payload, dispatched by Upload's frame task.
    /// @param Data    full-buffer byte snapshot; size already validated against the buffer size.
    auto UploadTransientBuffer(VulkanBuffer& Buffer, std::span<const std::byte> Data) -> void {
        const auto* Prop = Buffer.GetProp<TransientBufferProp>();
        if (!Prop) {
            LogWarning("UploadBuffer: buffer '{}' has no transient state", Buffer.GetName());
            return;
        }
        const bool   IsUniform  = magic_enum::enum_flags_test(Buffer.GetUsage(), RHIBufferUsage::Constant);
        const Uint32 Generation = IsUniform ? m_UniformBelt.GetGeneration() : m_StorageBelt.GetGeneration();
        if (Prop->CreationGeneration != Generation) {
            LogWarning("UploadBuffer: transient buffer '{}' belongs to a previous belt generation — "
                       "the upload is dropped",
                       Buffer.GetName());
            return;
        }
        std::memcpy(Prop->Allocation.MappedData, Data.data(), Data.size());
    }

    /// @brief Persistent upload: CpuToGpu copies straight into the allocation; GpuOnly stages through
    /// the ring.
    ///
    /// RHI thread only, reached through Upload's frame-affined task; the caller owns the WAR contract against
    /// in-flight GPU reads (CpuToGpu is a low-frequency-write flavor — per-frame data belongs in the belts).
    /// VMA handles the flush for non-coherent memory. GpuOnly uploads stage for the frame-start batch, which
    /// records the copy; the next frame-begin Tick seals the retirement marker.
    /// @param Buffer  persistent-flavor payload, dispatched by Upload's frame task.
    /// @param Data    full-buffer byte snapshot; size already validated against the buffer size.
    auto UploadPersistentBuffer(VulkanBuffer& Buffer, std::span<const std::byte> Data) -> void {
        const auto* Prop = Buffer.GetProp<PersistentBufferProp>();
        if (!Prop) {
            LogWarning("UploadBuffer: buffer '{}' has no persistent state", Buffer.GetName());
            return;
        }
        // Host-visible flavor: memory copies stay on the RHI thread; no staging, no frame-start batch entry.
        if (Buffer.GetMemoryUsage() == RHIMemoryUsage::CpuToGpu) {
            if (vmaCopyMemoryToAllocation(Prop->Allocator, Data.data(), Prop->Allocation, 0, Data.size()) != VK_SUCCESS)
                LogWarning("UploadBuffer: host write failed for '{}'", Buffer.GetName());
            return;
        }
        m_Staging->EnqueueBufferUpload(Buffer.GetName(), Buffer.GetVkBuffer(), Buffer.GetUsage(), Data);
    }

    /// @brief The Desc contract, validated once at the CreateBuffer entry.
    ///
    /// Backend-local on purpose: tolerance is a backend decision.
    /// @param Desc  buffer description to validate.
    [[nodiscard]] static auto ValidateDesc(const RHIBufferDesc& Desc) -> std::expected<void, ErrorMessage> {
        if (Desc.SizeBytes == 0)
            return std::unexpected(ErrorMessage("RHIBufferDesc: size is zero"));
        if (Desc.Usage == RHIBufferUsage::Unknown)
            return std::unexpected(ErrorMessage("RHIBufferDesc: usage is Unknown — a Desc is explicit"));
        if (Desc.Lifetime == RHILifetime::Unknown)
            return std::unexpected(ErrorMessage("RHIBufferDesc: lifetime is Unknown — a Desc is explicit"));
        if (Desc.Lifetime == RHILifetime::Transient) {
            // Transient slices live in the host-coherent belts — CpuToGpu is the only honest memory domain.
            // Pinning it keeps the axis explicit: no Unknown loophole, no silent GpuOnly lie.
            if (Desc.MemoryUsage != RHIMemoryUsage::CpuToGpu)
                return std::unexpected(
                    ErrorMessage("RHIBufferDesc: transient buffers are belt slices — memory usage must be CpuToGpu"));
            // Only two belt lanes exist today: uniform (Constant) and shader storage (Storage). Indirect is
            // an orthogonal usage the storage ring already carries, not a lane of its own. "Not uniform" must
            // never silently mean "storage" — a usage with neither lane bit has no belt and is rejected.
            constexpr auto kLaneUsages = RHIBufferUsage::Constant | RHIBufferUsage::Storage;
            if (!magic_enum::enum_flags_test_any(Desc.Usage, kLaneUsages))
                return std::unexpected(
                    ErrorMessage("RHIBufferDesc: transient buffers currently allow only the Constant (uniform belt) or "
                                 "Storage (shader-storage belt) usage bits"));
            return {};
        }
        if (Desc.MemoryUsage == RHIMemoryUsage::Unknown)
            return std::unexpected(ErrorMessage("RHIBufferDesc: memory usage is Unknown — a Desc is explicit"));
        return {};
    }

    /// @brief Persistent flavor dispatch: synchronous allocation, published Ready at return.
    ///
    /// Thread-safe because VMA locks internally and vkCreateBuffer is device-level. The ref goes Ready at
    /// creation so callers can bake the device address immediately; bytes are uploaded separately through
    /// RHIRenderDevice::UploadBuffer.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  buffer description (GpuOnly or CpuToGpu placement).
    [[nodiscard]] auto CreatePersistentBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> {
        PersistentBufferProp Prop{.Allocator     = m_Context->GetAllocator(),
                                  .DeferredQueue = &m_Context->GetDeferredDeletionQueue()};

        // GpuOnly derives transfer-dst (upload is a transfer by definition) and device address (BDA costs
        // nothing at creation) from placement.
        vk::BufferUsageFlags VkUsage = ToVkBufferUsage(Desc.Usage);
        const bool           bDeviceAddress =
            Desc.MemoryUsage == RHIMemoryUsage::GpuOnly &&
            static_cast<bool>(
                VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress);
        if (Desc.MemoryUsage == RHIMemoryUsage::GpuOnly) {
            VkUsage |= vk::BufferUsageFlagBits::eTransferDst;
            if (bDeviceAddress)
                VkUsage |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
        }

        // GpuOnly uploads are recorded on the transfer lane while consumers run on graphics; cross-family
        // ownership rides an explicit queue-family transfer, so every buffer is created exclusive. TODO: emit
        // the transfer->graphics ownership transfer barriers.
        const vk::BufferCreateInfo BufCI{
            .size        = Desc.SizeBytes,
            .usage       = VkUsage,
            .sharingMode = vk::SharingMode::eExclusive,
        };

        // NOTE: VMA memory types are `AUTO`. To understand what `AUTO` means here, think about some concepts,
        //       like host/device visible and host/device memory.
        //
        //       Classic discrete graphics cards have no ReBAR, which means device memory is invisible to
        //       host, thus `host visible` == `host memory`. On the other hand, cards that have ReBAR own
        //       device memory that are visible to host.
        //
        //       GPU (or shader program) can access both device memory and host memory. The later one is much
        //       slower because it needs to went through PCIE. So:
        //       - For `GpuOnly` buffers, they prefer device memory, but host memory can do the work as well.
        //       - For `CpuToGpu` buffers, we need host visiblity. VMA `AUTO` does the work
        //         here: If device memory is visble to host, then it chooses device memory. If it isn't, we
        //         have to use host memory. (Why not use `PREFER_HOST`? Because we don't want host memory when
        //         device memory is visible to host)
        //
        //       We declare we need host visibilty by adding `HOST_ACCESS_xxx` bit in flags.
        const VmaAllocationCreateInfo AllocInfo =
            Desc.MemoryUsage == RHIMemoryUsage::GpuOnly
                ? VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE}
                : VmaAllocationCreateInfo{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                          .usage = VMA_MEMORY_USAGE_AUTO};

        const auto Created = vmaCreateBuffer(m_Context->GetAllocator(),
                                             reinterpret_cast<const VkBufferCreateInfo*>(&BufCI),
                                             &AllocInfo,
                                             reinterpret_cast<VkBuffer*>(&Prop.Buffer),
                                             &Prop.Allocation,
                                             nullptr);
        if (Created != VK_SUCCESS)
            return std::unexpected(
                ErrorMessage("VulkanBufferManager::CreatePersistentBuffer: buffer creation failed via VMA"));

        if (bDeviceAddress) {
            const auto AddressInfo = vk::BufferDeviceAddressInfo{.buffer = Prop.Buffer};
            Prop.DeviceAddress     = m_Context->GetDevice().getBufferAddress(AddressInfo);
        }

        m_Context->GetDebugUtils().SetObjectName(Prop.Buffer, Name);

        auto Resource = RHIRef<RHIBuffer>::Create();
        if (auto Publish = Resource.Publish(std::make_unique<VulkanBuffer>(String(Name), Desc, std::move(Prop)),
                                            RHIRefState::Ready);
            !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    /// @brief Transient slice dispatch: the frame task resolves the belt slice.
    ///
    /// Pure allocation — bytes flow through Upload. No host-write barrier exists: belts are contractually
    /// coherent and host writes are implicitly visible to later submissions.
    /// @param Name  logical RHI name.
    /// @param Desc  buffer description, already validated at the CreateBuffer entry; this branch owns only
    ///              belt orchestration.
    [[nodiscard]] auto CreateTransientBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> {
        auto Resource = RHIRef<RHIBuffer>::Create();

        auto EnqueueResult =
            TaskGraph::Get().EnqueueFrameTask(ThreadQueue::RHI, [this, Resource, Name = String(Name), Desc] mutable {
                const bool IsUniform = magic_enum::enum_flags_test(Desc.Usage, RHIBufferUsage::Constant);
                auto       Alloc =
                    IsUniform ? m_UniformBelt.Allocate(Desc.SizeBytes) : m_StorageBelt.Allocate(Desc.SizeBytes);
                if (!Alloc) {
                    auto Error = Alloc.error().Append("Transient buffer belt resolution failed");
                    LogError("{}", Error.ToString());
                    Resource.MarkFailed(std::move(Error));
                    return;
                }
                const Uint32 Generation = IsUniform ? m_UniformBelt.GetGeneration() : m_StorageBelt.GetGeneration();
                // Desc was validated and pinned (CpuToGpu, Transient) at the CreateBuffer entry — forward it
                // as-is.
                auto         Payload    = std::make_unique<VulkanBuffer>(
                    std::move(Name), Desc, TransientBufferProp{.Allocation = *Alloc, .CreationGeneration = Generation});
                if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish) {
                    auto Error = Publish.error().Append("Transient buffer publication failed");
                    LogError("{}", Error.ToString());
                    Resource.MarkFailed(std::move(Error));
                    return;
                }
            });
        if (!EnqueueResult) {
            Resource.MarkFailed(EnqueueResult.error());
            return std::unexpected(EnqueueResult.error());
        }
        return Resource;
    }

    const VulkanResourceContext* m_Context     = nullptr; ///< borrowed resource context
    VulkanUniformBelt            m_UniformBelt = {};      ///< per-frame uniform lane
    VulkanShaderStorageBelt      m_StorageBelt = {};      ///< per-frame shader-storage lane
    VulkanStagingBelt*           m_Staging     = nullptr; ///< borrowed; owned by the render device
};

} // namespace SoulEngine
