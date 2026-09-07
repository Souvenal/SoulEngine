/// @file   Applications/TestApp.cppm
/// @brief  Demo/test application — self-registers with ApplicationFactory.

module;

// Required while Scene exposes entt::registry in its object layout. TestApplication's
// Application base can instantiate Scene lifetime operations in this module.
#include <entt/entt.hpp>

export module TestApp;

import Application;
import Scene;

namespace SoulEngine {

class TestApplication final : public Application {
  public:
    TestApplication() = default;
};

/// Auto-register with the application factory.
ApplicationFactory::AutoRegistrar<TestApplication> RegTestApp{"Test"};

} // namespace SoulEngine
