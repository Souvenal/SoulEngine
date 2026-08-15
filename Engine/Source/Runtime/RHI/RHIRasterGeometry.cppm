/// @file   RHIRasterGeometry.cppm
/// @brief  Backend-agnostic geometry records for GPU-driven raster drawing.

module;

#include <cstddef>

export module RHI:RasterGeometry;

export import :Types;
import :Ref;

export import std;

export namespace SoulEngine {

/// @brief One logical SubMesh source retained by a GPU-driven raster frame.
///
/// The source keeps RHI refs so the buffers remain alive while the shader-visible
/// geometry table and indirect draws are in flight. Vulkan resolves the refs to
/// device addresses during command recording; Renderer never observes native BDA.
struct RHIRasterGeometrySource {
    RHIRef<RHIVertexBuffer> PositionBufferRef = nullptr;
    RHIRef<RHIVertexBuffer> NormalBufferRef   = nullptr;
    RHIRef<RHIVertexBuffer> TangentBufferRef  = nullptr;
    RHIRef<RHIVertexBuffer> TexCoordBufferRef = nullptr;
    RHIRef<RHIIndexBuffer>  IndexBufferRef    = nullptr;
    Uint32                  IndexCount        = 0;
    Uint32                  MaterialID        = 0;
};

/// @brief Shader-visible ABI for one canonical tightly packed SubMesh.
///
/// Each address points to the first typed element in its independent buffer.
/// The current mesh importer uses float3/float4/float2/uint element arrays, so
/// typed Slang pointers provide the element stride and no per-record offsets or
/// strides are required.
struct alignas(8) RHIRasterGeometryData {
    Uint64 PositionAddress = 0;
    Uint64 NormalAddress   = 0;
    Uint64 TangentAddress  = 0;
    Uint64 TexCoordAddress = 0;
    Uint64 IndexAddress    = 0;
    Uint32 IndexCount      = 0;
    Uint32 MaterialID      = 0;
};

static_assert(sizeof(RHIRasterGeometryData) == 48);
static_assert(alignof(RHIRasterGeometryData) == 8);
static_assert(offsetof(RHIRasterGeometryData, PositionAddress) == 0);
static_assert(offsetof(RHIRasterGeometryData, IndexAddress) == 32);
static_assert(offsetof(RHIRasterGeometryData, IndexCount) == 40);
static_assert(offsetof(RHIRasterGeometryData, MaterialID) == 44);

} // namespace SoulEngine
