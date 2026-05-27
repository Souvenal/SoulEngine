export module RHI:Reflection;

export import Core;
import Shader;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

/// @brief Bitmask of pipeline-stage visibility for merged shader interfaces.
enum class ShaderStageMask : Uint32 {
    None          = 0,
    Vertex        = 1u << 0,
    Fragment      = 1u << 1,
    Compute       = 1u << 2,
    Hull          = 1u << 3,
    Domain        = 1u << 4,
    Geometry      = 1u << 5,
    Mesh          = 1u << 6,
    Amplification = 1u << 7,
};

[[nodiscard]] inline auto operator|(ShaderStageMask Left, ShaderStageMask Right) -> ShaderStageMask {
    return static_cast<ShaderStageMask>(static_cast<Uint32>(Left) | static_cast<Uint32>(Right));
}

inline auto operator|=(ShaderStageMask& Left, ShaderStageMask Right) -> ShaderStageMask& {
    Left = Left | Right;
    return Left;
}

[[nodiscard]] inline auto ToShaderStageMask(Shader::Stage Stage) -> ShaderStageMask {
    switch (Stage) {
    case Shader::Stage::Unknown:
        return ShaderStageMask::None;
    case Shader::Stage::Vertex:
        return ShaderStageMask::Vertex;
    case Shader::Stage::Fragment:
        return ShaderStageMask::Fragment;
    case Shader::Stage::Compute:
        return ShaderStageMask::Compute;
    case Shader::Stage::Hull:
        return ShaderStageMask::Hull;
    case Shader::Stage::Domain:
        return ShaderStageMask::Domain;
    case Shader::Stage::Geometry:
        return ShaderStageMask::Geometry;
    case Shader::Stage::Mesh:
        return ShaderStageMask::Mesh;
    case Shader::Stage::Amplification:
        return ShaderStageMask::Amplification;
    }
    return ShaderStageMask::None;
}

struct PipelineResourceBinding {
    Uint32               Set        = 0;
    Uint32               Binding    = 0;
    Shader::ResourceType Type       = Shader::ResourceType::Unknown;
    Uint32               ArrayCount = 1;
    ShaderStageMask      StageMask  = ShaderStageMask::None;

    [[nodiscard]] auto operator==(const PipelineResourceBinding&) const -> bool = default;
};

struct PipelinePushConstantRange {
    Uint32          Offset    = 0;
    Uint32          Size      = 0;
    ShaderStageMask StageMask = ShaderStageMask::None;

    [[nodiscard]] auto operator==(const PipelinePushConstantRange&) const -> bool = default;
};

/// @brief Merged pipeline-level shader-visible resource interface.
///
/// TODO: Promote this to a first-class RHI object only after descriptor /
/// push-constant binding APIs stabilize across backends.
struct PipelineResourceLayout {
    std::vector<PipelineResourceBinding>   Bindings      = {};
    std::vector<PipelinePushConstantRange> PushConstants = {};

    [[nodiscard]] auto operator==(const PipelineResourceLayout&) const -> bool = default;
};

/// @brief Merged graphics-pipeline shader reflection.
///
/// Resource layout and shader-side vertex input are intentionally distinct
/// concepts.  A future CPU-side vertex-buffer stream-layout abstraction can
/// map application buffers onto these reflected vertex-input requirements.
struct GraphicsPipelineShaderInterface {
    PipelineResourceLayout                    ResourceLayout = {};
    std::vector<Shader::VertexInputAttribute> VertexInputs   = {};
};

[[nodiscard]] inline auto IsGraphicsStage(Shader::Stage Stage) -> bool {
    switch (Stage) {
    case Shader::Stage::Unknown:
    case Shader::Stage::Compute:
        return false;
    case Shader::Stage::Vertex:
    case Shader::Stage::Fragment:
    case Shader::Stage::Hull:
    case Shader::Stage::Domain:
    case Shader::Stage::Geometry:
    case Shader::Stage::Mesh:
    case Shader::Stage::Amplification:
        return true;
    }
    return false;
}

[[nodiscard]] inline auto MergeGraphicsPipelineShaderInterface(
    const Shader::Program& VertexProgram,
    std::optional<Shader::Program> FragmentProgram)
    -> std::expected<GraphicsPipelineShaderInterface, ErrorMessage> {
    if (!VertexProgram.Code || VertexProgram.Code->empty()) {
        return std::unexpected(
            ErrorMessage(Format("Vertex shader '{}' has no compiled bytecode", VertexProgram.EntryPointName)));
    }
    if (!VertexProgram.Reflection) {
        return std::unexpected(
            ErrorMessage(Format("Vertex shader '{}' is missing normalized reflection", VertexProgram.EntryPointName)));
    }

    std::map<std::pair<Uint32, Uint32>, PipelineResourceBinding>   BindingMap;
    std::map<std::pair<Uint32, Uint32>, PipelinePushConstantRange> PushConstantMap;
    std::set<Shader::Stage>                                        SeenStages;
    std::optional<std::vector<Shader::VertexInputAttribute>>       VertexInputs = std::nullopt;

    auto ProcessProgram = [&](const Shader::Program& Program) -> std::expected<void, ErrorMessage> {
        if (!Program.Code || Program.Code->empty()) {
            return std::unexpected(
                ErrorMessage(Format("Shader program '{}' has no compiled bytecode", Program.EntryPointName)));
        }
        if (!Program.Reflection) {
            return std::unexpected(
                ErrorMessage(Format("Shader program '{}' is missing normalized reflection", Program.EntryPointName)));
        }
        if (!IsGraphicsStage(Program.Stage)) {
            return std::unexpected(
                ErrorMessage(Format("Shader program '{}' uses non-graphics stage {} in a graphics pipeline",
                                    Program.EntryPointName,
                                    static_cast<int>(Program.Stage))));
        }
        if (!SeenStages.insert(Program.Stage).second) {
            return std::unexpected(
                ErrorMessage(Format("Graphics pipeline contains duplicate stage {}", static_cast<int>(Program.Stage))));
        }

        const auto StageMask = ToShaderStageMask(Program.Stage);
        for (const auto& Binding : Program.Reflection->Bindings) {
            auto Key = std::make_pair(Binding.Set, Binding.Binding);
            auto It  = BindingMap.find(Key);
            if (It == BindingMap.end()) {
                BindingMap.emplace(Key,
                                   PipelineResourceBinding{
                                       .Set        = Binding.Set,
                                       .Binding    = Binding.Binding,
                                       .Type       = Binding.Type,
                                       .ArrayCount = Binding.ArrayCount,
                                       .StageMask  = StageMask,
                                   });
                continue;
            }

            if (It->second.Type != Binding.Type || It->second.ArrayCount != Binding.ArrayCount) {
                return std::unexpected(ErrorMessage(
                    Format("Conflicting shader bindings for set {} binding {}", Binding.Set, Binding.Binding)));
            }
            It->second.StageMask |= StageMask;
        }

        for (const auto& PushConstant : Program.Reflection->PushConstants) {
            auto Key = std::make_pair(PushConstant.Offset, PushConstant.Size);
            auto It  = PushConstantMap.find(Key);
            if (It != PushConstantMap.end()) {
                It->second.StageMask |= StageMask;
                continue;
            }

            for (const auto& [ExistingKey, ExistingRange] : PushConstantMap) {
                const auto ExistingBegin = ExistingRange.Offset;
                const auto ExistingEnd   = ExistingRange.Offset + ExistingRange.Size;
                const auto CurrentBegin  = PushConstant.Offset;
                const auto CurrentEnd    = PushConstant.Offset + PushConstant.Size;
                const bool Overlaps      = CurrentBegin < ExistingEnd && ExistingBegin < CurrentEnd;
                if (Overlaps) {
                    return std::unexpected(
                        ErrorMessage(Format("Conflicting push-constant ranges: [{}..{}) overlaps [{}..{})",
                                            CurrentBegin,
                                            CurrentEnd,
                                            ExistingBegin,
                                            ExistingEnd)));
                }
            }

            PushConstantMap.emplace(Key,
                                    PipelinePushConstantRange{
                                        .Offset    = PushConstant.Offset,
                                        .Size      = PushConstant.Size,
                                        .StageMask = StageMask,
                                    });
        }

        if (Program.Stage == Shader::Stage::Vertex)
            VertexInputs = Program.Reflection->VertexInputs;

        return {};
    };

    if (auto R = ProcessProgram(VertexProgram); !R)
        return std::unexpected(R.error());
    if (FragmentProgram)
        if (auto R = ProcessProgram(*FragmentProgram); !R)
            return std::unexpected(R.error());

    GraphicsPipelineShaderInterface Result{};
    Result.ResourceLayout.Bindings.reserve(BindingMap.size());
    for (const auto& [Key, Binding] : BindingMap)
        Result.ResourceLayout.Bindings.push_back(Binding);

    Result.ResourceLayout.PushConstants.reserve(PushConstantMap.size());
    for (const auto& [Key, Range] : PushConstantMap)
        Result.ResourceLayout.PushConstants.push_back(Range);

    if (VertexInputs)
        Result.VertexInputs = std::move(*VertexInputs);

    return Result;
}

} // namespace SoulEngine::RHI
