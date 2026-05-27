/// @file   VKTexture.cppm
/// @brief  DEPRECATED — TransitionImageLayout has been superseded by
///         VKCommandList::TransitionImage with automatic state tracking.
///
/// This file is retained as a module partition stub to avoid breaking
/// `export module Vulkan;` which still re-exports `:Texture`.  It will
/// be removed entirely when the Vulkan::Texture wrapper is implemented.

module;

export module Vulkan:Texture;

import RHI;

import vulkan;
export import std;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// TransitionImageLayout removed — use VKCommandList::TransitionImage() instead.

} // namespace SoulEngine::RHI::Vulkan
