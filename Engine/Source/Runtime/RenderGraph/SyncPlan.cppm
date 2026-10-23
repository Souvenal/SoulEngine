module;

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module RenderGraph.SyncPlan;

import RenderGraph.Types;
import Core;
import RHI;

import magic_enum;
import std;

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
