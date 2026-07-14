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

    auto operator()(const SetGraphicsPipelineCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const PushConstantsCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const BindShaderParametersCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
        StampShaderParameters(Cmd.Parameters);
    }
    auto operator()(const DrawIndexedCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
        if (Cmd.VertexBufferPtr)
            Cmd.VertexBufferPtr->UpdateLastUsageToken(CurrentToken);
        if (Cmd.IndexBufferPtr)
            Cmd.IndexBufferPtr->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const DrawCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
        if (Cmd.VertexBufferPtr)
            Cmd.VertexBufferPtr->UpdateLastUsageToken(CurrentToken);
    }

    // Commands that don't reference GPU resources — explicit empty overloads
    auto operator()(const SetViewportCmd&) -> void {}
    auto operator()(const SetFullViewportCmd&) -> void {}
    auto operator()(const SetScissorCmd&) -> void {}
    auto operator()(const SetFullScissorRectCmd&) -> void {}

    /// @brief Stamp usage tokens on render targets referenced by the pass
    ///        descriptor. Called once per pass, before visiting commands.
    auto StampPassAttachments(const RenderingDesc& Desc) -> void {
        if (Desc.ColorAttachment.TexturePtr)
            Desc.ColorAttachment.TexturePtr->UpdateLastUsageToken(CurrentToken);
        if (Desc.DepthAttachment.has_value() && Desc.DepthAttachment->TexturePtr)
            Desc.DepthAttachment->TexturePtr->UpdateLastUsageToken(CurrentToken);
    }

    /// @brief Stamp usage token on the final frame output used for presentation.
    auto StampPresentSource(RenderTarget* Source) -> void {
        if (Source)
            Source->UpdateLastUsageToken(CurrentToken);
    }

  private:
    auto StampShaderParameters(const ShaderParameters& Parameters) -> void {
        for (const auto& Set : Parameters.GetSets()) {
            for (const auto& Value : Set.GetValues()) {
                std::visit(
                    [this](const auto& TypedValue) -> void {
                        using ValueType = std::decay_t<decltype(TypedValue)>;
                        if constexpr (std::same_as<ValueType, SampledTexture*>) {
                            if (TypedValue)
                                TypedValue->UpdateLastUsageToken(CurrentToken);
                        } else if constexpr (std::same_as<ValueType, ResourceArray<SampledTexture>>) {
                            for (auto* Resource : TypedValue.GetResources()) {
                                if (Resource)
                                    Resource->UpdateLastUsageToken(CurrentToken);
                            }
                        } else if constexpr (std::same_as<ValueType, Sampler*>) {
                            if (TypedValue)
                                TypedValue->UpdateLastUsageToken(CurrentToken);
                        }
                    },
                    Value);
            }
        }
    }
};

} // namespace SoulEngine::RHI
