/// @file   ShaderCompilerFacade.cpp
/// @brief  ShaderCompiler module implementation.

module ShaderCompiler.Facade;

import ShaderCompiler.Types;
import magic_enum;
import std;
import Core;
import Shader;

namespace SoulEngine {

[[nodiscard]] auto ShaderCompiler::CompileGraphics(const GraphicsCompileDesc& Desc)
    -> std::expected<ShaderGraphicsProgram, ErrorMessage> {
    ValidateEntryBackendConsistency(Desc.Vertex);
    ValidateEntryBackendConsistency(Desc.Fragment);
    if (Desc.Vertex.Backend != Desc.Fragment.Backend) {
        return std::unexpected(ErrorMessage("Graphics shader compile requires matching vertex/fragment backends"));
    }

    auto&           Slot = m_Backends[static_cast<std::size_t>(Desc.Vertex.Backend)];
    std::lock_guard Lock(Slot.Mutex);

    if (!Slot.Instance) {
        auto Inst = CreateBackend(Desc.Vertex.Backend);
        if (!Inst)
            return std::unexpected(std::move(Inst.error()));
        Slot.Instance = std::move(*Inst);
    }

    return Slot.Instance->CompileGraphics(Desc);
}

[[nodiscard]] auto ShaderCompiler::CompileCompute(const ComputeCompileDesc& Desc)
    -> std::expected<ShaderComputeProgram, ErrorMessage> {
    if (Desc.Compute.EntryPoint.empty())
        return std::unexpected(ErrorMessage("Compute shader compile requires a non-empty entry point"));

    ValidateEntryBackendConsistency(Desc.Compute);
    auto&           Slot = m_Backends[static_cast<std::size_t>(Desc.Compute.Backend)];
    std::lock_guard Lock(Slot.Mutex);
    if (!Slot.Instance) {
        auto Inst = CreateBackend(Desc.Compute.Backend);
        if (!Inst)
            return std::unexpected(std::move(Inst.error()));
        Slot.Instance = std::move(*Inst);
    }
    return Slot.Instance->CompileCompute(Desc);
}

[[nodiscard]] auto ShaderCompiler::CompileRayTracing(const RayTracingCompileDesc& Desc)
    -> std::expected<ShaderRayTracingProgram, ErrorMessage> {
    if (Desc.RayGeneration.EntryPoint.empty())
        return std::unexpected(ErrorMessage("Ray-tracing shader compile requires a ray-generation entry point"));

    const auto Backend         = Desc.RayGeneration.Backend;
    auto       ValidateBackend = [this, Backend](const ShaderEntry& Entry) -> std::expected<void, ErrorMessage> {
        ValidateEntryBackendConsistency(Entry);
        if (Entry.Backend != Backend)
            return std::unexpected(ErrorMessage("Ray-tracing shader compile requires matching entry-point backends"));
        return {};
    };

    if (auto R = ValidateBackend(Desc.RayGeneration); !R)
        return std::unexpected(std::move(R.error()));
    for (const auto& Entry : Desc.MissEntries)
        if (auto R = ValidateBackend(Entry); !R)
            return std::unexpected(std::move(R.error()));
    for (const auto& Group : Desc.HitGroups) {
        if (Group.ClosestHit.has_value())
            if (auto R = ValidateBackend(*Group.ClosestHit); !R)
                return std::unexpected(std::move(R.error()));
        if (Group.AnyHit.has_value())
            if (auto R = ValidateBackend(*Group.AnyHit); !R)
                return std::unexpected(std::move(R.error()));
        if (Group.Intersection.has_value())
            if (auto R = ValidateBackend(*Group.Intersection); !R)
                return std::unexpected(std::move(R.error()));
    }
    for (const auto& Entry : Desc.CallableEntries)
        if (auto R = ValidateBackend(Entry); !R)
            return std::unexpected(std::move(R.error()));

    auto&           Slot = m_Backends[static_cast<std::size_t>(Backend)];
    std::lock_guard Lock(Slot.Mutex);
    if (!Slot.Instance) {
        auto Inst = CreateBackend(Backend);
        if (!Inst)
            return std::unexpected(std::move(Inst.error()));
        Slot.Instance = std::move(*Inst);
    }

    return Slot.Instance->CompileRayTracing(Desc);
}

auto ShaderCompiler::ValidateEntryBackendConsistency(const ShaderEntry& Entry) -> void {
    auto ExpectedExt = StringView{};
    switch (Entry.Backend) {
    case ShaderBackend::Slang:
        ExpectedExt = ".slang";
        break;
    default:
        break;
    }
    if (!ExpectedExt.empty() && Entry.SourcePath.extension() != ExpectedExt)
        LogWarning("Source '{}' has extension '{}', expected '{}' for ShaderBackend::{}",
                   Entry.SourcePath.string(),
                   Entry.SourcePath.extension().string(),
                   ExpectedExt,
                   magic_enum::enum_name(Entry.Backend));
}

[[nodiscard]] auto ShaderCompiler::CreateBackend(ShaderBackend Kind)
    -> std::expected<UPtr<IShaderBackend>, ErrorMessage> {
    auto Name = magic_enum::enum_name(Kind);
    auto Inst = ShaderBackendFactory::Get().Create(Name);
    if (!Inst)
        return std::unexpected(ErrorMessage(Format("Internal error: no backend registered for '{}'", Name)));
    return Inst;
}

} // namespace SoulEngine
