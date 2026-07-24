export module Resource:Array;

export import Core;
export import RHI;
import :Manager;
export import std;

export namespace SoulEngine {

/// @brief Mutable resource array that retains every assigned resource ref.
template <ManagedRHIResource T>
class ResourceArray {
  public:
    ResourceArray() = default;

    ResourceArray(const ResourceArray&)                    = delete;
    auto operator=(const ResourceArray&) -> ResourceArray& = delete;
    ResourceArray(ResourceArray&&) noexcept                = default;
    auto operator=(ResourceArray&&) noexcept -> ResourceArray& = default;

    [[nodiscard]] auto GetSize() const -> Uint32 {
        return static_cast<Uint32>(m_Resources.size());
    }

    [[nodiscard]] auto Set(Uint32 Slot, ResourceRef<T> Resource)
        -> std::expected<void, ErrorMessage> {
        if (!Resource)
            return std::unexpected(ErrorMessage(Format("Resource array slot {} has an invalid resource ref", Slot)));

        if (Slot >= m_Resources.size())
            m_Resources.resize(Slot + 1);
        m_Resources[Slot] = std::move(Resource);
        m_RhiArray.Set(Slot, nullptr);
        return {};
    }

    /// @brief Resolve resource observers into an RHI array snapshot when all assigned resources are ready.
    [[nodiscard]] auto TryGetReady() -> std::optional<RHIResourceArray<T>> {
        for (Uint32 Slot = 0; Slot < m_Resources.size(); ++Slot) {
            auto& ResourceRef = m_Resources[Slot];
            if (!ResourceRef) {
                m_RhiArray.Set(Slot, nullptr);
                continue;
            }

            auto* Resource = ResourceManager::Get().TryGetReady(ResourceRef);
            if (!Resource)
                return std::nullopt;
            m_RhiArray.Set(Slot, Resource);
        }

        return m_RhiArray;
    }

  private:
    std::vector<ResourceRef<T>> m_Resources = {};
    RHIResourceArray<T>       m_RhiArray  = {};
};

} // namespace SoulEngine
