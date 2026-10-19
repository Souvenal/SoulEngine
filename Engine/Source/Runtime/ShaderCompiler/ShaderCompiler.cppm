/// @file   ShaderCompiler.cppm
/// @brief  Main ShaderCompiler module — singleton facade over per-language
///         compiler backends.
///
/// Usage:
///   auto Result = ShaderCompiler::Get().CompileGraphics(Desc);
///
/// Thread safety: each compiler backend has its own mutex. Compiling concurrently; same-language compilations
/// are serialized.
///
/// ShaderBackend selection: explicit ShaderBackend enum in shader compile descriptors. File extension is
/// validated against the enum value (warning on mismatch) but never overrides it.
///
/// ShaderBackend registration: backend modules (e.g., Slang) self-register with ShaderBackendFactory via
/// AutoRegistrar constructors when the shared library is loaded. ShaderCompiler never references concrete
/// backend types directly. Adding a new backend requires zero changes to this file.

module;

export module ShaderCompiler;

export import :Types;

import magic_enum;
import std;
import Core;
import Shader;

namespace SoulEngine {

/// @brief Singleton facade over shader-language compiler backends.
///
/// Holds one backend slot per supported source language and initializes each lazily on first use. Each slot
/// carries its own mutex so that compilations in different languages can run concurrently.
///
/// Obtain via:
///   auto& Compiler = ShaderCompiler::Get();
///   auto Result    = Compiler.CompileGraphics(Desc);
export class ShaderCompiler : public Singleton<ShaderCompiler> {
    friend class Singleton<ShaderCompiler>;
    // Internal types

    struct BackendSlot {
        std::mutex           Mutex;
        UPtr<IShaderBackend> Instance = nullptr;
    };

  public:
    ShaderCompiler(const ShaderCompiler&)                    = delete;
    auto operator=(const ShaderCompiler&) -> ShaderCompiler& = delete;
    ShaderCompiler(ShaderCompiler&&)                         = delete;
    auto operator=(ShaderCompiler&&) -> ShaderCompiler&      = delete;

    /// @brief Compile a graphics-pipeline shader combination and pipeline-level reflection.
    [[nodiscard]] auto CompileGraphics(const GraphicsCompileDesc& Desc)
        -> std::expected<ShaderGraphicsProgram, ErrorMessage>;

    [[nodiscard]] auto CompileCompute(const ComputeCompileDesc& Desc)
        -> std::expected<ShaderComputeProgram, ErrorMessage>;

    [[nodiscard]] auto CompileRayTracing(const RayTracingCompileDesc& Desc)
        -> std::expected<ShaderRayTracingProgram, ErrorMessage>;

  private:
    ShaderCompiler()  = default;
    ~ShaderCompiler() = default;
    // ShaderBackend router

    auto ValidateEntryBackendConsistency(const ShaderEntry& Entry) -> void;

    /// @brief Create a new backend instance via the factory.
    ///
    /// The factory registry is populated by AutoRegistrar instances in each backend's translation unit.
    [[nodiscard]] auto CreateBackend(ShaderBackend Kind) -> std::expected<UPtr<IShaderBackend>, ErrorMessage>;

    std::array<BackendSlot, magic_enum::enum_count<ShaderBackend>()> m_Backends;
};

} // namespace SoulEngine
