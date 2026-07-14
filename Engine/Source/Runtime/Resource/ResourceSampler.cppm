export module Resource:Sampler;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

namespace SoulEngine::Resource {
namespace {

[[nodiscard]] auto MakeSamplerKey(const RHI::SamplerDesc& Desc) -> String {
    return Format("sampler|profile={}", static_cast<Uint32>(Desc.Profile));
}

[[nodiscard]] auto ValidateSamplerDesc(const RHI::SamplerDesc& Desc) -> std::expected<void, ErrorMessage> {
    if (Desc.Profile == RHI::SamplerProfile::Unknown)
        return std::unexpected(ErrorMessage("Sampler profile is unknown"));

    return {};
}

} // namespace

[[nodiscard]] auto SubmitSamplerRequest(ResourceContext& Context, const RHI::SamplerDesc& Desc)
    -> ResourceHandle<RHI::Sampler> {
    auto Key    = MakeSamplerKey(Desc);
    auto Work   = BeginResourceWork<RHI::Sampler>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    if (auto R = ValidateSamplerDesc(Desc); !R) {
        PublishResourceFailed<RHI::Sampler>(
            Context, Handle.GetGeneration(), Key, R.error().Append(Format("Invalid sampler request '{}'", Key)));
        return Handle;
    }

    LogDebug("Sampler requested '{}'", Key);

    auto* Graph      = Work.Graph;
    auto* ContextPtr = &Context;
    Graph->Enqueue(ThreadQueue::RHI, [ContextPtr, Generation = Handle.GetGeneration(), Key = String(Key), Desc] {
        auto& Context = *ContextPtr;
        if (Context.IsShutdownRequested()) {
            LogDebug("Async sampler RHI commit discarded after shutdown '{}'", Key);
            return;
        }

        if (!MarkResourceRhiCommitting<RHI::Sampler>(Context, Key, Generation))
            return;

        auto Result = RHI::RenderDevice::Get().CreateSampler(Desc);
        if (!Result) {
            PublishResourceFailed<RHI::Sampler>(
                Context, Generation, Key, Result.error().Append(Format("Failed to create sampler '{}'", Key)));
            return;
        }

        PublishResourceReady<RHI::Sampler>(Context, Generation, Key, Resource<RHI::Sampler>{.Object = std::move(*Result)});
    });

    return Handle;
}

} // namespace SoulEngine::Resource
