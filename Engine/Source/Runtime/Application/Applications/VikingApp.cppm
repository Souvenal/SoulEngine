/// @file   Applications/VikingApp.cppm
/// @brief  Viking room showcase application — self-registers with ApplicationFactory.

module;

// Required while Scene exposes entt::registry in its object layout. VikingApplication's
// Application base can instantiate Scene lifetime operations in this module.
#include <entt/entt.hpp>

export module VikingApp;

import Application;
import Scene;

namespace SoulEngine {

class VikingApplication final : public Application {
  public:
    VikingApplication() = default;
};

/// Auto-register with the application factory.
ApplicationFactory::AutoRegistrar<VikingApplication> RegVikingApp{"Viking"};

} // namespace SoulEngine
