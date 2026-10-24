/// @file   SlangCompiler/SlangUtils.cppm
/// @brief  Slang compiler internal utility helpers.
///
/// Stateless helper functions used by the Slang compilation pipeline:
///   - Compiler option resolution from engine config
///   - Diagnostic message extraction from Slang blobs
///   - Reflection query helpers (category checks, semantic name filters)

module;

#include <slang.h>

export module Slang.Utils;

import Core;

import std;

namespace SoulEngine {

CVar<bool> CVarShaderDebugInfo{
    "Shader.DebugInfo", false, "Emit SPIR-V debug info (OpLine/OpString) for RenderDoc", CVarTiming::Startup};

/// Build the session-level compiler options vector from the engine config.
export [[nodiscard]] auto ResolveCompilerOptions() -> std::vector<slang::CompilerOptionEntry>;

/// Extract a human-readable diagnostic string from a Slang IBlob.
export [[nodiscard]] auto DiagView(slang::IBlob* Diag) -> StringView;

/// Check whether a variable layout reflection contains a given parameter category.
export [[nodiscard]] auto HasCategory(slang::VariableLayoutReflection* VarLayout, slang::ParameterCategory Category)
    -> bool;

/// Determine whether a semantic name is a system-value semantic (SV_*).
export [[nodiscard]] auto IsSystemValueSemantic(StringView SemanticName) -> bool;

} // namespace SoulEngine
