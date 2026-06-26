export module Resource:Ref;

import :Context;
export import :Types;

export import std;

export namespace SoulEngine::Resource {

class Manager;

/// @brief Move-only logical owner for a resource request.
///
/// `ResourceRef` expresses that a runtime system still wants the resource.
/// It does not expose ready payload pointers and does not protect command-list
/// observer pointers. Render code must acquire frame-safe payload access
/// through `FrameResourceScope`, which stores a separate `ResourcePin`.
template <ManagedRHIResource T>
class ResourceRef {
  public:
    ResourceRef() = default;

    ResourceRef(const ResourceRef&)                    = delete;
    auto operator=(const ResourceRef&) -> ResourceRef& = delete;

    ResourceRef(ResourceRef&& Other) noexcept {
        m_Context = std::exchange(Other.m_Context, nullptr);
        m_Handle  = std::exchange(Other.m_Handle, {});
    }

    auto operator=(ResourceRef&& Other) noexcept -> ResourceRef& {
        if (this != &Other) {
            Reset();
            m_Context = std::exchange(Other.m_Context, nullptr);
            m_Handle  = std::exchange(Other.m_Handle, {});
        }
        return *this;
    }

    ~ResourceRef() {
        Reset();
    }

    [[nodiscard]] explicit operator bool() const {
        return m_Handle.IsValid();
    }

    [[nodiscard]] auto GetHandle() const -> const ResourceHandle<T>& {
        return m_Handle;
    }

    auto Reset() -> void {
        if (!m_Context || !m_Handle.IsValid())
            return;

        m_Context->ReleaseRef(m_Handle);
        m_Context = nullptr;
        m_Handle  = {};
    }

  private:
    friend class Manager;

    // Private construction retains logical demand; if the context rejects the
    // handle, the ref stays empty.
    explicit ResourceRef(ResourceContext& Context, ResourceHandle<T> Handle) {
        if (!Context.AddRef(Handle))
            return;

        m_Context = &Context;
        m_Handle  = std::move(Handle);
    }

    // Non-owning ResourceContext observer; ResourceRef owns logical demand only.
    ResourceContext*   m_Context = nullptr;
    ResourceHandle<T>  m_Handle  = {};
};

} // namespace SoulEngine::Resource
