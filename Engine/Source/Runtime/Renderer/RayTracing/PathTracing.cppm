export module Renderer:RayTracing.PathTracing;

import Core;
import RHI;


export import std;

export namespace SoulEngine {

struct PathTracingPassInput {
    RHIRef<RHITopLevelAccelerationStructure>          Tlas = nullptr;
    RHIRef<RHIRenderTarget>                           Output = nullptr;
    RHIRef<RHIRenderTarget>                           Accumulation = nullptr;
    RHIRef<RHIRenderTarget>                           PrimaryNormal = nullptr;
    RHIRef<RHIRenderTarget>                           PrimaryEntityId = nullptr;
    RHIRef<RHISampler>                                LinearSampler = nullptr;
    RHIRef<RHISampler>                                AnisotropicSampler = nullptr;
    RHIRefArray<RHISampledTexture>                    Textures = {};
    RHIRef<RHITransientShaderStorageBuffer>           InstancesBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer>           GeometryBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer>           MaterialBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer>           LightBuffer = nullptr;
    RHIRef<RHITransientConstantBuffer>                FrameBuffer = nullptr;
    RHIRef<RHITransientConstantBuffer>                ViewBuffer = nullptr;
    std::vector<RHIAccelerationStructureInstance>     AccelerationInstances = {};
    Uint32                                            Width = 0;
    Uint32                                            Height = 0;
};

class PathTracingPass final : public IRHIRayTracingPass {
  public:
    explicit PathTracingPass(RHIRef<RHIRayTracingPipeline> Pipeline)
        : IRHIRayTracingPass(std::move(Pipeline)) {}

    auto SetInput(PathTracingPassInput Input) -> void {
        m_Input = std::move(Input);
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        auto Input = std::move(m_Input);

        if (!GetShaderBindingSet(m_Pipeline) || !Input.Tlas || !Input.Output || !Input.Accumulation || !Input.PrimaryNormal ||
            !Input.PrimaryEntityId || !Input.LinearSampler || !Input.AnisotropicSampler || !Input.Textures ||
            !Input.InstancesBuffer || !Input.GeometryBuffer || !Input.MaterialBuffer || !Input.LightBuffer ||
            !Input.FrameBuffer || !Input.ViewBuffer || Input.AccelerationInstances.empty() || Input.Width == 0 ||
            Input.Height == 0)
            return std::unexpected(ErrorMessage("Path tracing pass resources are not ready"));

        if (auto R = BindResource("g_samplers.uSamplerLinear", std::move(Input.LinearSampler), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_samplers.uSamplerAniso", std::move(Input.AnisotropicSampler), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource(std::move(Input.Textures)); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.tlas", Input.Tlas, true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.output", std::move(Input.Output), false); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.accumulation", std::move(Input.Accumulation), false); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.primaryNormal", std::move(Input.PrimaryNormal), false); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.primaryEntityId", std::move(Input.PrimaryEntityId), false); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.instances", std::move(Input.InstancesBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.geometries", std::move(Input.GeometryBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.materials", std::move(Input.MaterialBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.lights", std::move(Input.LightBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.frame", std::move(Input.FrameBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_rayTracing.view", std::move(Input.ViewBuffer), true); !R)
            return std::unexpected(R.error());

        BuildOrUpdateTopLevelAccelerationStructure(Input.Tlas, Input.AccelerationInstances);
        if (auto R = Commit(); !R)
            return std::unexpected(R.error());
        TraceRays(Input.Width, Input.Height);
        return {};
    }

  private:
    PathTracingPassInput m_Input = {};
};

} // namespace SoulEngine
