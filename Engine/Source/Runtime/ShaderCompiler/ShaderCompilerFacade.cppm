/// @file   ShaderCompilerFacade.cppm
/// @brief  Public ShaderCompiler facade declaration.

module;

export module ShaderCompiler.Facade;

import magic_enum;
import std;
import Core;
import Shader;
import ShaderCompiler.Types;

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
