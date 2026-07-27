export module RHI;

export import Core;
export import :Types;
export import :RayTracing;
export import :RenderDevice;
export import :Command;      // RHICommandList, RHIPass, RHICommand variant
export import :UsageVisitor; // RHIUsageVisitor for command resource tracking

// RHI singleton lifecycle is now on RHIRenderDevice:
//   RHIRenderDevice::Create(WindowSys) — select and initialize the backend
//   RHIRenderDevice::Get()            — access
//   RHIRenderDevice::Destroy()        — teardown
//
// RHIBackendFactory moved to RHI:RenderDevice alongside RHIRenderDevice.
