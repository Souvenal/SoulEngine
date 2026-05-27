/// @file   SlangCompiler/SlangUtils.cppm
/// @brief  Slang compiler internal utility helpers.
///
/// Stateless helper functions used by the Slang compilation pipeline:
///   - Compiler option resolution from engine config
///   - Diagnostic message extraction from Slang blobs
///   - Reflection query helpers (category checks, semantic name filters)

module;

#include <slang.h>

export module Slang:Utils;

export import Core;

export import std;

namespace SoulEngine::ShaderCompiler::SlangCompiler {

using namespace SoulEngine::Core;

/// Build the session-level compiler options vector from the engine config.
[[nodiscard]] auto ResolveCompilerOptions() -> std::vector<slang::CompilerOptionEntry> {
    std::vector<slang::CompilerOptionEntry> Options;

    // ── EmitSpirvDirectly ─────────────────────────────────────────────────
    Options.push_back({
        .name  = slang::CompilerOptionName::EmitSpirvDirectly,
        .value = {.intValue0 = 1},
    });

    // ── VulkanUseEntryPointName ───────────────────────────────────────────
    Options.push_back({
        .name  = slang::CompilerOptionName::VulkanUseEntryPointName,
        .value = {.intValue0 = 1},
    });

    // ── DebugInformation ──────────────────────────────────────────────────
    const auto& ShaderCfg = ConfigManager::Get().GetConfig().Shader;
    const bool  WantDebug = ShaderCfg.DebugInfo.value_or(false);
    if (WantDebug) {
        Options.push_back({
            .name  = slang::CompilerOptionName::DebugInformation,
            .value = {.intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD},
        });
    }

    // ── Optimization ──────────────────────────────────────────────────
    Options.push_back({
        .name  = slang::CompilerOptionName::Optimization,
        .value = {.intValue0 = SLANG_OPTIMIZATION_LEVEL_HIGH},
    });

    return Options;
}

/// Extract a human-readable diagnostic string from a Slang IBlob.
[[nodiscard]] auto DiagView(slang::IBlob* Diag) -> StringView {
    if (!Diag)
        return {"unknown error"};
    return {static_cast<const char*>(Diag->getBufferPointer())};
}

/// Check whether a variable layout reflection contains a given parameter category.
[[nodiscard]] auto HasCategory(slang::VariableLayoutReflection* VarLayout, slang::ParameterCategory Category) -> bool {
    if (!VarLayout)
        return false;

    for (unsigned int Index = 0; Index < VarLayout->getCategoryCount(); ++Index) {
        if (VarLayout->getCategoryByIndex(Index) == Category)
            return true;
    }
    return false;
}

/// Determine whether a semantic name is a system-value semantic (SV_*).
[[nodiscard]] auto IsSystemValueSemantic(StringView SemanticName) -> bool {
    if (SemanticName.size() < 3)
        return false;

    return std::toupper(static_cast<unsigned char>(SemanticName[0])) == 'S' &&
           std::toupper(static_cast<unsigned char>(SemanticName[1])) == 'V' && SemanticName[2] == '_';
}

} // namespace SoulEngine::ShaderCompiler::SlangCompiler
