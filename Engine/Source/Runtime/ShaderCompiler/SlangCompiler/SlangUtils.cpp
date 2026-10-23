/// @file   SlangCompiler/SlangUtils.cpp
/// @brief  Slang compiler internal utility implementations.

module;

#include <slang.h>

module Slang.Utils;

import Core;
import std;

namespace SoulEngine {

[[nodiscard]] auto ResolveCompilerOptions() -> std::vector<slang::CompilerOptionEntry> {
    std::vector<slang::CompilerOptionEntry> Options;
    // EmitSpirvDirectly

    Options.push_back({
        .name  = slang::CompilerOptionName::EmitSpirvDirectly,
        .value = {.intValue0 = 1},
    });
    // VulkanUseEntryPointName

    Options.push_back({
        .name  = slang::CompilerOptionName::VulkanUseEntryPointName,
        .value = {.intValue0 = 1},
    });
    // DebugInformation

    const bool WantDebug = CVarShaderDebugInfo.Get();
    if (WantDebug) {
        Options.push_back({
            .name  = slang::CompilerOptionName::DebugInformation,
            .value = {.intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD},
        });
    }
    // Optimization

    Options.push_back({
        .name  = slang::CompilerOptionName::Optimization,
        .value = {.intValue0 = SLANG_OPTIMIZATION_LEVEL_HIGH},
    });

    return Options;
}

[[nodiscard]] auto DiagView(slang::IBlob* Diag) -> StringView {
    if (!Diag)
        return {"unknown error"};
    return {static_cast<const char*>(Diag->getBufferPointer())};
}

[[nodiscard]] auto HasCategory(slang::VariableLayoutReflection* VarLayout, slang::ParameterCategory Category) -> bool {
    if (!VarLayout)
        return false;

    for (unsigned int Index = 0; Index < VarLayout->getCategoryCount(); ++Index) {
        if (VarLayout->getCategoryByIndex(Index) == Category)
            return true;
    }
    return false;
}

[[nodiscard]] auto IsSystemValueSemantic(StringView SemanticName) -> bool {
    if (SemanticName.size() < 3)
        return false;

    return std::toupper(static_cast<unsigned char>(SemanticName[0])) == 'S' &&
           std::toupper(static_cast<unsigned char>(SemanticName[1])) == 'V' && SemanticName[2] == '_';
}

} // namespace SoulEngine
