module Renderer:Common.Utilities.impl;

import :Common.Utilities;
import Core;
import Material;
import RHI;
import Scene;
import std;

namespace SoulEngine {

[[nodiscard]] auto ResolveBindlessTextureIndices(std::span<const RHIRef<RHITexture>> Textures)
    -> std::expected<std::unordered_map<Uint64, Uint32>, ErrorMessage> {
    auto&                           Device  = RHIRenderDevice::Get();
    std::vector<RHIRef<RHITexture>> Missing = {};
    for (const auto& Texture : Textures)
        if (Texture && !Device.FindBindlessTextureIndex(Texture))
            Missing.push_back(Texture);
    if (!Missing.empty())
        if (auto R = Device.RegisterBindlessTextures(Missing); !R)
            return std::unexpected(R.error().Append("Bindless texture registration failed"));

    std::unordered_map<Uint64, Uint32> Result = {};
    Result.reserve(Textures.size());
    for (const auto& Texture : Textures)
        if (Texture)
            if (const auto Index = Device.FindBindlessTextureIndex(Texture))
                Result.emplace(Texture->GetId(), *Index);
    return Result;
}

[[nodiscard]] auto BuildLightGpuData(std::span<const LightRecord> Lights) -> std::vector<LightRecord::GpuData> {
    std::vector<LightRecord::GpuData> Result = {};
    Result.reserve(std::max<std::size_t>(Lights.size(), 1));
    for (const auto& Light : Lights)
        Result.emplace_back(Light.BuildGpuData());
    if (Result.empty())
        Result.emplace_back();
    return Result;
}

} // namespace SoulEngine
