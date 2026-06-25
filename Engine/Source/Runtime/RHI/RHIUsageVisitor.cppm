export module RHI:UsageVisitor;

export import :Command;
export import :Types;

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

/// @brief Visits every RHI::Command variant and updates the LastUsageToken
///        on any GpuResource-derived resource it references.
///
/// Future command variants that reference GPU resources MUST add an overload
/// here — the compiler will error on any uncovered variant, preventing silent
/// omission of usage tracking.
struct UsageVisitor {
    GpuCompletionToken CurrentToken = {};

    auto operator()(const SetPipelineCmd& Cmd) -> void {
        if (Cmd.Pipeline)
            Cmd.Pipeline->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const BindVertexBufferCmd& Cmd) -> void {
        if (Cmd.Buffer)
            Cmd.Buffer->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const BindIndexBufferCmd& Cmd) -> void {
        if (Cmd.Buffer)
            Cmd.Buffer->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const SetDrawMaterialDataCmd& Cmd) -> void {
        if (Cmd.Material.TestTexture)
            Cmd.Material.TestTexture->UpdateLastUsageToken(CurrentToken);
    }

    // Commands that don't reference GPU resources — explicit empty overloads
    auto operator()(const SetViewportCmd&) -> void {}
    auto operator()(const SetFullViewportCmd&) -> void {}
    auto operator()(const SetScissorCmd&) -> void {}
    auto operator()(const SetFullScissorRectCmd&) -> void {}
    auto operator()(const DrawIndexedCmd&) -> void {}
    auto operator()(const DrawCmd&) -> void {}
};

} // namespace SoulEngine::RHI
