/// @file   ShaderCompiler.cppm
/// @brief  Main ShaderCompiler module — singleton facade over per-language
///         compiler backends.
///
/// Usage:
///   auto Result = ShaderCompiler::Get().CompileGraphics(Desc);
///
/// Thread safety: each compiler backend has its own mutex.  Compiling
/// concurrently; same-language compilations are serialized.
///
/// ShaderBackend selection: explicit ShaderBackend enum in shader compile descriptors. File extension
/// is validated against the enum value (warning on mismatch) but never overrides it.
///
/// ShaderBackend registration: backend modules (e.g., Slang) self-register with
/// ShaderBackendFactory via AutoRegistrar constructors when the shared library
/// is loaded.  ShaderCompiler never references concrete backend types directly.
/// Adding a new backend requires zero changes to this file.

module;

export module ShaderCompiler;

export import :Types;

import magic_enum;
import std;
import Core;
import Shader;

export import Shader;

export namespace SoulEngine {

/// @brief Singleton facade over shader-language compiler backends.
///
/// Holds one backend slot per supported source language and initializes
/// each lazily on first use.  Each slot carries its own mutex so that
/// compilations in different languages can run concurrently.
///
/// Obtain via:
///   auto& Compiler = ShaderCompiler::Get();
///   auto Result    = Compiler.CompileGraphics(Desc);
class ShaderCompiler : public Singleton<ShaderCompiler> {
    friend class Singleton<ShaderCompiler>;

    // ── Internal types ──────────────────────────────────────────────

    struct BackendSlot {
        std::mutex     Mutex;
        UPtr<IShaderBackend> Instance = nullptr;
    };

  public:
    ShaderCompiler(const ShaderCompiler&)                    = delete;
    auto operator=(const ShaderCompiler&) -> ShaderCompiler& = delete;
    ShaderCompiler(ShaderCompiler&&)                         = delete;
    auto operator=(ShaderCompiler&&) -> ShaderCompiler&      = delete;

    /// @brief Compile a graphics-pipeline shader combination and pipeline-level reflection.
    [[nodiscard]] auto CompileGraphics(const GraphicsCompileDesc& Desc)
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

    [[nodiscard]] auto CompileCompute(const ComputeCompileDesc& Desc)
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

    [[nodiscard]] auto CompileRayTracing(const RayTracingCompileDesc& Desc)
        -> std::expected<ShaderRayTracingProgram, ErrorMessage> {
        if (Desc.RayGeneration.EntryPoint.empty())
            return std::unexpected(ErrorMessage("Ray-tracing shader compile requires a ray-generation entry point"));

        const auto Backend = Desc.RayGeneration.Backend;
        auto ValidateBackend = [this, Backend](const ShaderEntry& Entry) -> std::expected<void, ErrorMessage> {
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

  private:
    ShaderCompiler()  = default;
    ~ShaderCompiler() = default;

    // ── ShaderBackend router ──────────────────────────────────────────────

    auto ValidateEntryBackendConsistency(const ShaderEntry& Entry) -> void {
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

    /// Create a new backend instance via the factory.
    /// The factory registry is populated by AutoRegistrar instances in
    /// each backend's translation unit.
    [[nodiscard]] auto CreateBackend(ShaderBackend Kind) -> std::expected<UPtr<IShaderBackend>, ErrorMessage> {
        auto Name = magic_enum::enum_name(Kind);
        auto Inst = ShaderBackendFactory::Get().Create(Name);
        if (!Inst)
            return std::unexpected(ErrorMessage(Format("Internal error: no backend registered for '{}'", Name)));
        return Inst;
    }

    std::array<BackendSlot, magic_enum::enum_count<ShaderBackend>()> m_Backends;
};

} // namespace SoulEngine
