#include <catch2/catch_test_macros.hpp>
#include "crosspad/app/AppRegistry.hpp"
#include <string>

using namespace crosspad;

static const AppEntry testEntry1 = { "CoreTestApp1", "", nullptr, nullptr, nullptr, nullptr, 10 };
static const AppEntry testEntry2 = { "CoreTestApp2", "", nullptr, nullptr, nullptr, nullptr, 20 };

void registerTestApps() {
    static bool registered = false;
    if (registered) return;
    auto& registry = AppRegistry::getInstance();
    registry.registerApp(testEntry1);
    registry.registerApp(testEntry2);
    registered = true;
}

TEST_CASE("AppRegistry: registered apps are found", "[app]") {
    registerTestApps();
    auto& registry = AppRegistry::getInstance();

    REQUIRE(registry.getAppCount() >= 2);

    auto* app1 = registry.findApp("CoreTestApp1");
    REQUIRE(app1 != nullptr);
    REQUIRE(std::string(app1->name) == "CoreTestApp1");
    REQUIRE(app1->priority == 10);

    auto* app2 = registry.findApp("CoreTestApp2");
    REQUIRE(app2 != nullptr);
    REQUIRE(app2->priority == 20);
}

TEST_CASE("AppRegistry: nonexistent app returns nullptr", "[app]") {
    auto& registry = AppRegistry::getInstance();
    REQUIRE(registry.findApp("NoSuchApp") == nullptr);
}

TEST_CASE("AppRegistry: getSelectedApp returns highest priority", "[app]") {
    registerTestApps();
    auto& registry = AppRegistry::getInstance();
    auto* selected = registry.getSelectedApp();
    // Should pick CoreTestApp2 (priority 20) over CoreTestApp1 (priority 10)
    REQUIRE(selected != nullptr);
    REQUIRE(selected->priority >= 20);
}
