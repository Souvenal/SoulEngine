module;

#include <assimp/material.h>
#include <entt/entt.hpp>

export module Material:AssimpLoader;

import Core;
import :Types;
import :Texture;

namespace SoulEngine {

/// @brief Loads one Assimp material into a renderer-neutral material record.
export struct MaterialAssimpLoader {
    using result_type = SPtr<MaterialRecord>;

    MaterialAssimpLoader() = default;

    auto operator()(const aiMaterial* Source, const Path& ModelPath) -> result_type;

  private:
    TextureDataCache m_TextureCache = {};
};

export using MaterialHandle      = entt::resource<MaterialRecord>;
export using ConstMaterialHandle = entt::resource<const MaterialRecord>;
export using MaterialAssimpCache = entt::resource_cache<MaterialRecord, MaterialAssimpLoader>;

export [[nodiscard]] auto MakeMaterialCacheKey(const Path& ModelPath, Uint32 MaterialIndex) -> entt::id_type;

} // namespace SoulEngine
