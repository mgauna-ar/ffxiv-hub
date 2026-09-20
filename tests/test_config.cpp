#include "test_framework.hpp"
#include "common/config/json.hpp"
#include "common/config/config_manager.hpp"
#include "app/ui/config_binding.hpp"
#include <filesystem>

using namespace hub::config;

TEST_CASE(Config, JsonValueTypes) {
    JsonValue null_val;
    TEST_ASSERT_TRUE(null_val.is_null());

    JsonValue bool_val(true);
    TEST_ASSERT_TRUE(bool_val.is_bool());
    TEST_ASSERT_TRUE(bool_val.as_bool());

    JsonValue num_val(42.5);
    TEST_ASSERT_TRUE(num_val.is_number());
    TEST_ASSERT_NEAR(num_val.as_float(), 42.5f, 0.01f);

    JsonValue str_val("Dawntrail");
    TEST_ASSERT_TRUE(str_val.is_string());
    TEST_ASSERT_EQ(str_val.as_string(), "Dawntrail");
}

TEST_CASE(Config, JsonParseAndStringify) {
    const std::string json_str = R"({
  "name": "FFXIV Hub",
  "version": 1,
  "enabled": true,
  "threshold": 25.5,
  "tags": [
    "combat",
    "mitigation"
  ]
})";

    auto parsed = JsonValue::parse(json_str);
    TEST_ASSERT(parsed.has_value());
    TEST_ASSERT_TRUE(parsed->is_object());

    TEST_ASSERT_EQ((*parsed)["name"].as_string(), "FFXIV Hub");
    TEST_ASSERT_EQ((*parsed)["version"].as_int(), 1);
    TEST_ASSERT_TRUE((*parsed)["enabled"].as_bool());
    TEST_ASSERT_NEAR((*parsed)["threshold"].as_float(), 25.5f, 0.01f);

    TEST_ASSERT_TRUE((*parsed)["tags"].is_array());
    TEST_ASSERT_EQ((*parsed)["tags"].as_array().size(), 2u);
    TEST_ASSERT_EQ((*parsed)["tags"].as_array()[0].as_string(), "combat");
    TEST_ASSERT_EQ((*parsed)["tags"].as_array()[1].as_string(), "mitigation");

    // Round-trip stringify and re-parse
    std::string stringified = parsed->stringify(2);
    auto reparsed = JsonValue::parse(stringified);
    TEST_ASSERT(reparsed.has_value());
    TEST_ASSERT_EQ((*reparsed)["name"].as_string(), "FFXIV Hub");
    TEST_ASSERT_EQ((*reparsed)["version"].as_int(), 1);
}

TEST_CASE(Config, ConfigManagerDefaults) {
    auto& mgr = ConfigManager::instance();
    auto& root = mgr.root();

    TEST_ASSERT_TRUE(root.contains("hub"));
    TEST_ASSERT_TRUE(root.contains("combat_meter"));
    TEST_ASSERT_TRUE(root.contains("latency_mitigator"));

    TEST_ASSERT_EQ(root["hub"]["refresh_interval_ms"].as_int(), 500);
    TEST_ASSERT_NEAR(root["combat_meter"]["inactivity_timeout_seconds"].as_float(), 7.0f, 0.1f);
    TEST_ASSERT_NEAR(root["latency_mitigator"]["target_ping_ms"].as_float(), 15.0f, 0.1f);
    TEST_ASSERT_NEAR(root["latency_mitigator"]["min_animation_lock_ms"].as_float(), 25.0f, 0.1f);
}

TEST_CASE(Config, ClampGeometryToScreen) {
    // Normal window inside screen
    hub::Rect r1{100.0f, 100.0f, 400.0f, 300.0f};
    auto c1 = ConfigManager::clamp_geometry_to_screen(r1, 1920.0f, 1080.0f);
    TEST_ASSERT_NEAR(c1.x, 100.0f, 0.01f);
    TEST_ASSERT_NEAR(c1.y, 100.0f, 0.01f);
    TEST_ASSERT_NEAR(c1.width, 400.0f, 0.01f);
    TEST_ASSERT_NEAR(c1.height, 300.0f, 0.01f);

    // Window pushed completely off right edge
    hub::Rect r2{2500.0f, 100.0f, 400.0f, 300.0f};
    auto c2 = ConfigManager::clamp_geometry_to_screen(r2, 1920.0f, 1080.0f);
    TEST_ASSERT_TRUE(c2.x <= 1920.0f - 40.0f);

    // Window pushed completely above screen
    hub::Rect r3{100.0f, -500.0f, 400.0f, 300.0f};
    auto c3 = ConfigManager::clamp_geometry_to_screen(r3, 1920.0f, 1080.0f);
    TEST_ASSERT_NEAR(c3.y, 0.0f, 0.01f);
}

TEST_CASE(Config, BindingHelpersRoundTripAndPersist) {
    // Desktop controls used to be function-local statics seeded with literals, so
    // they reset every restart and showed values unrelated to the loaded config.
    auto& cfg = hub::config::ConfigManager::instance();
    const auto tmp = std::filesystem::temp_directory_path() / "hub_binding_test.json";
    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing(tmp);

    using namespace hub::app::ui;

    // Absent keys fall back rather than inventing a value.
    TEST_ASSERT(cfg_bool("no_such_section", "nope", true));
    TEST_ASSERT_NEAR(cfg_float("hub", "no_such_key", 4.25f), 4.25f, 0.001f);

    cfg_store("latency_mitigator", "target_ping_ms", 22.5f);
    TEST_ASSERT_NEAR(cfg_float("latency_mitigator", "target_ping_ms", 15.0f), 22.5f, 0.001f);

    cfg_store("hub", "minimize_to_tray", false);
    TEST_ASSERT(!cfg_bool("hub", "minimize_to_tray", true));

    cfg_store("combat_meter", "refresh_interval_ms", 250);
    TEST_ASSERT_EQ(cfg_int("combat_meter", "refresh_interval_ms", 500), 250);

    // A store writes through immediately, so a kill from the tray cannot lose it.
    TEST_ASSERT(std::filesystem::exists(tmp));
    TEST_ASSERT(cfg.load());
    TEST_ASSERT_NEAR(cfg_float("latency_mitigator", "target_ping_ms", 15.0f), 22.5f, 0.001f);
    TEST_ASSERT(!cfg_bool("hub", "minimize_to_tray", true));

    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing({});
}
