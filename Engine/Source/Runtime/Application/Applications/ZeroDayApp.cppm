/// @file   Applications/ZeroDayApp.cppm
/// @brief  Zero-Day benchmark application — self-registers with ApplicationFactory.

module;

// Required while Scene exposes entt::registry in its object layout. ZeroDayApplication's
// Application base can instantiate Scene lifetime operations in this module.
#include <entt/entt.hpp>

export module ZeroDayApp;

import Application;
import Scene;

namespace SoulEngine {

class ZeroDayApplication final : public Application {
  public:
    ZeroDayApplication() = default;
};

/// Auto-register with the application factory.
ApplicationFactory::AutoRegistrar<ZeroDayApplication> RegZeroDayApp{"ZeroDay"};

} // namespace SoulEngine
