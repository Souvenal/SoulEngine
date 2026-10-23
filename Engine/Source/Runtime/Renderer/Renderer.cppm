export module Renderer;

// :Common.GBuffer is exported for the cross-module EntityId picking contract (GBuffer::BackgroundEntityId;
// the editor maps readback values against it). The rest of :Common stays module-internal.
export import Renderer.Common.GBuffer;
export import Renderer.Interface;
export import Renderer.Selection;
// Note: the :Raster and :RayTracing aggregates are module-internal partitions; nothing here or in consumers
// of `import Renderer;` references their declarations, and the xmake moduleonly target links every partition
// object file directly, so the RasterRenderer and RayTracingRenderer AutoRegistrars run without an import
// from this interface.
