/// @file   SlangCompiler/SlangCompiler.cppm
/// @brief  Slang-backed concrete shader compiler backend.
///
/// Standalone module — self-registers with ShaderBackendFactory via static AutoRegistrar when the shared
/// library is loaded. Slang SDK dependencies live in this file and the :Reflection partition.
///
/// Based on the official minimal example:
///   https://docs.shader-slang.org/en/latest/compilation-api.html
///
/// TODO(Session caching): currently a fresh slang::ISession is created for every pipeline compile. Reusing an
/// ISession across compiles would cache parsed modules and compiled SPIR-V, at the cost of holding that
/// memory until the backend is destroyed. The current usage pattern (compile all shaders at startup, then
/// done) doesn't need it, but it's a win for runtime PSO generation or hot-reload. To implement: move the
/// session creation into Init() (currently in CreateSession()), store the ISession as a member, and skip
/// per-compile session creation.

module;

// MSVC bug, fstream is needed here TODO: remove this guard
#include <fstream>

#include <slang.h>
// Slang follows the COM ABI convention (vtable layout, addRef/release lifecycle). Its COM-style interfaces
// require a dedicated smart pointer instead of unique_ptr/shared_ptr — hence slang-com-ptr.h provides
// Slang::ComPtr<T>.
#include <slang-com-ptr.h>

export module Slang;

import magic_enum;
import :Types;
import :Utils;
import :Reflection;

import std;
import Core;
import ShaderCompiler;
import Shader;

namespace SoulEngine {

/// @brief Concrete shader compiler backed by the Slang C++ API.
///
/// Pipeline compiles load the requested modules, find all pipeline entry points, compose them into one linked
/// Slang program, then emit SPIR-V plus linked pipeline-level reflection.
///
/// Init is lazy: the constructor is cheap; the first pipeline compile triggers the one-time creation of the
/// global session.
class SlangBackend final : public IShaderBackend {
  public:
    SlangBackend() = default;

    SlangBackend(const SlangBackend&)                    = delete;
    auto operator=(const SlangBackend&) -> SlangBackend& = delete;
    SlangBackend(SlangBackend&&)                         = delete;
    auto operator=(SlangBackend&&) -> SlangBackend&      = delete;

    ~SlangBackend() override = default;

    [[nodiscard]] auto CompileGraphics(const GraphicsCompileDesc& Desc)
        -> std::expected<ShaderGraphicsProgram, ErrorMessage> override;

    [[nodiscard]] auto CompileCompute(const ComputeCompileDesc& Desc)
        -> std::expected<ShaderComputeProgram, ErrorMessage> override;

    [[nodiscard]] auto CompileRayTracing(const RayTracingCompileDesc& Desc)
        -> std::expected<ShaderRayTracingProgram, ErrorMessage> override;

  private:
    /// One-time initialization of the Slang global session. Called from the first pipeline compile request.
    [[nodiscard]] auto Init() -> std::expected<void, ErrorMessage>;

    bool                                    m_bInitialized = false;
    Slang::ComPtr<slang::IGlobalSession>    m_GlobalSession;
    /// @brief Pre-built compiler options reused across all pipeline compile calls.
    ///
    /// Populated once by ResolveCompilerOptions() during Init().
    std::vector<slang::CompilerOptionEntry> m_CompilerOptions;
};

/// Auto-register the Slang backend with the compiler factory.
ShaderBackendFactory::AutoRegistrar<SlangBackend> RegSlang{magic_enum::enum_name(ShaderBackend::Slang)};

} // namespace SoulEngine
