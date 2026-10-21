export module RHI;

export import RHI.Types;
export import RHI.Ref;
export import RHI.Pipeline;
export import RHI.RenderDevice;
export import RHI.Command;
export import RHI.Pass;

// RHI singleton lifecycle is now on RHIRenderDevice:
//   RHIRenderDevice::Create(WindowSys) — select and initialize the backend
//   RHIRenderDevice::Get()            — access
//   RHIRenderDevice::Destroy()        — teardown
//
// RHIBackendFactory moved to RHI:RenderDevice alongside RHIRenderDevice.
