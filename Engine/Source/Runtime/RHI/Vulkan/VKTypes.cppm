module;

#include <vk_mem_alloc.h>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module Vulkan:Types;

import vulkan;

import RHI;

import :Capability;

namespace SoulEngine {

// Conversion helpers

[[nodiscard]] auto ToVkFormat(RHIFormat Fmt) -> vk::Format;

/// @brief Native buffer access mask from the two declaration axes: Usage picks the mechanism,
/// Access picks the direction.
///
/// Direction participates only for bidirectional mechanisms (Storage — RMW arrives as Read|Write);
/// unidirectional usages carry an implied direction and ignore Access. Accumulates over the usage union, so
/// both the creation union (upload batch) and single-role view declarations (pass prologue) map correctly.
/// The declared direction translates faithfully — a read-modify-writing shader declares Read|Write on the
/// view.
///
/// Not a pure flag mapping: vertex/index buffers additionally feed acceleration structure builds when ray
/// tracing is available (mirrors ToVkBufferUsage).
[[nodiscard]] auto ToVkBufferAccess(RHIBufferUsage Usage, RHIAccess Access) -> vk::AccessFlags2;

/// @brief Texture counterpart of ToVkBufferAccess. RenderTarget/Storage are bidirectional and
/// participate in direction; Sampled and the transfer usages carry an implied direction and ignore Access.
/// DepthStencil attachment coverage is always Read|Write — a load reads the previous depth and the store
/// writes, regardless of the declared direction. Accumulates over the usage union: merged prologue views
/// arrive as multi-role unions and every role contributes its mask.
[[nodiscard]] auto ToVkTextureAccess(RHITextureUsage Usage, RHIAccess Access) -> vk::AccessFlags2;

/// @brief Image layout for a view's declared role and direction. A single-role usage maps to its
/// optimal layout; storage and every multi-role union share only General; a depth/stencil view without Write
/// is a read-only attachment. This is the single place where layout exists: the RHI vocabulary carries no
/// layout axis.
[[nodiscard]] auto ToVkImageLayout(RHITextureUsage Usage, RHIAccess Access) -> vk::ImageLayout;

[[nodiscard]] auto ToVkImageAspect(RHIFormat Fmt) -> vk::ImageAspectFlags;

/// @brief RHI-neutral sync vocabulary -> native Vulkan pipeline stage flags.
/// The mapping is total; the graph-side mapping table decides precision.
[[nodiscard]] auto ToVkPipelineStages(RHISyncStage Stage) -> vk::PipelineStageFlags2;

} // namespace SoulEngine
