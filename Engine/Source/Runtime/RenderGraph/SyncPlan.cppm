module;

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module RenderGraph:SyncPlan;

import :Types;
import Core;
import RHI;

import magic_enum;
import std;

namespace SoulEngine {

// Stage mapping
// The prologue derives only the stage; the backend derives the native access mask and the image layout from
// the view's (Usage, Access) at emission. The declared direction translates faithfully: a shader that
// read-modify-writes (atomic append, counters) declares Read|Write on the view. Initial mapping is coarse:
// shader accesses in graphics passes map to AllGraphics; Transfer/Compute/Indirect/Attachment kinds map
// precisely. Refining precision is a table-data change, never a schema change.

} // namespace SoulEngine

namespace SoulEngine {

/// @brief Map one pass's declared accesses into its prologue: the complete,
/// per-resource-deduplicated set of dst-only state requirements.
///
/// Graph-created and imported resources both participate; uniform-belt buffers and readback copy targets stay
/// covered by backend-reserved sites (the host-write visibility barrier, the readback ring). Views of one
/// resource (and, for textures, one subresource range) merge into a single transition that accumulates the
/// usage, access, and stage union — a mixed union (e.g. Sampled | Storage on one range) is exactly what the
/// backend's layout derivation maps to its shared layout. Views over disjoint ranges of one texture produce
/// independent transitions, which the per-subresource tracker resolves at emission.
export [[nodiscard]] auto BuildPassPrologue(const RGPassNode& Node, const std::vector<RGResourceEntry>& Resources)
    -> RHIBarrierList;
} // namespace SoulEngine
