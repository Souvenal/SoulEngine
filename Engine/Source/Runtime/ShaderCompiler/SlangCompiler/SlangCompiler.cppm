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
/// every pipeline compile.  Reusing an ISession across compiles would cache
/// parsed modules and compiled SPIR-V, at the cost of holding that memory
/// until the backend is destroyed.  The current usage pattern (compile all
/// shaders at startup, then done) doesn't need it, but it's a win for
/// runtime PSO generation or hot-reload.  To implement: move the session
/// creation into Init() (currently in CreateSession()), store the ISession as
/// a member, and skip per-compile session creation.

module;

#include <magic_enum/magic_enum.hpp>
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

[[nodiscard]] auto CreateSession(slang::IGlobalSession*                       GlobalSession,
                                 std::span<const slang::CompilerOptionEntry> CompilerOptions,
                                 std::span<const Path>                       IncludeDirs,
                                 bool                                         bEnableRayTracing)
    -> std::expected<Slang::ComPtr<slang::ISession>, ErrorMessage> {
    // Session concept: https://docs.shader-slang.org/en/latest/compilation-api.html#about-sessions
    // My understanding: a session holds caches and states for modules.
    slang::TargetDesc TargetDesc{.format  = SLANG_SPIRV,
                                 // target SPIR-V 1.6 (Vulkan 1.3 feature set)
                                 .profile = GlobalSession->findProfile("spirv_1_6")};

    std::array<slang::CompilerOptionEntry, 1> TargetOptions = {};
    if (bEnableRayTracing) {
        const auto Capability = GlobalSession->findCapability("spvRayTracingKHR");
        if (Capability == SLANG_CAPABILITY_UNKNOWN)
            return std::unexpected(ErrorMessage("Slang does not expose the spvRayTracingKHR capability"));

        TargetOptions[0] = slang::CompilerOptionEntry{
            .name  = slang::CompilerOptionName::Capability,
            .value = {.intValue0 = Capability},
        };
        TargetDesc.compilerOptionEntries    = TargetOptions.data();
        TargetDesc.compilerOptionEntryCount = static_cast<Uint32>(TargetOptions.size());
    }

    // Compiler options (EmitSpirvDirectly, DebugInformation, etc.) are
    // pre-resolved during Init() by ResolveCompilerOptions().
    slang::SessionDesc SessionDesc{
        .targets                  = &TargetDesc,
        .targetCount              = 1,
        .defaultMatrixLayoutMode  = SLANG_MATRIX_LAYOUT_ROW_MAJOR,
        .compilerOptionEntries    = CompilerOptions.data(),
        .compilerOptionEntryCount = static_cast<uint32_t>(CompilerOptions.size()),
    };

    // Convert IncludeDirs to C-strings for Slang search paths.
    // Strings must be stored separately — Dir.string() returns a
    // temporary that would dangle if we only kept the c_str().
    std::vector<String>      SearchPathStrings;
    std::vector<const char*> SearchPathCStrs;
    SearchPathStrings.reserve(IncludeDirs.size());
    SearchPathCStrs.reserve(IncludeDirs.size());
    for (const auto& Dir : IncludeDirs)
        SearchPathStrings.push_back(Dir.string());
    for (const auto& S : SearchPathStrings)
        SearchPathCStrs.push_back(S.c_str());
    if (!SearchPathCStrs.empty()) {
        SessionDesc.searchPaths     = SearchPathCStrs.data();
        SessionDesc.searchPathCount = static_cast<SlangInt32>(SearchPathCStrs.size());
    }

    Slang::ComPtr<slang::ISession> Session;
    if (auto Rc = GlobalSession->createSession(SessionDesc, Session.writeRef()); SLANG_FAILED(Rc)) {
        return std::unexpected(ErrorMessage(Format("Failed to create Slang session (error code: {})", Rc)));
    }
    return Session;
}

[[nodiscard]] auto LoadModuleFromSource(slang::ISession*            Session,
                                        const ShaderEntry&          Entry,
                                        std::vector<String>&        SourceStorage,
                                        Slang::ComPtr<slang::IBlob>& DiagBlob)
    -> std::expected<slang::IModule*, ErrorMessage> {
    auto SourceContent = ReadFile(Entry.SourcePath);
    if (!SourceContent)
        return std::unexpected(std::move(SourceContent.error()));

    SourceStorage.push_back(std::move(*SourceContent));
    const auto ModuleName = Entry.SourcePath.stem().string();

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
    // Currently a fresh ISession is created per pipeline compile (see the
    // TODO at the top of this file).  The session's module cache is therefore
    // empty every time, so loadModuleFromSourceString is the only viable path.
    //
    // Once the ISession is reused across compiles (move createSession into
    // Init()), the flow changes:
    //   1. Compile path calls loadModule(ModuleName) first.
    //   2. If found -> cached result returned instantly (no recompile).
    //   3. If not found -> fall back to loadModuleFromSourceString, which
    //      populates the cache for subsequent calls.
    //
    // This avoids re-parsing and re-compiling unchanged modules and is the
    // primary benefit of session reuse.
    auto* Module = Session->loadModuleFromSourceString(ModuleName.c_str(),
                                                       Entry.SourcePath.string().c_str(),
                                                       SourceStorage.back().c_str(),
                                                       DiagBlob.writeRef());
    if (!Module) {
        return std::unexpected(
            ErrorMessage(Format("Failed to load module '{}': {}", Entry.SourcePath.string(), DiagView(DiagBlob))));
    }
    return Module;
}

[[nodiscard]] auto FindEntryPoint(slang::IModule* Module, const ShaderEntry& Entry, StringView StageName)
    -> std::expected<Slang::ComPtr<slang::IEntryPoint>, ErrorMessage> {
    Slang::ComPtr<slang::IEntryPoint> EntryPoint;
    if (auto Rc = Module->findEntryPointByName(Entry.EntryPoint.c_str(), EntryPoint.writeRef());
        SLANG_FAILED(Rc) || !EntryPoint) {
        return std::unexpected(ErrorMessage(
            Format("{} entry point '{}' not found in '{}'", StageName, Entry.EntryPoint, Entry.SourcePath.string())));
    }
    return EntryPoint;
}

// Slang's "component" is a compiler/linker unit. Modules and entry points are 
// both `IComponentType` values.
// A graphics shader program is represented by composing the module(s) plus
// all pipeline entry points into one component, then linking it so
// code generation and reflection see the final pipeline interface instead of isolated stages.
[[nodiscard]] auto ComposeAndLink(slang::ISession*                   Session,
                                  std::span<slang::IComponentType*> Components,
                                  StringView                         Description,
                                  Slang::ComPtr<slang::IBlob>&       DiagBlob)
    -> std::expected<Slang::ComPtr<slang::IComponentType>, ErrorMessage> {
    // Compose into a unified GPU program.
    Slang::ComPtr<slang::IComponentType> Composite;
    if (auto Rc = Session->createCompositeComponentType(Components.data(),
                                                        static_cast<SlangInt>(Components.size()),
                                                        Composite.writeRef(),
                                                        DiagBlob.writeRef());
        SLANG_FAILED(Rc) || !Composite) {
        return std::unexpected(
            ErrorMessage(Format("Failed to compose {}: {}", Description, DiagView(DiagBlob))));
    }

    // Ensure that there are no missing dependencies in the composed program.
    Slang::ComPtr<slang::IComponentType> Linked;
    if (auto Rc = Composite->link(Linked.writeRef(), DiagBlob.writeRef()); SLANG_FAILED(Rc) || !Linked) {
        return std::unexpected(ErrorMessage(Format("Failed to link {}: {}", Description, DiagView(DiagBlob))));
    }
    return Linked;
}

[[nodiscard]] auto GetTargetCode(slang::IComponentType* Linked,
                                 StringView             Description,
                                 Slang::ComPtr<slang::IBlob>& DiagBlob)
    -> std::expected<std::vector<Uint32>, ErrorMessage> {
    Slang::ComPtr<slang::IBlob> CodeBlob;
    if (auto Rc = Linked->getTargetCode(0, CodeBlob.writeRef(), DiagBlob.writeRef());
        SLANG_FAILED(Rc) || !CodeBlob) {
        return std::unexpected(ErrorMessage(Format("Failed to generate {}: {}", Description, DiagView(DiagBlob))));
    }

    auto* Begin = static_cast<const Uint32*>(CodeBlob->getBufferPointer());
    auto  Count = CodeBlob->getBufferSize() / sizeof(Uint32);
    return std::vector<Uint32>(Begin, Begin + Count);
}

} // anonymous namespace

/// @brief Concrete shader compiler backed by the Slang C++ API.
///
/// Pipeline compiles load the requested modules, find all pipeline entry points,
/// compose them into one linked Slang program, then emit SPIR-V plus linked
/// pipeline-level reflection.
///
/// Init is lazy: the constructor is cheap; the first pipeline compile triggers
/// the one-time creation of the global session.
class Backend final : public IBackend {
  public:
    Backend() = default;

    Backend(const Backend&)                    = delete;
    auto operator=(const Backend&) -> Backend& = delete;
    Backend(Backend&&)                         = delete;
    auto operator=(Backend&&) -> Backend&      = delete;

    ~Backend() override = default;

    [[nodiscard]] auto CompileGraphics(const GraphicsCompileDesc& Desc)
        -> std::expected<Shader::GraphicsProgram, ErrorMessage> override {
        if (!m_bInitialized)
            if (auto R = Init(); !R)
                return std::unexpected(std::move(R.error()));

        if (Desc.Vertex.EntryPoint.empty() || Desc.Fragment.EntryPoint.empty()) {
            return std::unexpected(ErrorMessage("Graphics shader compile requires non-empty vertex and fragment entry points"));
        }

        auto Session = CreateSession(m_GlobalSession.get(), m_CompilerOptions, Desc.IncludeDirs, false);
        if (!Session)
            return std::unexpected(std::move(Session.error()));

        Slang::ComPtr<slang::IBlob> DiagBlob;
        std::vector<String>         SourceStorage;
        SourceStorage.reserve(2);

        auto VertexModule = LoadModuleFromSource(Session->get(), Desc.Vertex, SourceStorage, DiagBlob);
        if (!VertexModule)
            return std::unexpected(VertexModule.error().Append(
                Format("Graphics pipeline vertex shader '{}'/'{}'", Desc.Vertex.SourcePath.string(), Desc.Vertex.EntryPoint)));

        slang::IModule* FragmentModule = *VertexModule;
        if (Desc.Fragment.SourcePath.lexically_normal() != Desc.Vertex.SourcePath.lexically_normal()) {
            auto LoadedFragmentModule = LoadModuleFromSource(Session->get(), Desc.Fragment, SourceStorage, DiagBlob);
            if (!LoadedFragmentModule)
                return std::unexpected(LoadedFragmentModule.error().Append(Format("Graphics pipeline fragment shader '{}'/'{}'",
                                                                                  Desc.Fragment.SourcePath.string(),
                                                                                  Desc.Fragment.EntryPoint)));
            FragmentModule = *LoadedFragmentModule;
        }

        auto VertexEntryPoint = FindEntryPoint(*VertexModule, Desc.Vertex, "Vertex");
        if (!VertexEntryPoint)
            return std::unexpected(std::move(VertexEntryPoint.error()));

        auto FragmentEntryPoint = FindEntryPoint(FragmentModule, Desc.Fragment, "Fragment");
        if (!FragmentEntryPoint)
            return std::unexpected(std::move(FragmentEntryPoint.error()));

        std::vector<slang::IComponentType*> Components;
        Components.reserve(4);
        Components.push_back(*VertexModule);
        if (FragmentModule != *VertexModule)
            Components.push_back(FragmentModule);
        Components.push_back(VertexEntryPoint->get());
        Components.push_back(FragmentEntryPoint->get());

        auto Linked = ComposeAndLink(Session->get(), Components, "graphics shader program", DiagBlob);
        if (!Linked)
            return std::unexpected(std::move(Linked.error()));

        auto Code = GetTargetCode(Linked->get(), "graphics SPIR-V", DiagBlob);
        if (!Code)
            return std::unexpected(std::move(Code.error()));

        auto* Layout = (*Linked)->getLayout(0);
        if (!Layout)
            return std::unexpected(ErrorMessage("Compiled graphics shader is missing linked reflection layout"));

        auto* VertexInfo = Layout->findEntryPointByName(Desc.Vertex.EntryPoint.c_str());
        if (!VertexInfo)
            return std::unexpected(ErrorMessage(
                Format("Linked graphics reflection is missing vertex entry point '{}'", Desc.Vertex.EntryPoint)));

        auto* FragmentInfo = Layout->findEntryPointByName(Desc.Fragment.EntryPoint.c_str());
        if (!FragmentInfo)
            return std::unexpected(ErrorMessage(
                Format("Linked graphics reflection is missing fragment entry point '{}'", Desc.Fragment.EntryPoint)));

        auto PipelineReflection = BuildShaderReflection(Layout, VertexInfo, FragmentInfo);
        if (!PipelineReflection) {
            return std::unexpected(PipelineReflection.error().Append("Failed to build graphics pipeline reflection"));
        }

        String VertexEntryPointName = String(VertexInfo->getName());
        if (ToShaderStage(VertexInfo->getStage()) != Shader::Stage::Vertex) {
            return std::unexpected(ErrorMessage(Format("Entry point '{}' is not a vertex shader", VertexEntryPointName)));
        }

        String FragmentEntryPointName = String(FragmentInfo->getName());
        if (ToShaderStage(FragmentInfo->getStage()) != Shader::Stage::Fragment) {
            return std::unexpected(
                ErrorMessage(Format("Entry point '{}' is not a fragment shader", FragmentEntryPointName)));
        }

        return Shader::GraphicsProgram{
            .Code                   = std::move(*Code),
            .VertexEntryPointName   = std::move(VertexEntryPointName),
            .FragmentEntryPointName = std::move(FragmentEntryPointName),
            .Reflection             = std::move(*PipelineReflection),
        };
    }

    [[nodiscard]] auto CompileRayTracing(const RayTracingCompileDesc& Desc)
        -> std::expected<Shader::RayTracingProgram, ErrorMessage> override {
        if (!m_bInitialized)
            if (auto R = Init(); !R)
                return std::unexpected(std::move(R.error()));

        if (Desc.RayGeneration.EntryPoint.empty())
            return std::unexpected(ErrorMessage("Ray-tracing shader compile requires a ray-generation entry point"));
        if (Desc.MissEntries.empty())
            return std::unexpected(ErrorMessage("Ray-tracing shader compile requires at least one miss entry point"));
        if (Desc.HitGroups.empty())
            return std::unexpected(ErrorMessage("Ray-tracing shader compile requires at least one hit group"));

        for (Uint32 GroupIndex = 0; GroupIndex < Desc.HitGroups.size(); ++GroupIndex) {
            const auto& Group = Desc.HitGroups[GroupIndex];
            if (!Group.ClosestHit.has_value() || Group.ClosestHit->EntryPoint.empty()) {
                return std::unexpected(
                    ErrorMessage(Format("Ray-tracing hit group {} requires a closest-hit entry point", GroupIndex)));
            }
            if (Group.Type == Shader::RayTracingHitGroupType::Triangles && Group.Intersection.has_value()) {
                return std::unexpected(ErrorMessage(
                    Format("Triangle ray-tracing hit group {} must not declare an intersection entry point", GroupIndex)));
            }
            if (Group.Type == Shader::RayTracingHitGroupType::Procedural && !Group.Intersection.has_value()) {
                return std::unexpected(ErrorMessage(
                    Format("Procedural ray-tracing hit group {} requires an intersection entry point", GroupIndex)));
            }
        }

        auto Session = CreateSession(m_GlobalSession.get(), m_CompilerOptions, Desc.IncludeDirs, true);
        if (!Session)
            return std::unexpected(std::move(Session.error()));

        struct LoadedModule {
            Path            SourcePath = {};
            slang::IModule* Module     = nullptr;
        };
        struct LoadedEntry {
            const ShaderEntry*                  Request      = nullptr;
            Shader::Stage                       ExpectedStage = Shader::Stage::Unknown;
            StringView                          StageName    = {};
            Slang::ComPtr<slang::IEntryPoint>   EntryPoint   = {};
        };
        struct LoadedHitGroup {
            Shader::RayTracingHitGroupType Type         = Shader::RayTracingHitGroupType::Unknown;
            std::optional<Uint32>          ClosestHit   = std::nullopt;
            std::optional<Uint32>          AnyHit       = std::nullopt;
            std::optional<Uint32>          Intersection = std::nullopt;
        };

        Slang::ComPtr<slang::IBlob> DiagBlob;
        std::vector<String>         SourceStorage = {};
        std::vector<LoadedModule>   Modules       = {};
        std::vector<LoadedEntry>    Entries       = {};

        auto LoadEntry = [&](const ShaderEntry& Entry, Shader::Stage ExpectedStage, StringView StageName)
            -> std::expected<Uint32, ErrorMessage> {
            const Path NormalizedPath = Entry.SourcePath.lexically_normal();
            slang::IModule* Module = nullptr;
            for (const auto& Loaded : Modules) {
                if (Loaded.SourcePath == NormalizedPath) {
                    Module = Loaded.Module;
                    break;
                }
            }
            if (!Module) {
                auto LoadedModuleResult = LoadModuleFromSource(Session->get(), Entry, SourceStorage, DiagBlob);
                if (!LoadedModuleResult)
                    return std::unexpected(LoadedModuleResult.error().Append(
                        Format("Ray-tracing {} shader '{}'/'{}'", StageName, Entry.SourcePath.string(), Entry.EntryPoint)));
                Module = *LoadedModuleResult;
                Modules.emplace_back(LoadedModule{.SourcePath = NormalizedPath, .Module = Module});
            }

            auto EntryPoint = FindEntryPoint(Module, Entry, StageName);
            if (!EntryPoint)
                return std::unexpected(std::move(EntryPoint.error()));

            Entries.emplace_back(LoadedEntry{
                .Request       = &Entry,
                .ExpectedStage = ExpectedStage,
                .StageName     = StageName,
                .EntryPoint    = std::move(*EntryPoint),
            });
            return static_cast<Uint32>(Entries.size() - 1);
        };

        auto RayGeneration = LoadEntry(Desc.RayGeneration, Shader::Stage::RayGeneration, "Ray-generation");
        if (!RayGeneration)
            return std::unexpected(std::move(RayGeneration.error()));

        std::vector<Uint32> MissEntries = {};
        MissEntries.reserve(Desc.MissEntries.size());
        for (const auto& Entry : Desc.MissEntries) {
            auto Miss = LoadEntry(Entry, Shader::Stage::Miss, "Miss");
            if (!Miss)
                return std::unexpected(std::move(Miss.error()));
            MissEntries.push_back(*Miss);
        }

        std::vector<LoadedHitGroup> HitGroups = {};
        HitGroups.reserve(Desc.HitGroups.size());
        for (const auto& Group : Desc.HitGroups) {
            LoadedHitGroup LoadedGroup{.Type = Group.Type};
            auto ClosestHit = LoadEntry(*Group.ClosestHit, Shader::Stage::ClosestHit, "Closest-hit");
            if (!ClosestHit)
                return std::unexpected(std::move(ClosestHit.error()));
            LoadedGroup.ClosestHit = *ClosestHit;

            if (Group.AnyHit.has_value()) {
                auto AnyHit = LoadEntry(*Group.AnyHit, Shader::Stage::AnyHit, "Any-hit");
                if (!AnyHit)
                    return std::unexpected(std::move(AnyHit.error()));
                LoadedGroup.AnyHit = *AnyHit;
            }
            if (Group.Intersection.has_value()) {
                auto Intersection = LoadEntry(*Group.Intersection, Shader::Stage::Intersection, "Intersection");
                if (!Intersection)
                    return std::unexpected(std::move(Intersection.error()));
                LoadedGroup.Intersection = *Intersection;
            }
            HitGroups.emplace_back(std::move(LoadedGroup));
        }

        std::vector<Uint32> CallableEntries = {};
        CallableEntries.reserve(Desc.CallableEntries.size());
        for (const auto& Entry : Desc.CallableEntries) {
            auto Callable = LoadEntry(Entry, Shader::Stage::Callable, "Callable");
            if (!Callable)
                return std::unexpected(std::move(Callable.error()));
            CallableEntries.push_back(*Callable);
        }

        std::vector<slang::IComponentType*> Components = {};
        Components.reserve(Modules.size() + Entries.size());
        for (const auto& Module : Modules)
            Components.push_back(Module.Module);
        for (const auto& Entry : Entries)
            Components.push_back(Entry.EntryPoint.get());

        auto Linked = ComposeAndLink(Session->get(), Components, "ray-tracing shader program", DiagBlob);
        if (!Linked)
            return std::unexpected(std::move(Linked.error()));

        auto Code = GetTargetCode(Linked->get(), "ray-tracing SPIR-V", DiagBlob);
        if (!Code)
            return std::unexpected(std::move(Code.error()));

        auto* Layout = (*Linked)->getLayout(0);
        if (!Layout)
            return std::unexpected(ErrorMessage("Compiled ray-tracing shader is missing linked reflection layout"));

        std::vector<slang::EntryPointReflection*> EntryPointInfos(Entries.size(), nullptr);
        auto ResolveEntry = [&](Uint32 EntryIndex) -> std::expected<String, ErrorMessage> {
            const auto& Entry = Entries[EntryIndex];
            auto* EntryInfo = Layout->findEntryPointByName(Entry.Request->EntryPoint.c_str());
            if (!EntryInfo) {
                return std::unexpected(ErrorMessage(Format("Linked ray-tracing reflection is missing {} entry point '{}'",
                                                           Entry.StageName,
                                                           Entry.Request->EntryPoint)));
            }
            if (ToShaderStage(EntryInfo->getStage()) != Entry.ExpectedStage) {
                return std::unexpected(ErrorMessage(Format("Entry point '{}' is not a {} shader",
                                                           EntryInfo->getName(),
                                                           Entry.StageName)));
            }
            EntryPointInfos[EntryIndex] = EntryInfo;
            return String(EntryInfo->getName());
        };

        auto RayGenerationName = ResolveEntry(*RayGeneration);
        if (!RayGenerationName)
            return std::unexpected(std::move(RayGenerationName.error()));

        std::vector<String> MissNames = {};
        MissNames.reserve(MissEntries.size());
        for (const auto EntryIndex : MissEntries) {
            auto Name = ResolveEntry(EntryIndex);
            if (!Name)
                return std::unexpected(std::move(Name.error()));
            MissNames.emplace_back(std::move(*Name));
        }

        std::vector<Shader::RayTracingHitGroup> ProgramHitGroups = {};
        ProgramHitGroups.reserve(HitGroups.size());
        for (const auto& Group : HitGroups) {
            Shader::RayTracingHitGroup ProgramGroup{.Type = Group.Type};
            auto ClosestName = ResolveEntry(*Group.ClosestHit);
            if (!ClosestName)
                return std::unexpected(std::move(ClosestName.error()));
            ProgramGroup.ClosestHitEntryPointName = std::move(*ClosestName);
            if (Group.AnyHit.has_value()) {
                auto AnyName = ResolveEntry(*Group.AnyHit);
                if (!AnyName)
                    return std::unexpected(std::move(AnyName.error()));
                ProgramGroup.AnyHitEntryPointName = std::move(*AnyName);
            }
            if (Group.Intersection.has_value()) {
                auto IntersectionName = ResolveEntry(*Group.Intersection);
                if (!IntersectionName)
                    return std::unexpected(std::move(IntersectionName.error()));
                ProgramGroup.IntersectionEntryPointName = std::move(*IntersectionName);
            }
            ProgramHitGroups.emplace_back(std::move(ProgramGroup));
        }

        std::vector<String> CallableNames = {};
        CallableNames.reserve(CallableEntries.size());
        for (const auto EntryIndex : CallableEntries) {
            auto Name = ResolveEntry(EntryIndex);
            if (!Name)
                return std::unexpected(std::move(Name.error()));
            CallableNames.emplace_back(std::move(*Name));
        }

        auto PipelineReflection = BuildRayTracingShaderReflection(Layout, EntryPointInfos);
        if (!PipelineReflection)
            return std::unexpected(PipelineReflection.error().Append("Failed to build ray-tracing pipeline reflection"));

        return Shader::RayTracingProgram{
            .Code                        = std::move(*Code),
            .RayGenerationEntryPointName = std::move(*RayGenerationName),
            .MissEntryPointNames          = std::move(MissNames),
            .HitGroups                    = std::move(ProgramHitGroups),
            .CallableEntryPointNames      = std::move(CallableNames),
            .Reflection                   = std::move(*PipelineReflection),
        };
    }

  private:
    /// One-time initialization of the Slang global session.
    /// Called from the first pipeline compile request.
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
    /// during Init() and reused across all pipeline compile calls.
    std::vector<slang::CompilerOptionEntry> m_CompilerOptions;
};

/// Auto-register the Slang backend with the compiler factory.
BackendFactory::AutoRegistrar<Backend> RegSlang{magic_enum::enum_name(SoulEngine::ShaderCompiler::Backend::Slang)};

} // namespace SoulEngine::ShaderCompiler::SlangCompiler
