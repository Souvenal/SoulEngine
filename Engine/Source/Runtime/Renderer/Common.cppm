module;

#include <cstddef>
#include <hlsl++.h>

export module Renderer:Common;

import Core;
import Scene;

export import std;

export namespace SoulEngine {

/// @brief Shared frame constant-buffer ABI used by every renderer.
struct alignas(16) RendererFrameConstants {
    Float32 Time          = 0.0f;
    Float32 ExposureEV100 = 15.0f;
    Uint32  LightCount    = 0;
};
static_assert(sizeof(RendererFrameConstants) == 16);
static_assert(offsetof(RendererFrameConstants, Time) == 0);
static_assert(offsetof(RendererFrameConstants, ExposureEV100) == 4);
static_assert(offsetof(RendererFrameConstants, LightCount) == 8);

/// @brief Shared view constant-buffer ABI used by every renderer.
struct alignas(16) RendererViewConstants {
    /// @brief Build the shared view constants from an immutable camera record.
    explicit RendererViewConstants(const CameraViewRecord& View)
        : ViewProjection(View.ViewProjection),
          ViewProjectionInverse(hlslpp::inverse(View.ViewProjection)),
          CameraPosition(hlslpp::interop::float4{
              hlslpp::float4{View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}}),
          ViewportSize(hlslpp::interop::float2{
              hlslpp::float2{static_cast<Float32>(View.GetWidth()), static_cast<Float32>(View.GetHeight())}}) {}

    alignas(16) hlslpp::float4x4 ViewProjection = hlslpp::float4x4::identity();
    alignas(16) hlslpp::float4x4 ViewProjectionInverse = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
    hlslpp::interop::float2 ViewportSize = hlslpp::interop::float2{hlslpp::float2{0.0f, 0.0f}};
};
static_assert(sizeof(RendererViewConstants) == 160);
static_assert(offsetof(RendererViewConstants, ViewProjection) == 0);
static_assert(offsetof(RendererViewConstants, ViewProjectionInverse) == 64);
static_assert(offsetof(RendererViewConstants, CameraPosition) == 128);
static_assert(offsetof(RendererViewConstants, ViewportSize) == 144);

/// @brief Build the shared transient GPU light table from scene records.
[[nodiscard]] auto BuildLightGpuData(std::span<const LightRecord> Lights)
    -> std::vector<LightRecord::GpuData> {
    std::vector<LightRecord::GpuData> Result = {};
    Result.reserve(std::max<std::size_t>(Lights.size(), 1));
    for (const auto& Light : Lights)
        Result.emplace_back(Light.BuildGpuData());
    if (Result.empty())
        Result.emplace_back();
    return Result;
}

} // namespace SoulEngine
