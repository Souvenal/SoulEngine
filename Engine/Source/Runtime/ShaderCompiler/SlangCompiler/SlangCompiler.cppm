/// @file   SlangCompiler/SlangCompiler.cppm
/// @brief  Slang-backed concrete shader compiler backend.
///
/// Standalone module — self-registers with BackendFactory via static
/// AutoRegistrar when the shared library is loaded.
/// Slang SDK dependencies live in this file and the :Reflection partition.
///
/// Based on the official minimal example:
///   https://docs.shader-slang.org/en/latest/compilation-api.html
///
/// TODO(Session caching): currently a fresh slang::ISession is created for
/// every Compile() call.  Reusing an ISession across compiles would cache
/// parsed modules and compiled SPIR-V, at the cost of holding that memory
/// until the backend is destroyed.  The current usage pattern (compile all
/// shaders at startup, then done) doesn't need it, but it's a win for
/// runtime PSO generation or hot-reload.  To implement: move the session
/// creation into Init() (currently in Compile(), near "Create session"), store
/// the ISession as a member, and skip recreation in Compile().

module;

#include <slang.h>
// Slang follows the COM ABI convention (vtable layout, addRef/release lifecycle).
// Its COM-style interfaces require a dedicated smart pointer instead of
// unique_ptr/shared_ptr — hence slang-com-ptr.h provides Slang::ComPtr<T>.
#include <slang-com-ptr.h>

export module Slang;

import :Types;
import :Utils;
import :Reflection;

import std;
import Core;
import ShaderCompiler;

namespace SoulEngine::ShaderCompiler::SlangCompiler {

using namespace SoulEngine::Core;

namespace {

// ── Compilation paths ──────────────────────────────────────────────

/// Compile all entry points in a module at once (getTargetCode path).
[[nodiscard]] auto CompileModule(slang::IModule*                       Module,
                                 const CompileDesc&                    Desc,
                                 Slang::ComPtr<slang::IComponentType>& Linked,
                                 Slang::ComPtr<slang::IBlob>&          CodeBlob,
                                 Slang::ComPtr<slang::IBlob>&          DiagBlob) -> std::expected<void, ErrorMessage> {
    // Link the module without selecting a specific entry point.
    // Calling link() directly on the Module is valid because IModule
    // inherits from IComponentType.
    if (auto Rc = Module->link(Linked.writeRef(), DiagBlob.writeRef()); SLANG_FAILED(Rc) || !Linked) {
        if (const auto* P = std::get_if<Path>(&Desc.Source)) {
            return std::unexpected(
                ErrorMessage(Format("Failed to link module '{}': {}", P->string(), DiagView(DiagBlob))));
        }
        return std::unexpected(ErrorMessage(Format("Failed to link module: {}", DiagView(DiagBlob))));
    }

    // Compile all entry points into a single SPIR-V module.
    if (auto Rc = Linked->getTargetCode(0, CodeBlob.writeRef(), DiagBlob.writeRef()); SLANG_FAILED(Rc) || !CodeBlob) {
        if (const auto* P = std::get_if<Path>(&Desc.Source)) {
            return std::unexpected(
                ErrorMessage(Format("Failed to generate target code for '{}': {}", P->string(), DiagView(DiagBlob))));
        }
        return std::unexpected(ErrorMessage(Format("Failed to generate target code: {}", DiagView(DiagBlob))));
    }

    return {};
}

/// Compile a single entry point (findEntryPointByName + compose + link + getEntryPointCode).
[[nodiscard]] auto CompileEntryPoint(Slang::ComPtr<slang::ISession>&       Session,
                                     slang::IModule*                       Module,
                                     const CompileDesc&                    Desc,
                                     Slang::ComPtr<slang::IComponentType>& Linked,
                                     Slang::ComPtr<slang::IBlob>&          CodeBlob,
                                     Slang::ComPtr<slang::IBlob>& DiagBlob) -> std::expected<void, ErrorMessage> {
    // ── Find entry point by name ──────────────────────────────────────
    Slang::ComPtr<slang::IEntryPoint> EntryPoint;
    {
        if (auto Rc = Module->findEntryPointByName(String(Desc.EntryPointName.value()).c_str(), EntryPoint.writeRef());
            SLANG_FAILED(Rc) || !EntryPoint) {
            if (const auto* P = std::get_if<Path>(&Desc.Source)) {
                return std::unexpected(ErrorMessage(
                    Format("Entry point '{}' not found in '{}'", Desc.EntryPointName.value(), P->string())));
            }
            return std::unexpected(ErrorMessage(Format("Entry point '{}' not found", Desc.EntryPointName.value())));
        }
    }

    // ── Compose (module + entry point) ────────────────────────────────
    // Compose into a unified GPU program
    Slang::ComPtr<slang::IComponentType> Composite;
    {
        std::array<slang::IComponentType*, 2> Components{Module, EntryPoint.get()};
        if (auto Rc =
                Session->createCompositeComponentType(Components.data(), 2, Composite.writeRef(), DiagBlob.writeRef());
            SLANG_FAILED(Rc) || !Composite) {
            return std::unexpected(ErrorMessage(
                Format("Failed to compose component for '{}': {}", Desc.EntryPointName.value(), DiagView(DiagBlob))));
        }
    }

    // ── Link ──────────────────────────────────────────────────────────
    // Ensure that there are no missing dependencies in the composed program
    if (auto Rc = Composite->link(Linked.writeRef(), DiagBlob.writeRef()); SLANG_FAILED(Rc) || !Linked) {
        return std::unexpected(ErrorMessage(
            Format("Failed to link entry point '{}': {}", Desc.EntryPointName.value(), DiagView(DiagBlob))));
    }

    // ── Get SPIR-V bytecode ──────────────────────────────────────────
    if (auto Rc = Linked->getEntryPointCode(0, 0, CodeBlob.writeRef(), DiagBlob.writeRef());
        SLANG_FAILED(Rc) || !CodeBlob) {
        return std::unexpected(ErrorMessage(
            Format("Failed to generate SPIR-V for '{}': {}", Desc.EntryPointName.value(), DiagView(DiagBlob))));
    }

    return {};
}

} // anonymous namespace

/// @brief Concrete shader compiler backed by the Slang C++ API.
///
/// Two compilation paths:
///   Desc.EntryPointName == std::nullopt:
///     loadModule -> link -> getTargetCode  (all entry points)
///   Desc.EntryPointName has value:
///     loadModule -> findEntryPointByName -> createCompositeComponentType
///     -> link -> getEntryPointCode         (single entry point)
///
/// In both paths, entry point names and stages are read from Slang's
/// reflection data and returned as Shader::SShaderProgram values.
///
/// Init is lazy: the constructor is cheap; Compile() triggers the one-time
/// creation of the global session on first use.
class Backend final : public IBackend {
  public:
    Backend() = default;

    Backend(const Backend&)                    = delete;
    auto operator=(const Backend&) -> Backend& = delete;
    Backend(Backend&&)                         = delete;
    auto operator=(Backend&&) -> Backend&      = delete;

    ~Backend() override = default;

    [[nodiscard]] auto Compile(const CompileDesc& Desc)
        -> std::expected<std::vector<Shader::Program>, ErrorMessage> override {
        // Lazy init on first use.
        if (!m_bInitialized)
            if (auto R = Init(); !R)
                return std::unexpected(std::move(R.error()));

        // ── 1. Read source ──────────────────────────────────────────────
        String SourceContent;
        if (auto* P = std::get_if<Path>(&Desc.Source)) {
            // File mode — read from disk.
            auto FileContent = ReadFile(*P);
            if (!FileContent)
                return std::unexpected(std::move(FileContent.error()));
            SourceContent = std::move(*FileContent);
        } else {
            // Inline mode — use the string directly.
            SourceContent = String(std::get<StringView>(Desc.Source));
        }

        // ── 2. Create session (SPIR-V target) ──────────────────────────
        //
        // Session concept: https://docs.shader-slang.org/en/latest/compilation-api.html#about-sessions
        // My understanding: a session holds caches and states for modules
        slang::TargetDesc TargetDesc{.format  = SLANG_SPIRV,
                                     // target SPIR-V 1.6 (Vulkan 1.3 feature set)
                                     .profile = m_GlobalSession->findProfile("spirv_1_6")};

        // Compiler options (EmitSpirvDirectly, DebugInformation, etc.) were
        // pre-resolved during Init() by ResolveCompilerOptions().
        slang::SessionDesc SessionDesc{
            .targets                  = &TargetDesc,
            .targetCount              = 1,
            .defaultMatrixLayoutMode  = SLANG_MATRIX_LAYOUT_ROW_MAJOR,
            .compilerOptionEntries    = m_CompilerOptions.data(),
            .compilerOptionEntryCount = static_cast<uint32_t>(m_CompilerOptions.size()),
        };

        // Convert IncludeDirs to C-strings for Slang search paths.
        // Strings must be stored separately — Dir.string() returns a
        // temporary that would dangle if we only kept the c_str().
        std::vector<String>      SearchPathStrings;
        std::vector<const char*> SearchPathCStrs;
        SearchPathStrings.reserve(Desc.IncludeDirs.size());
        SearchPathCStrs.reserve(Desc.IncludeDirs.size());
        for (const auto& Dir : Desc.IncludeDirs)
            SearchPathStrings.push_back(Dir.string());
        for (const auto& S : SearchPathStrings)
            SearchPathCStrs.push_back(S.c_str());
        if (!SearchPathCStrs.empty()) {
            SessionDesc.searchPaths     = SearchPathCStrs.data();
            SessionDesc.searchPathCount = static_cast<SlangInt32>(SearchPathCStrs.size());
        }

        Slang::ComPtr<slang::ISession> Session;
        if (auto Rc = m_GlobalSession->createSession(SessionDesc, Session.writeRef()); SLANG_FAILED(Rc)) {
            return std::unexpected(ErrorMessage(Format("Failed to create Slang session (error code: {})", Rc)));
        }

        // ── 3. Load module from source string ──────────────────────────
        //
        // ISession has two loading paths:
        //
        //   loadModule(moduleName)
        //     — Scours the session's search paths for a file named <moduleName>.slang.
        //     — If the module was already loaded (by any path), returns the cached
        //       result from the session.
        //
        //   loadModuleFromSourceString(moduleName, path, source, ...)
        //     — Loads shader source directly from memory.
        //     — moduleName gives this blob of source an identifier.  Other modules
        //       can refer to it by this name via import "moduleName".
        //     — path is a backup key for the session's module cache.  It matters
        //       when a shader uses path-based imports like import "../foo.slang";
        //       nullptr caches by moduleName only.
        //
        // ── Future optimisation ──────────────────────────────────────────
        //
        // Currently a fresh ISession is created per Compile() call (see the
        // TODO at the top of this file).  The session's module cache is therefore
        // empty every time, so loadModuleFromSourceString is the only viable path.
        //
        // Once the ISession is reused across compiles (move createSession into
        // Init()), the flow changes:
        //   1. Compile() calls loadModule(ModuleName) first.
        //   2. If found -> cached result returned instantly (no recompile).
        //   3. If not found -> fall back to loadModuleFromSourceString, which
        //      populates the cache for subsequent calls.
        //
        // This avoids re-parsing and re-compiling unchanged modules and is the
        // primary benefit of session reuse.
        Slang::ComPtr<slang::IBlob> DiagBlob;

        String ModuleName = std::get_if<Path>(&Desc.Source) ? std::get<Path>(Desc.Source).stem().string() : "unnamed";
        auto*  Module     = Session->loadModuleFromSourceString(
            ModuleName.c_str(),
            std::get_if<Path>(&Desc.Source) ? std::get<Path>(Desc.Source).string().c_str() : nullptr,
            SourceContent.c_str(),
            DiagBlob.writeRef());

        if (!Module) {
            if (const auto* P = std::get_if<Path>(&Desc.Source)) {
                return std::unexpected(
                    ErrorMessage(Format("Failed to load module '{}': {}", P->string(), DiagView(DiagBlob))));
            }
            return std::unexpected(ErrorMessage(Format("Failed to load module: {}", DiagView(DiagBlob))));
        }

        // ── 4. Link -> get SPIR-V bytecode ───────────────────────────────
        Slang::ComPtr<slang::IComponentType> Linked;
        Slang::ComPtr<slang::IBlob>          CodeBlob;

        if (!Desc.EntryPointName) {
            if (auto R = CompileModule(Module, Desc, Linked, CodeBlob, DiagBlob); !R)
                return std::unexpected(std::move(R.error()));
        } else {
            if (auto R = CompileEntryPoint(Session, Module, Desc, Linked, CodeBlob, DiagBlob); !R)
                return std::unexpected(std::move(R.error()));
        }

        // ── 5. Build ShaderPrograms from compiled code + reflection ──────
        //
        // Copy SPIR-V bytecode into a shared immutable buffer so that
        // multiple ShaderPrograms from the same module can share one
        // allocation.
        auto* Begin = static_cast<const Uint32*>(CodeBlob->getBufferPointer());
        auto  Count = CodeBlob->getBufferSize() / sizeof(Uint32);
        auto  Code  = std::make_shared<const std::vector<Uint32>>(Begin, Begin + Count);

        std::vector<Shader::Program> Programs;

        auto Emit = [&](slang::ShaderReflection*     ProgramLayout,
                        slang::EntryPointReflection* EntryPoint) -> std::expected<void, ErrorMessage> {
            if (!EntryPoint)
                return std::unexpected(ErrorMessage("Compiled shader entry point reflection is missing"));

            auto Reflection = BuildShaderReflection(ProgramLayout, EntryPoint);
            if (!Reflection) {
                return std::unexpected(
                    Reflection.error().Append(Format("Failed to build normalized reflection for entry point '{}'",
                                                     EntryPoint->getName() ? EntryPoint->getName() : "<unnamed>")));
            }

            Programs.push_back(Shader::Program{
                .Code           = Code,
                .EntryPointName = String(EntryPoint->getName()),
                .Stage          = ToShaderStage(EntryPoint->getStage()),
                .Reflection     = std::move(*Reflection),
            });
            return {};
        };

        if (!Desc.EntryPointName) {
            // Module-level: all entry points compiled via getTargetCode.
            // Reflection comes from IModule::getDefinedEntryPoint().
            SlangInt32 Count = Module->getDefinedEntryPointCount();
            if (Count == 0)
                return std::unexpected(ErrorMessage(Format("Compiled shader contains no reflected entry points")));

            Programs.reserve(static_cast<size_t>(Count));
            for (SlangInt32 i = 0; i < Count; i++) {
                Slang::ComPtr<slang::IEntryPoint> EP;
                Module->getDefinedEntryPoint(i, EP.writeRef());

                auto* Layout = EP->getLayout(0);
                if (!Layout || Layout->getEntryPointCount() == 0)
                    continue;

                auto* Info = Layout->getEntryPointByIndex(0);
                if (auto R = Emit(Layout, Info); !R)
                    return std::unexpected(std::move(R.error()));
            }
        } else {
            // Single entry point: only the composed entry point is in the
            // SPIR-V blob.  Reflection comes from Linked->getLayout(0).
            auto* Layout = Linked->getLayout(0);
            if (!Layout || Layout->getEntryPointCount() == 0)
                return std::unexpected(ErrorMessage("Compiled shader contains no reflected entry points"));

            for (SlangInt32 i = 0; i < Layout->getEntryPointCount(); i++) {
                auto* Info = Layout->getEntryPointByIndex(i);
                if (auto R = Emit(Layout, Info); !R)
                    return std::unexpected(std::move(R.error()));
            }
        }

        if (Programs.empty())
            return std::unexpected(ErrorMessage("Compiled shader contains no reflected entry points"));

        return Programs;
    }

  private:
    /// One-time initialization of the Slang global session.
    /// Called from Compile() on the first request.
    [[nodiscard]] auto Init() -> std::expected<void, ErrorMessage> {
        if (auto Rc = slang::createGlobalSession(m_GlobalSession.writeRef()); SLANG_FAILED(Rc)) {
            return std::unexpected(ErrorMessage(Format("Failed to create Slang global session (error code: {})", Rc)));
        }
        m_CompilerOptions = ResolveCompilerOptions();
        m_bInitialized    = true;
        return {};
    }

    bool                                    m_bInitialized = false;
    Slang::ComPtr<slang::IGlobalSession>    m_GlobalSession;
    /// Pre-built compiler options, populated once by ResolveCompilerOptions()
    /// during Init() and reused across all Compile() calls.
    std::vector<slang::CompilerOptionEntry> m_CompilerOptions;
};

/// Auto-register the Slang backend with the compiler factory.
BackendFactory::AutoRegistrar<Backend> RegSlang{magic_enum::enum_name(SoulEngine::ShaderCompiler::Backend::Slang)};

} // namespace SoulEngine::ShaderCompiler::SlangCompiler
