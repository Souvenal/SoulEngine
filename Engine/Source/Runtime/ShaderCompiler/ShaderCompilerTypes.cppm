/// @file   ShaderCompilerTypes.cppm
/// @brief  Compile-request descriptor and backend enum for ShaderCompiler.
///
/// This partition holds only the types needed to describe a compile request.
/// Results are returned as Shader::Program values from the Shader module.

module;

#include <magic_enum/magic_enum.hpp>

export module ShaderCompiler:Types;

import Shader;
import Core;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::ShaderCompiler {

/// Supported shader compiler backends.
///
/// The Backend field in CompileDesc is the canonical source of truth for
/// selecting which compiler pipeline to use.  When Source is a file Path
/// the extension is validated against this value (warning on mismatch),
/// but the enum always wins.
enum class Backend : Uint8 {
    Unknown = 0,

    /// Slang shading language — the project's primary shader compiler.
    Slang
};

/// @brief Request for a single shader entry point in a source file.
struct ShaderEntry {
    Path    SourcePath = {};
    String  EntryPoint = {};
    Backend Backend    = Backend::Slang;
};

/// @brief Descriptor for a single shader compile request.
///
/// Source is specified via std::variant — either a filesystem Path
/// (file mode, compiler reads from disk) or a StringView (inline mode,
/// source text passed directly).  Mutually exclusive at the type level.
///
/// Two compilation modes controlled by EntryPointName:
///   has value -> compile that specific entry point only
///   no value  -> compile all entry points in the module into one SPIR-V blob
struct CompileDesc {
    /// Source — either a filesystem path (read from disk) or inline text.
    std::variant<Path, StringView> Source;

    /// Target compiler backend.  Canonical selection; file extension is
    /// validated against this but the enum always determines routing.
    /// Defaults to the project's primary backend (Slang).
    Backend Backend = Backend::Slang;

    /// Optional name of the entry-point function to compile.
    /// std::nullopt -> compile the entire module (all entry points).
    std::optional<StringView> EntryPointName = std::nullopt;

    /// Optional preprocessor definitions in "KEY=VALUE" form.
    std::span<const StringView> Defines = {};

    /// Additional include search directories.
    std::span<const Path> IncludeDirs = {};
};

/// @brief Abstract backend for shading-language compilation.
///
/// Each supported shading language gets its own implementation.
/// Owned by the ShaderCompiler facade.
class IBackend {
  public:
    IBackend()                                   = default;
    IBackend(const IBackend&)                    = delete;
    auto operator=(const IBackend&) -> IBackend& = delete;
    IBackend(IBackend&&)                         = delete;
    auto operator=(IBackend&&) -> IBackend&      = delete;

    virtual ~IBackend() = default;

    /// @brief Compile shader source and return one Program per
    ///        reflected entry point.
    ///
    /// When Desc.EntryPointName is set, returns exactly one program.
    /// When empty, returns all reflected entry points sharing the same
    /// compiled bytecode.
    [[nodiscard]] virtual auto Compile(const CompileDesc& Desc)
        -> std::expected<std::vector<Shader::Program>, ErrorMessage> = 0;
};

/// @brief Factory type for compiler backends.
/// Each backend auto-registers via AutoRegistrar in its own translation unit.
using BackendFactory = Factory<IBackend>;

} // namespace SoulEngine::ShaderCompiler
