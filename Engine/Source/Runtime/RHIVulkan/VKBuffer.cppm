module;

#include <vk_mem_alloc.h>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module RHIVulkan.Buffer;

import Core;
import TaskGraph;
import vulkan;
import RHI;
import std;

import RHIVulkan.Capability;
import RHIVulkan.Types;
import RHIVulkan.Context;
import RHIVulkan.Debug;
import RHIVulkan.Semaphore;
import RHIVulkan.RingBuffer;
import RHIVulkan.Staging;

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
export class VulkanUniformBelt {
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
        -> std::expected<VulkanUniformBelt, ErrorMessage>;

    /// @brief Bump-allocate a range for the current frame, checked against maxUniformBufferRange.
    ///
    /// The offset must fit the Uint32 dynamic-offset range; with any sane belt capacity this is structurally
    /// guaranteed, so the check is a cheap invariant guard.
    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage>;

    /// @brief Frame-begin relay: bump the generation (invalidating earlier slices), then retire the
    /// markers the device timeline has passed and seal the previous frame's pending allocations.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void;

    /// @brief Current frame generation; slices stamp it at creation, uploads compare against it.
    [[nodiscard]] auto GetGeneration() const -> Uint32;

  private:
    VulkanRingBuffer m_Ring       = {}; ///< backing ring
    Uint32           m_Generation = 0;  ///< frame generation; a bump invalidates earlier slices
};

/// @brief Transient shader-storage lane: a typed facade over VulkanRingBuffer.
///
/// Serves per-frame storage/indirect data rebuilt during command execution. Same ring, growth,
/// dynamic-binding, and generation-guard contract as the uniform belt; instances additionally carry the
/// indirect-buffer usage bit for draw-indirect slices. RHI-thread confined.
export class VulkanShaderStorageBelt {
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
        -> std::expected<VulkanShaderStorageBelt, ErrorMessage>;

    /// @brief Bump-allocate a range for the current frame, checked against maxStorageBufferRange.
    ///
    /// The offset must fit the Uint32 dynamic-offset range; with any sane belt capacity this is structurally
    /// guaranteed, so the check is a cheap invariant guard.
    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage>;

    /// @brief Frame-begin relay: bump the generation (invalidating earlier slices), then retire the
    /// markers the device timeline has passed and seal the previous frame's pending allocations.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void;

    /// @brief Current frame generation; slices stamp it at creation, uploads compare against it.
    [[nodiscard]] auto GetGeneration() const -> Uint32;

  private:
    VulkanRingBuffer m_Ring       = {}; ///< backing ring
    Uint32           m_Generation = 0;  ///< frame generation; a bump invalidates earlier slices
};

/// Pure RHIBufferUsage → vk::BufferUsageFlags mapping: no implicit bits.
export [[nodiscard]] auto ToVkBufferUsage(RHIBufferUsage Usage) -> vk::BufferUsageFlags;

/// @brief Persistent-flavor state: the buffer owns its VMA allocation.
///
/// GpuOnly and CpuToGpu share this exact member shape — they differ only in values (baked device address or
/// zero) and behavior (upload path), never in layout.
export struct PersistentBufferProp {
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
export struct TransientBufferProp {
    RingAllocation Allocation         = {}; ///< backing belt range (handle, mapped base, offset)
    /// @brief Belt generation stamped at slice creation.
    ///
    /// A cross-frame upload targets a range the belt may already have recycled or handed to a new frame, so
    /// the upload task compares generations and rejects mismatches.
    Uint32         CreationGeneration = 0;
};

/// @brief Unified backend buffer: one identity, two lifetime flavors as variant state.
///
/// Persistent flavors (GpuOnly / CpuToGpu, split by Desc.MemoryUsage) own their allocation in
/// PersistentBufferProp; the transient belt-slice flavor borrows its range in TransientBufferProp. The
/// variant makes mixed-flavor state unrepresentable; GetProp<T>() is the only access path to flavor state.
/// The host-visible readback ring below is a separate class.
export class VulkanBuffer final : public RHIBuffer {
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
    VulkanBuffer(String Name, const RHIBufferDesc& Desc, PersistentBufferProp Prop);

    /// @brief Transient-flavor construction: pure assignment, cannot fail.
    ///
    /// Constructed by the manager's frame task after the belt resolves this frame's range. The Prop caches
    /// the whole RingAllocation — buffer handle, mapped base, and offset — so a mid-frame ring growth (two
    /// live instances) cannot make the range ambiguous; the descriptor range is exactly the allocation's size
    /// — a wider range would expose neighboring slices to validation.
    VulkanBuffer(String Name, const RHIBufferDesc& Desc, TransientBufferProp Prop);

    /// @brief Destruction rides the backend deferred deletion queue.
    ///
    /// A virtual timeline event keeps the handles valid until the next real submission completes (the last
    /// GPU use cannot be later than the destruction frame's own submission).
    ~VulkanBuffer() override;

    /// @brief Flavor-state access: the requested Prop, or nullptr when the buffer is the other
    /// flavor. The only access path to flavor state.
    template <typename T>
    [[nodiscard]] auto GetProp() const -> const T* {
        return std::get_if<T>(&m_Prop);
    }

    /// @brief Native handle — the backing belt instance buffer for transient slices.
    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer;
    [[nodiscard]] auto GetDeviceAddress() const noexcept -> Uint64 override;

  private:
    std::variant<PersistentBufferProp, TransientBufferProp> m_Prop = {}; ///< lifetime-flavor state
};

/// @brief The single home of buffer concerns.
///
/// Desc validation and creation dispatch (persistent / transient belt slice), uploads, and the transient
/// belts. GpuOnly upload bytes stage through the device-owned staging belt. The render device owns one
/// instance and forwards; belts are RHI-thread confined (touched by frame tasks and Execute only).
export class VulkanBufferManager final {
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
                                     VulkanStagingBelt& Staging) -> std::expected<VulkanBufferManager, ErrorMessage>;

    /// @brief Release the VMA-backed belts while the allocator is still alive.
    ///
    /// Device shutdown ordering requires this to run before the allocator is destroyed.
    auto Shutdown() -> void;

    /// @brief Single creation entry: validate the Desc, then dispatch on Lifetime × MemoryUsage.
    ///
    /// The Desc contract is validated here at the top (backend-local: each backend decides its own
    /// tolerance). Persistent flavors allocate synchronously on the calling thread (VMA locks internally,
    /// vkCreateBuffer is device-level) and go Ready at return; transient slices materialize on the RHI
    /// thread's frame task.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  buffer description; Lifetime × MemoryUsage selects the flavor.
    [[nodiscard]] auto CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage>;

    /// @brief Upload a full buffer: snapshot the bytes, then hand a frame-affined task to the RHI thread.
    ///
    /// The task (drained at the matching RHI frame, before Execute) writes host-visible buffers directly and
    /// stages GpuOnly uploads for the frame-start batch; Execute records the whole batch at frame start.
    /// Frame affinity makes the batch atomic per producer frame: all of frame N's uploads land together.
    /// @param Buffer  destination buffer ref.
    /// @param Data    full-buffer byte snapshot; its size must equal the buffer size.
    [[nodiscard]] auto Upload(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage>;

    /// @brief Tick-chain relay: forward the frame-begin timeline value to the transient belts.
    ///
    /// Each belt bumps its generation (invalidating earlier slices), retires the ring markers the device
    /// timeline has passed — destroying superseded instances whose stamp has passed — and seals the previous
    /// frame's pending allocations against the frame-begin value.
    /// @param FrameBeginTimelineValue  host-reserved frame-begin value, issued by the device Tick.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void;

  private:
    /// @brief Transient-slice upload: write the belt range in place through its persistent mapping.
    ///
    /// The generation guard rejects cross-frame uploads: a transient ref retained past its creation frame (a
    /// contract violation the frame-task ordinals cannot see) would otherwise corrupt a recycled or in-flight
    /// range. Belts are contractually coherent, so no flush is needed.
    /// @param Buffer  transient-flavor payload, dispatched by Upload's frame task.
    /// @param Data    full-buffer byte snapshot; size already validated against the buffer size.
    auto UploadTransientBuffer(VulkanBuffer& Buffer, std::span<const std::byte> Data) -> void;

    /// @brief Persistent upload: CpuToGpu copies straight into the allocation; GpuOnly stages through
    /// the ring.
    ///
    /// RHI thread only, reached through Upload's frame-affined task; the caller owns the WAR contract against
    /// in-flight GPU reads (CpuToGpu is a low-frequency-write flavor — per-frame data belongs in the belts).
    /// VMA handles the flush for non-coherent memory. GpuOnly uploads stage for the frame-start batch, which
    /// records the copy; the next frame-begin Tick seals the retirement marker.
    /// @param Buffer  persistent-flavor payload, dispatched by Upload's frame task.
    /// @param Data    full-buffer byte snapshot; size already validated against the buffer size.
    auto UploadPersistentBuffer(VulkanBuffer& Buffer, std::span<const std::byte> Data) -> void;

    /// @brief The Desc contract, validated once at the CreateBuffer entry.
    ///
    /// Backend-local on purpose: tolerance is a backend decision.
    /// @param Desc  buffer description to validate.
    [[nodiscard]] static auto ValidateDesc(const RHIBufferDesc& Desc) -> std::expected<void, ErrorMessage>;

    /// @brief Persistent flavor dispatch: synchronous allocation, published Ready at return.
    ///
    /// Thread-safe because VMA locks internally and vkCreateBuffer is device-level. The ref goes Ready at
    /// creation so callers can bake the device address immediately; bytes are uploaded separately through
    /// RHIRenderDevice::UploadBuffer.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  buffer description (GpuOnly or CpuToGpu placement).
    [[nodiscard]] auto CreatePersistentBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage>;

    /// @brief Transient slice dispatch: the frame task resolves the belt slice.
    ///
    /// Pure allocation — bytes flow through Upload. No host-write barrier exists: belts are contractually
    /// coherent and host writes are implicitly visible to later submissions.
    /// @param Name  logical RHI name.
    /// @param Desc  buffer description, already validated at the CreateBuffer entry; this branch owns only
    ///              belt orchestration.
    [[nodiscard]] auto CreateTransientBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage>;

    const VulkanResourceContext* m_Context     = nullptr; ///< borrowed resource context
    VulkanUniformBelt            m_UniformBelt = {};      ///< per-frame uniform lane
    VulkanShaderStorageBelt      m_StorageBelt = {};      ///< per-frame shader-storage lane
    VulkanStagingBelt*           m_Staging     = nullptr; ///< borrowed; owned by the render device
};

} // namespace SoulEngine
