module;

#include <entt/entity/entity.hpp>

export module Renderer:GBuffer;

import RHI;
export import std;

export namespace SoulEngine {

/// @brief Shared renderer contract for visibility data emitted by Raster and RayTracing.
struct GBuffer {
    static constexpr Uint32 ColorAttachmentCount = 4;
    static constexpr std::array<RHIFormat, ColorAttachmentCount> ColorFormats = {
        RHIFormat::B8G8R8A8_UNORM,
        RHIFormat::R16G16B16A16_SFLOAT,
        RHIFormat::R32_UINT,
        RHIFormat::R16G16B16A16_SFLOAT,
    };
    static constexpr Uint32 BackgroundEntityId = 0;

    [[nodiscard]] static auto EncodeEntityId(entt::entity Entity) -> Uint32 {
        return static_cast<Uint32>(Entity) + 1u;
    }
};

[[nodiscard]] inline auto MakeGBufferColorFormats() -> std::vector<RHIFormat> {
    return {GBuffer::ColorFormats.begin(), GBuffer::ColorFormats.end()};
}

} // namespace SoulEngine
