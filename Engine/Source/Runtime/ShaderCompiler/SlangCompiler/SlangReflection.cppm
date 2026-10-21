/// @file   SlangCompiler/SlangReflection.cppm
/// @brief  Slang shader reflection extraction — normalized bindings, push
///         constants, vertex inputs.
///
/// Stateless extraction functions that translate Slang reflection API objects into the engine's uniform
/// ShaderReflection representation. All Slang SDK dependencies are isolated to this partition.

module;

#include <slang.h>

export module Slang.Reflection;

import Slang.Types;
import Slang.Utils;

import std;
import Core;
import Shader;

namespace SoulEngine {

export [[nodiscard]] auto BuildGraphicsShaderReflection(slang::ShaderReflection*     ProgramLayout,
                                                        slang::EntryPointReflection* VertexEntryPoint,
                                                        slang::EntryPointReflection* FragmentEntryPoint)
    -> std::expected<ShaderReflection, ErrorMessage>;

export [[nodiscard]] auto BuildComputeShaderReflection(slang::ShaderReflection*     ProgramLayout,
                                                       slang::EntryPointReflection* ComputeEntryPoint)
    -> std::expected<ShaderReflection, ErrorMessage>;

export [[nodiscard]] auto BuildRayTracingShaderReflection(slang::ShaderReflection*                      ProgramLayout,
                                                          std::span<slang::EntryPointReflection* const> EntryPoints)
    -> std::expected<ShaderReflection, ErrorMessage>;

} // namespace SoulEngine
