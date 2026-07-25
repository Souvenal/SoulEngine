export module Resource:Sampler;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

namespace SoulEngine {
namespace {

[[nodiscard]] auto MakeSamplerKey(const RHISamplerDesc& Desc) -> String {
    return Format("sampler|profile={}", static_cast<Uint32>(Desc.Profile));
}

[[nodiscard]] auto ValidateSamplerDesc(const RHISamplerDesc& Desc) -> std::expected<void, ErrorMessage> {
    if (Desc.Profile == RHISamplerProfile::Unknown)
        return std::unexpected(ErrorMessage("Sampler profile is unknown"));

    return {};
}

} // namespace

[[nodiscard]] auto SubmitSamplerRequest(ResourceContext& Context, const RHISamplerDesc& Desc)
    -> ResourceHandle<RHISampler> {
    auto Key    = MakeSamplerKey(Desc);
    auto Work   = BeginResourceWork<RHISampler>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    if (auto R = ValidateSamplerDesc(Desc); !R) {
        PublishResourceFailed<RHISampler>(
            Context, Handle.GetGeneration(), Key, R.error().Append(Format("Invalid sampler request '{}'", Key)));
        return Handle;
    }

    LogDebug("Sampler requested '{}'", Key);

    auto* ContextPtr = &Context;
    auto EnqueueResult = TaskGraph::Get().Enqueue(
        ThreadQueue::RHI,
        [ContextPtr, Generation = Handle.GetGeneration(), Key = String(Key), Desc] {
        auto& Context = *ContextPtr;
        if (Context.IsShutdownRequested()) {
            LogDebug("Async sampler RHI commit discarded after shutdown '{}'", Key);
            return;
        }

        if (!MarkResourceRhiCommitting<RHISampler>(Context, Key, Generation))
            return;

        auto Result = RHIRenderDevice::Get().CreateSampler(Desc);
        if (!Result) {
            PublishResourceFailed<RHISampler>(
                Context, Generation, Key, Result.error().Append(Format("Failed to create sampler '{}'", Key)));
            return;
        }

        PublishResourceReady<RHISampler>(Context, Generation, Key, Resource<RHISampler>{.Object = std::move(*Result)});
        });
    if (!EnqueueResult) {
        PublishResourceFailed<RHISampler>(
            Context,
            Handle.GetGeneration(),
            Key,
            EnqueueResult.error().Append(
                Format("Failed to enqueue async {} work '{}'", ResourceTraits<RHISampler>::Info.Label, Key)));
    }

    return Handle;
}

} // namespace SoulEngine
