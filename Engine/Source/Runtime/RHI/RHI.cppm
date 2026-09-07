export module RHI;

export import Core;
export import :Types;
export import :Ref;
export import :Pipeline;
export import :RayTracing;
export import :RenderDevice;
export import :Command;
export import :Pass;

// RHI singleton lifecycle is now on RHIRenderDevice:
//   RHIRenderDevice::Create(WindowSys) — select and initialize the backend
//   RHIRenderDevice::Get()            — access
//   RHIRenderDevice::Destroy()        — teardown
//
// RHIBackendFactory moved to RHI:RenderDevice alongside RHIRenderDevice.
