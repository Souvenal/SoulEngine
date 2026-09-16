// CONTROL CASE: the registered type is defined in THIS TU. Identical entt
// usage otherwise; codegen passes on every clang version tested.
module;

#include <entt/entt.hpp>

export module bugrepro:SelfRegister;

export struct LocalRecord {
    int Value = 0;
};

namespace {

auto Register() -> void {
    entt::meta_factory<LocalRecord> Factory = entt::meta_factory<LocalRecord>{}.type("localrecord");
    Factory.data<&LocalRecord::Value>("value");
}

} // namespace

namespace MetaRegistration {
struct Registration { Registration() { Register(); } };
Registration g_Registration = {};
} // namespace MetaRegistration
