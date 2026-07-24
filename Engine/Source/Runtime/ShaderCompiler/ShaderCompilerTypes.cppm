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

export namespace SoulEngine {

/// Supported shader compiler backends.
///
/// The ShaderBackend field in shader compile descriptors is the canonical source of
/// truth for selecting which compiler pipeline to use. Source file extensions
/// are validated against this value (warning on mismatch), but the enum always
/// wins.
enum class ShaderBackend : Uint8 {
    Unknown = 0,

    /// Slang shading language — the project's primary shader compiler.
    Slang
};

/// @brief Request for a single shader entry point in a source file.
struct ShaderEntry {
    Path    SourcePath = {};
    String  EntryPoint = {};
    ShaderBackend Backend    = ShaderBackend::Slang;
};

/// @brief Descriptor for a graphics-pipeline shader compile request.
struct GraphicsCompileDesc {
    ShaderEntry Vertex = {};
    ShaderEntry Fragment = {};

    /// Additional include search directories.
    std::span<const Path> IncludeDirs = {};
};

/// @brief One logical ray-tracing hit group compile request.
struct RayTracingHitGroupCompileDesc {
    ShaderRayTracingHitGroupType Type         = ShaderRayTracingHitGroupType::Triangles;
    std::optional<ShaderEntry>     ClosestHit   = std::nullopt;
    std::optional<ShaderEntry>     AnyHit       = std::nullopt;
    std::optional<ShaderEntry>     Intersection = std::nullopt;
};

/// @brief Compile request for one linked hardware ray-tracing shader program.
struct RayTracingCompileDesc {
    ShaderEntry                                RayGeneration = {};
    std::vector<ShaderEntry>                   MissEntries   = {};
    std::vector<RayTracingHitGroupCompileDesc> HitGroups     = {};
    std::vector<ShaderEntry>                   CallableEntries = {};

    /// Additional include search directories.
    std::span<const Path> IncludeDirs = {};
};

/// @brief Abstract backend for shading-language compilation.
///
/// Each supported shading language gets its own implementation.
/// Owned by the ShaderCompiler facade.
class IShaderBackend {
  public:
    IShaderBackend()                                   = default;
    IShaderBackend(const IShaderBackend&)                    = delete;
    auto operator=(const IShaderBackend&) -> IShaderBackend& = delete;
    IShaderBackend(IShaderBackend&&)                         = delete;
    auto operator=(IShaderBackend&&) -> IShaderBackend&      = delete;

    virtual ~IShaderBackend() = default;

    /// @brief Compile and reflect a graphics pipeline shader combination.
    [[nodiscard]] virtual auto CompileGraphics(const GraphicsCompileDesc& Desc)
        -> std::expected<ShaderGraphicsProgram, ErrorMessage> = 0;

    /// @brief Compile and reflect one linked ray-tracing shader program.
    [[nodiscard]] virtual auto CompileRayTracing(const RayTracingCompileDesc& Desc)
        -> std::expected<ShaderRayTracingProgram, ErrorMessage> = 0;
};

/// @brief Factory type for compiler backends.
/// Each backend auto-registers via AutoRegistrar in its own translation unit.
using ShaderBackendFactory = Factory<IShaderBackend>;

} // namespace SoulEngine
