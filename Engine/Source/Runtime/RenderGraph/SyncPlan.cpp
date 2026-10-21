module;

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

module RenderGraph.SyncPlan;

import RenderGraph.Types;
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

[[nodiscard]] auto ShaderStageFor(RHIPassType Class) -> RHISyncStage {
    switch (Class) {
    case RHIPassType::Graphics:
        return RHISyncStage::AllGraphics;
    case RHIPassType::Compute:
        return RHISyncStage::ComputeShader;
    case RHIPassType::RayTracing:
        return RHISyncStage::RayTracingShader;
    case RHIPassType::Transfer:
        return RHISyncStage::Transfer;
    default:
        return RHISyncStage::None;
    }
}

[[nodiscard]] auto BufferStageFor(RHIBufferUsage Usage, RHIPassType Class) -> RHISyncStage {
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Indirect))
        // The indirect buffer may be produced by an earlier compute pass (e.g. GPU culling); the DrawIndirect
        // stage is exact here.
        return RHISyncStage::DrawIndirect;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Constant) ||
        magic_enum::enum_flags_test(Usage, RHIBufferUsage::Storage))
        return ShaderStageFor(Class);
    return RHISyncStage::None;
}

[[nodiscard]] auto TextureStageFor(RHITextureUsage Usage, RHIPassType Class) -> RHISyncStage {
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::RenderTarget))
        return RHISyncStage::ColorAttachmentOutput;
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::DepthStencil))
        // Depth attachment writes, including the final store operation, may occur in either fragment-test
        // stage. This is synchronization coverage, not a request to force Early-Z or Late-Z execution.
        return RHISyncStage::EarlyFragmentTests | RHISyncStage::LateFragmentTests;
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::TransferSrc) ||
        magic_enum::enum_flags_test(Usage, RHITextureUsage::TransferDst))
        return RHISyncStage::Transfer;
    return ShaderStageFor(Class);
}

[[nodiscard]] auto BuildPassPrologue(const RGPassNode& Node, const std::vector<RGResourceEntry>& Resources)
    -> RHIBarrierList {
    RHIBarrierList Prologue = {};

    const auto AddBuffer = [&](const RGBufferViewData* Decl) {
        const auto& Resource = Resources[Decl->Buffer.Index];
        const auto* Prop     = std::get_if<RGBufferProp>(&Resource.Prop);
        if (!Prop)
            return;
        if (magic_enum::enum_flags_test(Decl->Usage, RHIBufferUsage::Constant) ||
            magic_enum::enum_flags_test(Decl->Usage, RHIBufferUsage::TransferDst))
            return; // uniform-belt buffers are covered by the host-write
                    // visibility barrier; copy-dst targets are readback buffers
        const auto Stage = BufferStageFor(Decl->Usage, Node.StageClass);
        for (auto& T : Prologue.Buffers) {
            if (T.View.Buffer == Prop->Resolved) {
                T.View.Usage  = T.View.Usage | Decl->Usage;
                T.View.Access = T.View.Access | Decl->Access;
                T.Stage       = T.Stage | Stage;
                return;
            }
        }
        Prologue.Buffers.push_back(
            {.View = {.Buffer = Prop->Resolved, .Access = Decl->Access, .Usage = Decl->Usage}, .Stage = Stage});
    };
    const auto AddTexture = [&](const RGTextureViewData* Decl) {
        const auto& Resource = Resources[Decl->Texture.Index];
        const auto* Prop     = std::get_if<RGTextureProp>(&Resource.Prop);
        if (!Prop)
            return;
        const auto Stage = TextureStageFor(Decl->Usage, Node.StageClass);
        for (auto& T : Prologue.Textures) {
            if (T.View.Texture == Prop->Resolved && T.View.Range.BaseMipLevel == Decl->Range.BaseMipLevel &&
                T.View.Range.MipLevelCount == Decl->Range.MipLevelCount) {
                T.View.Usage  = T.View.Usage | Decl->Usage;
                T.View.Access = T.View.Access | Decl->Access;
                T.Stage       = T.Stage | Stage;
                return;
            }
        }
        Prologue.Textures.push_back(
            {.View  = {.Texture = Prop->Resolved, .Access = Decl->Access, .Usage = Decl->Usage, .Range = Decl->Range},
             .Stage = Stage});
    };

    for (const auto* Decl : Node.Buffers)
        AddBuffer(Decl);
    for (const auto* Decl : Node.Textures)
        AddTexture(Decl);
    return Prologue;
}

} // namespace SoulEngine
