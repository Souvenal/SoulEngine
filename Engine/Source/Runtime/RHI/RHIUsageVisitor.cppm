module;

export module RHI:UsageVisitor;

export import :Command;
export import :RayTracing;
export import :Types;

export namespace SoulEngine {

/// @brief Visits every RHICommand variant and updates the LastUsageToken
///        on any RHIGpuResource-derived resource it references.
///
/// Future command variants that reference GPU resources MUST add an overload
/// here — the compiler will error on any uncovered variant, preventing silent
/// omission of usage tracking.
struct RHIUsageVisitor {
    RHIGpuCompletionToken CurrentToken = {};

    auto operator()(const RHISetGraphicsPipelineCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const RHISetRayTracingPipelineCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const RHIPushConstantsCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const RHIBindShaderParametersCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
        StampShaderParameters(Cmd.Parameters);
    }
    auto operator()(const RHIDrawIndexedCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
        for (auto* VertexBufferPtr : Cmd.VertexBuffers) {
            if (VertexBufferPtr)
                VertexBufferPtr->UpdateLastUsageToken(CurrentToken);
        }
        if (Cmd.IndexBufferPtr)
            Cmd.IndexBufferPtr->UpdateLastUsageToken(CurrentToken);
    }
    auto operator()(const RHIDrawCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
        for (auto* VertexBufferPtr : Cmd.VertexBuffers) {
            if (VertexBufferPtr)
                VertexBufferPtr->UpdateLastUsageToken(CurrentToken);
        }
    }
    auto operator()(const RHIUpdateRayTracingGeometryTableCmd& Cmd) -> void {
        for (const auto& Geometry : Cmd.Update.Geometries) {
            if (Geometry.PositionBuffer)
                Geometry.PositionBuffer->UpdateLastUsageToken(CurrentToken);
            if (Geometry.NormalBuffer)
                Geometry.NormalBuffer->UpdateLastUsageToken(CurrentToken);
            if (Geometry.IndexBuffer)
                Geometry.IndexBuffer->UpdateLastUsageToken(CurrentToken);
        }
    }
    auto operator()(const RHIBuildOrUpdateTopLevelAccelerationStructureCmd& Cmd) -> void {
        if (Cmd.TargetPtr)
            Cmd.TargetPtr->UpdateLastUsageToken(CurrentToken);
        for (const auto& Instance : Cmd.Instances) {
            if (Instance.BottomLevelPtr)
                Instance.BottomLevelPtr->UpdateLastUsageToken(CurrentToken);
        }
    }
    auto operator()(const RHITraceRaysCmd& Cmd) -> void {
        if (Cmd.PipelinePtr)
            Cmd.PipelinePtr->UpdateLastUsageToken(CurrentToken);
    }

    // Commands that don't reference GPU resources — explicit empty overloads
    auto operator()(const RHISetViewportCmd&) -> void {}
    auto operator()(const RHISetFullViewportCmd&) -> void {}
    auto operator()(const RHISetScissorCmd&) -> void {}
    auto operator()(const RHISetFullScissorRectCmd&) -> void {}

    /// @brief Stamp usage tokens on render targets referenced by the pass
    ///        descriptor. Called once per pass, before visiting commands.
    auto StampPassAttachments(const RHIRenderingDesc& Desc) -> void {
        if (Desc.ColorAttachment.TexturePtr)
            Desc.ColorAttachment.TexturePtr->UpdateLastUsageToken(CurrentToken);
        if (Desc.DepthAttachment.has_value() && Desc.DepthAttachment->TexturePtr)
            Desc.DepthAttachment->TexturePtr->UpdateLastUsageToken(CurrentToken);
    }

    /// @brief Stamp usage token on the final frame output used for presentation.
    auto StampPresentSource(RHIRenderTarget* Source) -> void {
        if (Source)
            Source->UpdateLastUsageToken(CurrentToken);
    }

  private:
    auto StampShaderParameters(const RHIShaderParameters& Parameters) -> void {
        for (const auto& Set : Parameters.GetSets()) {
            for (const auto& Value : Set.GetValues()) {
                std::visit(
                    [this](const auto& TypedValue) -> void {
                        using ValueType = std::decay_t<decltype(TypedValue)>;
                        if constexpr (std::same_as<ValueType, RHISampledTexture*>) {
                            if (TypedValue)
                                TypedValue->UpdateLastUsageToken(CurrentToken);
                        } else if constexpr (std::same_as<ValueType, RHIResourceArray<RHISampledTexture>>) {
                            for (auto* Resource : TypedValue.GetResources()) {
                                if (Resource)
                                    Resource->UpdateLastUsageToken(CurrentToken);
                            }
                        } else if constexpr (std::same_as<ValueType, RHIVertexBuffer*>) {
                            if (TypedValue)
                                TypedValue->UpdateLastUsageToken(CurrentToken);
                        } else if constexpr (std::same_as<ValueType, RHIIndexBuffer*>) {
                            if (TypedValue)
                                TypedValue->UpdateLastUsageToken(CurrentToken);
                        } else if constexpr (std::same_as<ValueType, RHISampler*>) {
                            if (TypedValue)
                                TypedValue->UpdateLastUsageToken(CurrentToken);
                        } else if constexpr (std::same_as<ValueType, RHITopLevelAccelerationStructure*>) {
                            if (TypedValue)
                                TypedValue->UpdateLastUsageToken(CurrentToken);
                        } else if constexpr (std::same_as<ValueType, RHIRayTracingGeometryTable*>) {
                            // Metadata buffers are host-written immediately before trace recording.
                            // Vulkan stamps their token only after the graphics submission succeeds.
                        } else if constexpr (std::same_as<ValueType, RHIRenderTarget*>) {
                            if (TypedValue)
                                TypedValue->UpdateLastUsageToken(CurrentToken);
                        }
                    },
                    Value);
            }
        }
    }
};

} // namespace SoulEngine
