module RHI:RenderDevice.impl;

import Core;
import WindowSystem;

import :RenderDevice;
import :Types;
import :Ref;
import :Pipeline;
import :Pass;
import :Command;

import std;

namespace SoulEngine {

/// @brief Error for backend methods a concrete device leaves unimplemented.
///
/// Tests run a base RHIRenderDevice (or a small override) to keep RenderGraph's realize step off the real
/// GPU; reaching this default there is expected, in production it means a backend missed an override.
[[nodiscard]] auto NotImplemented(StringView Method) -> ErrorMessage {
    auto Message = ErrorMessage(Format("RHIRenderDevice: '{}' is not implemented by this device", Method));
    LogError("{}", Message.ToString());
    return Message;
}

auto RHIRenderDevice::Initialize(IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("Initialize"));
}

auto RHIRenderDevice::GetBackendType() const -> RHIBackendType {
    return RHIBackendType::Unknown;
}

auto RHIRenderDevice::CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
    -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateBuffer"));
}

auto RHIRenderDevice::CreateReadback(StringView Name, Uint64 SizeBytes)
    -> std::expected<RHIRef<RHIReadback>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateReadback"));
}

auto RHIRenderDevice::UploadBuffer(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
    -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("UploadBuffer"));
}

auto RHIRenderDevice::CreateTexture(StringView Name, const RHITextureDesc& Desc)
    -> std::expected<RHIRef<RHITexture>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateTexture"));
}

auto RHIRenderDevice::RegisterBindlessTextures(std::span<const RHIRef<RHITexture>> Textures)
    -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("RegisterBindlessTextures"));
}

auto RHIRenderDevice::FindBindlessTextureIndex(const RHIRef<RHITexture>& Texture) const -> std::optional<Uint32> {
    return std::nullopt;
}

auto RHIRenderDevice::UploadTexture(RHIRef<RHITexture> Texture, std::span<const std::byte> Data)
    -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("UploadTexture"));
}

auto RHIRenderDevice::CreateSampler(StringView Name, const RHISamplerDesc& Desc)
    -> std::expected<RHIRef<RHISampler>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateSampler"));
}

auto RHIRenderDevice::CreateShaderBindingSet(StringView Name, const RHIShaderBindingSetDesc& Desc)
    -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateShaderBindingSet"));
}

auto RHIRenderDevice::CreateGraphicsPipeline(StringView Name, const RHIGraphicsPipelineDesc& Desc)
    -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateGraphicsPipeline"));
}

auto RHIRenderDevice::CreateComputePipeline(StringView Name, const RHIComputePipelineDesc& Desc)
    -> std::expected<RHIRef<RHIComputePipeline>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateComputePipeline"));
}

auto RHIRenderDevice::CreateRayTracingPipeline(StringView Name, const RHIRayTracingPipelineDesc& Desc)
    -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateRayTracingPipeline"));
}

auto RHIRenderDevice::CreateBottomLevelAccelerationStructure(StringView                                     Name,
                                                             const RHIBottomLevelAccelerationStructureDesc& Desc)
    -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateBottomLevelAccelerationStructure"));
}

auto RHIRenderDevice::CreateTopLevelAccelerationStructure(StringView                                  Name,
                                                          const RHITopLevelAccelerationStructureDesc& Desc)
    -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> {
    return std::unexpected(NotImplemented("CreateTopLevelAccelerationStructure"));
}

auto RHIRenderDevice::BuildBlas(RHIRef<RHIBottomLevelAccelerationStructure> Blas) -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("BuildBlas"));
}

auto RHIRenderDevice::BuildTlas(RHIRef<RHITopLevelAccelerationStructure> Tlas) -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("BuildTlas"));
}

auto RHIRenderDevice::Tick() -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("Tick"));
}

auto RHIRenderDevice::BeginFrame() -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("BeginFrame"));
}

auto RHIRenderDevice::Execute(RenderResult& Result) -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("Execute"));
}

auto RHIRenderDevice::EndFrame() -> std::expected<RHIFrameCompletion, ErrorMessage> {
    return std::unexpected(NotImplemented("EndFrame"));
}

auto RHIRenderDevice::WaitFinish(const RHIFrameCompletion& Completion) -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("WaitFinish"));
}

auto RHIRenderDevice::GetCurrentFrameIndex() const -> Uint32 {
    return 0;
}

auto RHIRenderDevice::WaitIdle() -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("WaitIdle"));
}

auto RHIRenderDevice::Shutdown() -> std::expected<void, ErrorMessage> {
    return std::unexpected(NotImplemented("Shutdown"));
}

auto RHIRenderDevice::NextTransientBufferId() -> Uint64 {
    static std::atomic<Uint64> NextId = 1;
    return NextId.fetch_add(1, std::memory_order_relaxed);
}

// Static member definitions

UPtr<RHIRenderDevice> RHIRenderDevice::s_Instance = nullptr;

auto RHIRenderDevice::Create(StringView Backend, IWindowSystem* WindowSys) -> std::expected<void, ErrorMessage> {
    LogInfo("Configured RHI backend: '{}'", Backend);

    // Verify the backend is registered before attempting creation.
    if (!RHIBackendFactory::Get().Contains(Backend)) {
        String Supported;
        auto   Names = RHIBackendFactory::Get().Keys();
        for (std::size_t i = 0; i < Names.size(); ++i) {
            if (i > 0)
                Supported += ", ";
            Supported += Names[i];
        }
        return std::unexpected(
            ErrorMessage(Format("Unsupported RHI backend: '{}'. Supported backends: {}", Backend, Supported)));
    }

    // Create the backend via self-registering factory — no switch, no concrete backend imports needed.
    auto Ctx = RHIBackendFactory::Get().Create(Backend);
    if (!Ctx)
        return std::unexpected(ErrorMessage(Format("Failed to create '{}' RHI backend", Backend)));
    if (!WindowSys)
        return std::unexpected(ErrorMessage("RHI backend requires a window system"));
    if (auto R = Ctx->Initialize(WindowSys); !R) {
        return std::unexpected(R.error().Append(Format("Failed to initialize '{}' RHI backend", Backend)));
    }

    s_Instance             = std::move(Ctx);
    GDeferredDeletionQueue = &GDeferredDeletionQueueStorage;
    return {};
}

auto RHIRenderDevice::Destroy() -> std::expected<void, ErrorMessage> {
    if (!s_Instance)
        return {};

    auto ShutdownResult = s_Instance->Shutdown();
    s_Instance.reset();
    GDeferredDeletionQueue = nullptr;
    if (!ShutdownResult)
        return std::unexpected(ShutdownResult.error());
    return {};
}

auto RHIRenderDevice::Get() -> RHIRenderDevice& {
    return *s_Instance;
}

} // namespace SoulEngine
