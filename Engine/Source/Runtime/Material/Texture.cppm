module;

#include <entt/entt.hpp>

export module Material:Texture;

import Core;
import RHI;
import std;
import :Types;

namespace SoulEngine {

/// @brief Loads one external image into an RHI sampled texture.
export struct TextureDataLoader {
    using result_type = std::shared_ptr<TextureData>;

    auto operator()(const Path& AssetPath) const -> result_type;
};

export using TextureDataCache = entt::resource_cache<TextureData, TextureDataLoader>;

} // namespace SoulEngine
