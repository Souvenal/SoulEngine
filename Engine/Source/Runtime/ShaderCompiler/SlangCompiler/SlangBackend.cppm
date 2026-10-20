/// @file   SlangCompiler/SlangBackend.cppm
/// @brief  Public Slang shader compiler backend declaration.

module;

#include <slang.h>
#include <slang-com-ptr.h>

export module Slang:Backend;

import std;
import Core;
import ShaderCompiler;
import Shader;

namespace SoulEngine {

/// @brief Concrete shader compiler backed by the Slang C++ API.
///
/// Pipeline compiles load requested modules, compose them with all entry points, then emit SPIR-V and linked
/// pipeline reflection. Initialization creates the global session on the first compile request.
export class SlangBackend final : public IShaderBackend {
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
    [[nodiscard]] auto Init() -> std::expected<void, ErrorMessage>;

    bool                                    m_bInitialized = false;
    Slang::ComPtr<slang::IGlobalSession>    m_GlobalSession;
    std::vector<slang::CompilerOptionEntry> m_CompilerOptions;
};

} // namespace SoulEngine
