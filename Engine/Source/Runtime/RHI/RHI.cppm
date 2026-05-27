export module RHI;

export import Core;
export import :Types;
export import :Reflection;
export import :RenderDevice;
export import :CommandList;

// RHI singleton lifecycle is now on RenderDevice:
//   RenderDevice::Create(Window)   — bootstrap
//   RenderDevice::Get()            — access
//   RenderDevice::Destroy()        — teardown
//
// BackendFactory moved to RHI:RenderDevice alongside RenderDevice.
