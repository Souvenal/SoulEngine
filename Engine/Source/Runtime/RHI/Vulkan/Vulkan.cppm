/// @file   Vulkan/Vulkan.cppm
/// @brief  Standalone Vulkan RHI backend module.
///
/// Self-registers with RHI::BackendFactory when the shared library is loaded.
/// No consumer of RHI ever needs to import this module directly --
/// registration fires automatically via the static initializer.

module;

export module Vulkan;

// A little trick here:
// we use factory for RHI backend, thus no need for exporting symbols.
import :RenderDevice;

import RHI;

namespace SoulEngine::RHI::Vulkan {

/// Auto-register the Vulkan backend with the RHI factory.
/// Safe at global/namespace scope -- RHI::BackendFactory inherits Singleton,
/// so the registry map is guaranteed to exist when this constructor runs.
RHI::BackendFactory::AutoRegistrar<RenderDevice> RegVulkan{"Vulkan"};

} // namespace SoulEngine::RHI::Vulkan
