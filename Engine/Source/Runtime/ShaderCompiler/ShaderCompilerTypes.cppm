/// @file   ShaderCompilerTypes.cppm
/// @brief  Compile-request descriptor and backend enum for ShaderCompiler.
///
/// This partition holds only the types needed to describe a compile request.
/// Results are returned as pipeline program values from the Shader module.

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
/// The Backend field in shader compile descriptors is the canonical source of
/// truth for selecting which compiler pipeline to use. Source file extensions
/// are validated against this value (warning on mismatch), but the enum always
/// wins.
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

/// @brief Descriptor for a graphics-pipeline shader compile request.
struct GraphicsCompileDesc {
    ShaderEntry Vertex = {};
    ShaderEntry Fragment = {};

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

    /// @brief Compile and reflect a graphics pipeline shader combination.
    [[nodiscard]] virtual auto CompileGraphics(const GraphicsCompileDesc& Desc)
        -> std::expected<Shader::GraphicsProgram, ErrorMessage> = 0;
};

/// @brief Factory type for compiler backends.
/// Each backend auto-registers via AutoRegistrar in its own translation unit.
using BackendFactory = Factory<IBackend>;

} // namespace SoulEngine::ShaderCompiler
