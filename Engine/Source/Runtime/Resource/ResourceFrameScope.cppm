export module Resource:FrameScope;

export import RHI;
export import :Manager;
export import :Ref;
export import std;

export namespace SoulEngine::Resource {

template <typename Tuple>
struct FrameResourcePinTuple;

template <ManagedRHIResource... T>
struct FrameResourcePinTuple<std::tuple<T...>> {
    using Type = std::tuple<std::vector<ResourcePin<T>>...>;
};

using FrameResourcePins = typename FrameResourcePinTuple<ManagedRHIResourceTypes>::Type;

/// @brief Per-frame resource lifetime scope for command-list observer pointers.
///
/// RHI command lists intentionally store raw observer pointers, not Resource
/// handles or owning smart pointers.  Renderer code should therefore acquire
/// every command-list resource through this scope: `Acquire()` pins the
/// ResourceContext-owned payload, stores that pin inside the frame packet, and
/// returns the raw RHI pointer to write into the command.
///
/// This shape is deliberate.  It removes the easy-to-forget two-step sequence
/// of "pin from Manager, then remember to add the pin to the frame packet".
/// Launch keeps `FrameResourceScope` alive until RHILoop finishes Execute(),
/// so every observer pointer emitted by the renderer remains valid for the
/// command list's execution window.
struct FrameResourceScope {
    template <ManagedRHIResource T>
    [[nodiscard]] auto Acquire(const ResourceHandle<T>& Handle) -> T* {
        auto Pin = Manager::Get().Pin(Handle);
        if (!Pin)
            return nullptr;

        auto* Ptr = Pin.Get();
        Store<T>(std::move(Pin));
        return Ptr;
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto Acquire(const ResourceRef<T>& Ref) -> T* {
        return Acquire(Ref.GetHandle());
    }

  private:
    FrameResourcePins Pins = {};

    template <ManagedRHIResource T>
    auto Store(ResourcePin<T> Pin) -> void {
        if (!Pin)
            return;

        std::get<std::vector<ResourcePin<T>>>(Pins).emplace_back(std::move(Pin));
    }
};

} // namespace SoulEngine::Resource
