// CRASH CASE: the type is imported from another partition of the same module.
// The PCM->OBJ codegen job of this TU dies with SEGV 0xC0000005 while mangling
// entt::meta_factory<BugRecord>::data<&BugRecord::Value>.
module;

#include <entt/entt.hpp>

export module bugrepro:Register;

import :Types;

namespace {

auto Register() -> void {
    entt::meta_factory<BugRecord> Factory = entt::meta_factory<BugRecord>{}.type("bugrecord");
    Factory.data<&BugRecord::Value>("value");
}

} // namespace

namespace MetaRegistration {
struct Registration { Registration() { Register(); } };
Registration g_Registration = {};
} // namespace MetaRegistration
