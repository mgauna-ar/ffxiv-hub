#include "test_framework.hpp"
#include "common/config/json.hpp"
#include "common/config/config_manager.hpp"
#include "app/ui/config_binding.hpp"
#include "common/ui/overlay_config.hpp"
#include "meter/combat_plugin.hpp"
#include "mitigator/latency_plugin.hpp"
#include <filesystem>
#include <fstream>

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

TEST_CASE(Config, AppSaveDoesNotClobberMirroredGeometry) {
    // The desktop app loads config.json once and then rewrites the whole
    // document on every toggle and on exit. Before the payload's geometry was
    // mirrored into the root, those saves wrote back the position the overlay
    // had at app startup, so dragging an overlay in-game never survived.
    auto& cfg = hub::config::ConfigManager::instance();
    const auto tmp = std::filesystem::temp_directory_path() / "hub_geometry_test.json";
    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing(tmp);

    auto& root = cfg.root();
    root["combat_meter"]["overlay_x"] = JsonValue(-1.0);
    root["combat_meter"]["overlay_y"] = JsonValue(-1.0);

    // What the 1 Hz push from the payload delivers after an in-game drag.
    hub::ui::store_overlay_geometry(root["combat_meter"], 1200.0f, 340.0f, 800.0f, 480.0f);

    TEST_ASSERT(cfg.save());

    // Wipe in memory, then read back what actually landed on disk.
    root["combat_meter"]["overlay_x"] = JsonValue(-1.0);
    TEST_ASSERT(cfg.load());
    TEST_ASSERT_NEAR(cfg.root()["combat_meter"]["overlay_x"].as_float(), 1200.0f, 0.01f);
    TEST_ASSERT_NEAR(cfg.root()["combat_meter"]["overlay_y"].as_float(), 340.0f, 0.01f);
    TEST_ASSERT_NEAR(cfg.root()["combat_meter"]["overlay_width"].as_float(), 800.0f, 0.01f);
    TEST_ASSERT_NEAR(cfg.root()["combat_meter"]["overlay_height"].as_float(), 480.0f, 0.01f);

    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing({});
}

TEST_CASE(Config, StoreOverlayGeometryLeavesOtherFieldsAlone) {
    // Geometry is the only field the payload owns. Mirroring the rest of its
    // push back into the config would undo a toggle the user just flipped in
    // the desktop app, since that push lags the control by up to a second.
    JsonValue section{JsonValue::ObjectType{}};
    section["overlay_locked"] = JsonValue(true);
    section["overlay_opacity"] = JsonValue(0.5);
    section["overlay_hide_conditions"] = JsonValue(3.0);

    hub::ui::store_overlay_geometry(section, 10.0f, 20.0f, 30.0f, 40.0f);

    TEST_ASSERT_NEAR(section["overlay_x"].as_float(), 10.0f, 0.01f);
    TEST_ASSERT_TRUE(section["overlay_locked"].as_bool(false));
    TEST_ASSERT_NEAR(section["overlay_opacity"].as_float(), 0.5f, 0.01f);
    TEST_ASSERT_EQ(section["overlay_hide_conditions"].as_int(0), 3);
    TEST_ASSERT_FALSE(section.contains("overlay_visible"));
    TEST_ASSERT_FALSE(section.contains("overlay_scale"));
}

TEST_CASE(Config, SaveIsAtomicOnExistingFile) {
    // The payload and the app both write this document on their own cadence, so
    // a truncating write can be read half-finished by the other process.
    auto& cfg = hub::config::ConfigManager::instance();
    const auto tmp = std::filesystem::temp_directory_path() / "hub_atomic_test.json";
    auto sidecar = tmp;
    sidecar += ".tmp";
    std::filesystem::remove(tmp);
    std::filesystem::remove(sidecar);
    cfg.set_custom_path_for_testing(tmp);

    cfg.root()["hub"]["marker"] = JsonValue("first");
    TEST_ASSERT(cfg.save());
    cfg.root()["hub"]["marker"] = JsonValue("second");
    TEST_ASSERT(cfg.save());

    TEST_ASSERT(std::filesystem::exists(tmp));
    TEST_ASSERT_FALSE(std::filesystem::exists(sidecar));

    // Scoped: Windows refuses to remove a file that still has an open handle, so
    // leaving the stream open until the end of the test fails the cleanup below.
    std::string contents;
    {
        std::ifstream in(tmp);
        std::stringstream buf;
        buf << in.rdbuf();
        contents = buf.str();
    }
    auto parsed = JsonValue::parse(contents);
    TEST_ASSERT(parsed.has_value());
    TEST_ASSERT(parsed->is_object());
    TEST_ASSERT_EQ((*parsed)["hub"]["marker"].as_string(), "second");

    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing({});
}

TEST_CASE(Config, PayloadSaveKeepsAppOnlySections) {
    // The payload loads config.json once and autosaves every 5 s. Writing its
    // whole root put back the hub keys it loaded at injection, reverting any app
    // setting changed since.
    const auto tmp = std::filesystem::temp_directory_path() / "hub_owned_sections_test.json";
    auto sidecar = tmp;
    sidecar += ".tmp";
    std::filesystem::remove(tmp);
    std::filesystem::remove(sidecar);

    ConfigManager app;
    ConfigManager payload;
    app.set_custom_path_for_testing(tmp);
    payload.set_custom_path_for_testing(tmp);

    TEST_ASSERT(app.save());
    payload.set_owned_sections({"combat_meter", "latency_mitigator"});
    TEST_ASSERT(payload.load());

    // The app writes behind the payload's back.
    app.root()["hub"]["minimize_to_tray"] = JsonValue(false);
    app.root()["hub"]["test_marker"] = JsonValue("kept");
    app.root()["combat_meter"]["app_only_key"] = JsonValue(true);
    TEST_ASSERT(app.save());

    // The payload autosaves its live overlay state from a stale snapshot.
    payload.root()["combat_meter"]["overlay_x"] = JsonValue(1200.0);
    TEST_ASSERT(payload.save());

    ConfigManager reader;
    reader.set_custom_path_for_testing(tmp);
    TEST_ASSERT(reader.load());
    auto& root = reader.root();
    TEST_ASSERT_FALSE(root["hub"]["minimize_to_tray"].as_bool(true));
    TEST_ASSERT_EQ(root["hub"]["test_marker"].as_string(), "kept");
    TEST_ASSERT_TRUE(root["combat_meter"]["app_only_key"].as_bool(false));
    TEST_ASSERT_NEAR(root["combat_meter"]["overlay_x"].as_float(), 1200.0f, 0.01f);
    TEST_ASSERT_FALSE(std::filesystem::exists(sidecar));

    std::filesystem::remove(tmp);
}

TEST_CASE(Config, ResetToDefaultsDiscardsEveryChange) {
    // "Reset everything" used to delete the file and call load(), which returns
    // early with no file and left every in-memory value as it was.
    ConfigManager cfg;
    cfg.root()["hub"]["minimize_to_tray"] = JsonValue(false);
    cfg.root()["combat_meter"]["show_col_crit"] = JsonValue(false);
    cfg.root()["latency_mitigator"]["target_ping_ms"] = JsonValue(40.0);
    cfg.root()["stray_section"] = JsonValue(JsonValue::ObjectType{});

    cfg.reset_to_defaults();
    TEST_ASSERT_EQ(cfg.root().stringify(2), ConfigManager::default_document().stringify(2));
}

namespace {

// A plugin keeps its current value for any key absent from the file, so a key
// missing from the defaults survives a reset in-game.
void assert_defaults_cover(const JsonValue& serialized, const char* section) {
    const auto defaults = ConfigManager::default_document();
    for (const auto& [key, value] : serialized.as_object()) {
        (void)value;
        if (key == "plugin_enabled") continue;
        if (!defaults[section].contains(key)) {
            TEST_ASSERT_EQ(std::string(section) + "." + key, std::string("listed in default_document()"));
        }
    }
}

} // namespace

TEST_CASE(Config, DefaultsCoverEveryCombatMeterKey) {
    hub::meter::CombatPlugin plugin;
    plugin.initialize();
    JsonValue out{JsonValue::ObjectType{}};
    plugin.serialize_config(out);
    assert_defaults_cover(out, "combat_meter");
    plugin.shutdown();
}

TEST_CASE(Config, DefaultsCoverEveryLatencyMitigatorKey) {
    hub::mitigator::LatencyPlugin plugin;
    plugin.initialize();
    JsonValue out{JsonValue::ObjectType{}};
    plugin.serialize_config(out);
    assert_defaults_cover(out, "latency_mitigator");
    plugin.shutdown();
}

TEST_CASE(Config, SaveReportsFailureInsteadOfClaimingSuccess) {
    // save() used to return true even when the stream never reached disk.
    auto& cfg = hub::config::ConfigManager::instance();
    const auto blocker = std::filesystem::temp_directory_path() / "hub_blocker_file";
    std::filesystem::remove_all(blocker);
    { std::ofstream touch(blocker); touch << "not a directory"; }

    // Parent of the target is a regular file, so the write cannot succeed.
    cfg.set_custom_path_for_testing(blocker / "config.json");
    TEST_ASSERT_FALSE(cfg.save());

    std::filesystem::remove(blocker);
    cfg.set_custom_path_for_testing({});
}
