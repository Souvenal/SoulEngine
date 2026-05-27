/// @file   ShaderCompiler.cppm
/// @brief  Main ShaderCompiler module — singleton facade over per-language
///         compiler backends.
///
/// Usage:
///   auto Result = ShaderCompiler::Get().Compile(Desc);
///
/// Thread safety: each compiler backend has its own mutex.  Compiling
/// concurrently; same-language compilations are serialized.
///
/// Backend selection: explicit Backend enum in CompileDesc.  File extension
/// is validated against the enum value (warning on mismatch) but never overrides it.
///
/// Backend registration: backend modules (e.g., Slang) self-register with
/// BackendFactory via AutoRegistrar constructors when the shared library
/// is loaded.  ShaderCompiler never references concrete backend types directly.
/// Adding a new backend requires zero changes to this file.

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
///   auto Result    = Compiler.Compile(Desc);
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

    /// @brief Compile shader source to one or more ShaderPrograms.
    ///
    /// Selects the backend by Backend enum, initializes it lazily if
    /// this is the first use, and serializes access to it through its
    /// per-backend mutex.
    ///
    /// Returns one ShaderProgram per reflected entry point.
    /// When EntryPointName is set, returns exactly one program on success.
    /// When EntryPointName is empty, returns all reflected entry points
    /// sharing the same compiled bytecode.
    [[nodiscard]] auto Compile(const CompileDesc& Desc) -> std::expected<std::vector<Shader::Program>, ErrorMessage> {
        // Validate path/backend consistency (warning-only), then dispatch.
        ValidateBackendConsistency(Desc);

        return CompileWithBackend(Desc.Backend, Desc);
    }

  private:
    ShaderCompiler()  = default;
    ~ShaderCompiler() = default;

    // ── Backend router ──────────────────────────────────────────────

    /// When Source is a filesystem Path, check that its extension is
    /// consistent with the canonical one for Desc.Backend.  On mismatch
    /// log a warning but still use the Backend enum as the source of truth.
    /// Inline-source (StringView) mode has no extension to validate.
    auto ValidateBackendConsistency(const CompileDesc& Desc) -> void {
        auto* P = std::get_if<Path>(&Desc.Source);
        if (!P)
            return; // inline source — nothing to validate

        auto ExpectedExt = StringView{};
        switch (Desc.Backend) {
        case Backend::Slang:
            ExpectedExt = ".slang";
            break;
        default:
            break;
        }
        if (!ExpectedExt.empty() && P->extension() != ExpectedExt)
            LogWarning("Source '{}' has extension '{}', expected '{}' for Backend::{}",
                       P->string(),
                       P->extension().string(),
                       ExpectedExt,
                       magic_enum::enum_name(Desc.Backend));
    }

    /// Ensure the backend for a language is initialized (lazy) and
    /// forward the compile request under its per-backend lock.
    [[nodiscard]] auto CompileWithBackend(Backend Backend, const CompileDesc& Desc)
        -> std::expected<std::vector<Shader::Program>, ErrorMessage> {
        auto&           Slot = m_Backends[static_cast<std::size_t>(Backend)];
        std::lock_guard Lock(Slot.Mutex);

        if (!Slot.Instance) {
            auto Inst = CreateBackend(Backend);
            if (!Inst)
                return std::unexpected(std::move(Inst.error()));
            Slot.Instance = std::move(*Inst);
        }

        return Slot.Instance->Compile(Desc);
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
