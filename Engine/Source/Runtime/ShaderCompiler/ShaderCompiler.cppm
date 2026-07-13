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
/// Backend selection: explicit Backend enum in shader compile descriptors. File extension
/// is validated against the enum value (warning on mismatch) but never overrides it.
///
/// Backend registration: backend modules (e.g., Slang) self-register with
/// BackendFactory via AutoRegistrar constructors when the shared library
/// is loaded.  ShaderCompiler never references concrete backend types directly.
/// Adding a new backend requires zero changes to this file.

module;

#include <magic_enum/magic_enum.hpp>

export module ShaderCompiler;

export import :Types;

import std;
import Core;
import Shader;

export import Shader;

using namespace SoulEngine::Core;

export namespace SoulEngine::ShaderCompiler {

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
        UPtr<IBackend> Instance = nullptr;
    };

  public:
    ShaderCompiler(const ShaderCompiler&)                    = delete;
    auto operator=(const ShaderCompiler&) -> ShaderCompiler& = delete;
    ShaderCompiler(ShaderCompiler&&)                         = delete;
    auto operator=(ShaderCompiler&&) -> ShaderCompiler&      = delete;

    /// @brief Compile a graphics-pipeline shader combination and pipeline-level reflection.
    [[nodiscard]] auto CompileGraphics(const GraphicsCompileDesc& Desc)
        -> std::expected<Shader::GraphicsProgram, ErrorMessage> {
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

  private:
    ShaderCompiler()  = default;
    ~ShaderCompiler() = default;

    // ── Backend router ──────────────────────────────────────────────

    auto ValidateEntryBackendConsistency(const ShaderEntry& Entry) -> void {
        auto ExpectedExt = StringView{};
        switch (Entry.Backend) {
        case Backend::Slang:
            ExpectedExt = ".slang";
            break;
        default:
            break;
        }
        if (!ExpectedExt.empty() && Entry.SourcePath.extension() != ExpectedExt)
            LogWarning("Source '{}' has extension '{}', expected '{}' for Backend::{}",
                       Entry.SourcePath.string(),
                       Entry.SourcePath.extension().string(),
                       ExpectedExt,
                       magic_enum::enum_name(Entry.Backend));
    }

    /// Create a new backend instance via the factory.
    /// The factory registry is populated by AutoRegistrar instances in
    /// each backend's translation unit.
    [[nodiscard]] auto CreateBackend(Backend Backend) -> std::expected<UPtr<IBackend>, ErrorMessage> {
        auto Name = magic_enum::enum_name(Backend);
        auto Inst = BackendFactory::Get().Create(Name);
        if (!Inst)
            return std::unexpected(ErrorMessage(Format("Internal error: no backend registered for '{}'", Name)));
        return Inst;
    }

    std::array<BackendSlot, magic_enum::enum_count<Backend>()> m_Backends;
};

} // namespace SoulEngine::ShaderCompiler
