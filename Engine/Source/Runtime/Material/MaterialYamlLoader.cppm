module;

#include <entt/entt.hpp>

export module Material.YamlLoader;

import Core;
import Material.Types;
import Material.Texture;

namespace SoulEngine {

/// @brief Loads one complete Material YAML file into a runtime material record.
export struct MaterialYamlLoader {
    using result_type = SPtr<MaterialRecord>;

    auto operator()(const Path& MaterialPath, const Path& AssetRoot) -> result_type;

  private:
    // TODO: Share this cache with MaterialAssimpLoader once material loading performance matters.
    TextureDataCache m_TextureCache = {};
    auto             LoadTexture(MaterialRecord& Material, TextureType Type, const String& Path) -> void;
};

export using MaterialYamlCache = entt::resource_cache<MaterialRecord, MaterialYamlLoader>;

} // namespace SoulEngine
