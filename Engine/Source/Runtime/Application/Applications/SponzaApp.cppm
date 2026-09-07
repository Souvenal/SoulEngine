/// @file   Applications/SponzaApp.cppm
/// @brief  Sponza showcase application — self-registers with ApplicationFactory.

module;

// Required while Scene exposes entt::registry in its object layout. SponzaApplication's
// Application base can instantiate Scene lifetime operations in this module.
#include <entt/entt.hpp>

export module SponzaApp;

import Application;
import Scene;

namespace SoulEngine {

class SponzaApplication final : public Application {
  public:
    SponzaApplication() = default;
};

/// Auto-register with the application factory.
ApplicationFactory::AutoRegistrar<SponzaApplication> RegSponzaApp{"Sponza"};

} // namespace SoulEngine
