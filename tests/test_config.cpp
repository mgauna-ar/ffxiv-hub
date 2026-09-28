#include "test_framework.hpp"
#include "common/config/json.hpp"
#include "common/config/config_manager.hpp"
#include "app/ui/config_binding.hpp"
#include "common/ui/overlay_config.hpp"
#include "meter/combat_plugin.hpp"
#include "mitigator/latency_plugin.hpp"
#include "hub/plugin_registry.hpp"
#include "app/app_state.hpp"
#include "common/config/key_table.hpp"
#include "common/os/logger.hpp"
#include "common/os/paths.hpp"
#include "meter/combat_settings.hpp"
#include "mitigator/latency_settings.hpp"
#include "payload/command_dispatcher.hpp"
#include <cfloat>
#include <climits>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>

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
    ConfigManager mgr;
    mgr.set_defaults(hub::app::AppState::default_config());
    const auto root = mgr.document();

    TEST_ASSERT_TRUE(root.contains("hub"));
    TEST_ASSERT_TRUE(root.contains("combat_meter"));
    TEST_ASSERT_TRUE(root.contains("latency_mitigator"));

    // hub.refresh_interval_ms was never read by anything, and is no default.
    TEST_ASSERT_FALSE(root["hub"].contains("refresh_interval_ms"));
    TEST_ASSERT_NEAR(root["combat_meter"]["overlay_hide_after_combat_seconds"].as_float(), 5.0f, 0.1f);
    TEST_ASSERT_NEAR(root["latency_mitigator"]["overlay_hide_after_combat_seconds"].as_float(), 5.0f, 0.1f);
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
    TEST_ASSERT(cfg_get("no_such_section", "nope", true));
    TEST_ASSERT_NEAR(cfg_get("hub", "no_such_key", 4.25f), 4.25f, 0.001f);

    cfg_store("latency_mitigator", "target_ping_ms", 22.5f);
    TEST_ASSERT_NEAR(cfg_get("latency_mitigator", "target_ping_ms", 15.0f), 22.5f, 0.001f);

    cfg_store("hub", "minimize_to_tray", false);
    TEST_ASSERT(!cfg_get("hub", "minimize_to_tray", true));

    cfg_store("combat_meter", "refresh_interval_ms", 250);
    TEST_ASSERT_EQ(cfg_get("combat_meter", "refresh_interval_ms", 500), 250);

    // A store writes through immediately, so a kill from the tray cannot lose it.
    TEST_ASSERT(std::filesystem::exists(tmp));
    TEST_ASSERT(cfg.load());
    TEST_ASSERT_NEAR(cfg_get("latency_mitigator", "target_ping_ms", 15.0f), 22.5f, 0.001f);
    TEST_ASSERT(!cfg_get("hub", "minimize_to_tray", true));

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

    const float x_before = cfg.get("combat_meter", "overlay_x", -1.0f);
    const float y_before = cfg.get("combat_meter", "overlay_y", -1.0f);
    cfg.set("combat_meter", "overlay_x", JsonValue(-1.0));
    cfg.set("combat_meter", "overlay_y", JsonValue(-1.0));

    // What the 1 Hz push from the payload delivers after an in-game drag.
    JsonValue geometry{JsonValue::ObjectType{}};
    hub::ui::store_overlay_geometry(geometry, 1200.0f, 340.0f, 800.0f, 480.0f);
    cfg.merge_section("combat_meter", geometry);

    TEST_ASSERT(cfg.save());

    // Wipe in memory, then read back what actually landed on disk.
    cfg.set("combat_meter", "overlay_x", JsonValue(-1.0));
    TEST_ASSERT(cfg.load());
    TEST_ASSERT_NEAR(cfg.get("combat_meter", "overlay_x", 0.0f), 1200.0f, 0.01f);
    TEST_ASSERT_NEAR(cfg.get("combat_meter", "overlay_y", 0.0f), 340.0f, 0.01f);
    TEST_ASSERT_NEAR(cfg.get("combat_meter", "overlay_width", 0.0f), 800.0f, 0.01f);
    TEST_ASSERT_NEAR(cfg.get("combat_meter", "overlay_height", 0.0f), 480.0f, 0.01f);
    cfg.set("combat_meter", "overlay_x", JsonValue(x_before));
    cfg.set("combat_meter", "overlay_y", JsonValue(y_before));

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

    cfg.set("hub", "marker", JsonValue("first"));
    TEST_ASSERT(cfg.save());
    cfg.set("hub", "marker", JsonValue("second"));
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
    cfg.set_section("hub", cfg.defaults()["hub"]);
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
    app.set_defaults(hub::app::AppState::default_config());
    payload.set_defaults(hub::payload::plugin_config_defaults());
    app.set_custom_path_for_testing(tmp);
    payload.set_custom_path_for_testing(tmp);

    TEST_ASSERT(app.save());
    payload.set_owned_sections(hub::plugins::config_sections());
    TEST_ASSERT(payload.load());

    // The app writes behind the payload's back.
    app.set("hub", "minimize_to_tray", JsonValue(false));
    app.set("hub", "test_marker", JsonValue("kept"));
    app.set("combat_meter", "app_only_key", JsonValue(true));
    TEST_ASSERT(app.save());

    // The payload autosaves its live overlay state from a stale snapshot.
    payload.set("combat_meter", "overlay_x", JsonValue(1200.0));
    TEST_ASSERT(payload.save());

    ConfigManager reader;
    reader.set_custom_path_for_testing(tmp);
    TEST_ASSERT(reader.load());
    const auto root = reader.document();
    TEST_ASSERT_FALSE(root["hub"]["minimize_to_tray"].as_bool(true));
    TEST_ASSERT_EQ(root["hub"]["test_marker"].as_string(), "kept");
    TEST_ASSERT_TRUE(root["combat_meter"]["app_only_key"].as_bool(false));
    TEST_ASSERT_NEAR(root["combat_meter"]["overlay_x"].as_float(), 1200.0f, 0.01f);
    TEST_ASSERT_FALSE(std::filesystem::exists(sidecar));

    std::filesystem::remove(tmp);
}

TEST_CASE(Config, PayloadAutosaveKeepsTheDesktopRate) {
    // A key only the app sets, already on file when the payload loaded it, used to
    // be written back by the payload's autosave within 5 s of the app changing it.
    const auto tmp = std::filesystem::temp_directory_path() / "hub_desktop_rate_test.json";
    auto sidecar = tmp;
    sidecar += ".tmp";
    std::filesystem::remove(tmp);
    std::filesystem::remove(sidecar);

    ConfigManager app;
    ConfigManager payload;
    app.set_defaults(hub::app::AppState::default_config());
    payload.set_defaults(hub::payload::plugin_config_defaults());
    app.set_custom_path_for_testing(tmp);
    payload.set_custom_path_for_testing(tmp);

    app.set("combat_meter", "desktop_dps_metric", JsonValue(1));
    TEST_ASSERT(app.save());
    payload.set_owned_sections(hub::plugins::config_sections());
    TEST_ASSERT(payload.load());

    app.set("combat_meter", "desktop_dps_metric", JsonValue(3));
    TEST_ASSERT(app.save());

    // What the payload's autosave does (hub::payload::save_plugin_config).
    hub::meter::CombatPlugin combat;
    combat.initialize();
    combat.deserialize_config(payload.section("combat_meter"));
    JsonValue section;
    combat.serialize_config(section);
    payload.set_section("combat_meter", section);
    TEST_ASSERT(payload.save());

    ConfigManager reader;
    reader.set_custom_path_for_testing(tmp);
    TEST_ASSERT(reader.load());
    TEST_ASSERT_EQ(reader.get("combat_meter", "desktop_dps_metric", 0), 3);
    TEST_ASSERT(reader.value("combat_meter", "plugin_enabled").has_value());

    std::filesystem::remove(tmp);
}

TEST_CASE(Config, SaveIfChangedWritesOnlyAChange) {
    // The payload's autosave used to re-read, merge and rename config.json every
    // 5 s whether or not anything had changed.
    const auto dir = std::filesystem::temp_directory_path() / "hub_save_if_changed_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "config.json";
    const auto blocker = dir / "blocker";
    { std::ofstream touch(blocker); touch << "not a directory"; }

    ConfigManager cfg;
    cfg.set_custom_path_for_testing(path);
    JsonValue section{JsonValue::ObjectType{}};
    section["overlay_opacity"] = JsonValue(0.8);
    cfg.set_section("combat_meter", section);
    TEST_ASSERT_TRUE(cfg.save_if_changed());
    TEST_ASSERT_TRUE(std::filesystem::exists(path));

    // The same section again, and a set to the value it holds: nothing to write.
    std::filesystem::remove(path);
    cfg.set_section("combat_meter", section);
    cfg.set("combat_meter", "overlay_opacity", JsonValue(0.8));
    TEST_ASSERT_TRUE(cfg.save_if_changed());
    TEST_ASSERT_FALSE(std::filesystem::exists(path));

    section["overlay_opacity"] = JsonValue(0.5);
    cfg.set_section("combat_meter", section);
    TEST_ASSERT_TRUE(cfg.save_if_changed());
    TEST_ASSERT_TRUE(std::filesystem::exists(path));

    // A failed save stays pending until one succeeds.
    cfg.set("combat_meter", "overlay_opacity", JsonValue(0.4));
    cfg.set_custom_path_for_testing(blocker / "config.json");
    TEST_ASSERT_FALSE(cfg.save_if_changed());
    cfg.set_custom_path_for_testing(path);
    TEST_ASSERT_TRUE(cfg.save_if_changed());
    ConfigManager reader;
    reader.set_custom_path_for_testing(path);
    TEST_ASSERT_TRUE(reader.load());
    TEST_ASSERT_EQ(reader.get("combat_meter", "overlay_opacity", 0.0), 0.4);

    // A load is what is on disk, so it leaves nothing to save.
    std::filesystem::remove(dir / "config.json.tmp");
    TEST_ASSERT_TRUE(cfg.load());
    std::filesystem::remove(path);
    TEST_ASSERT_TRUE(cfg.save_if_changed());
    TEST_ASSERT_FALSE(std::filesystem::exists(path));

    std::filesystem::remove_all(dir);
}

TEST_CASE(Config, JsonEqualityIsDeep) {
    JsonValue a{JsonValue::ObjectType{}};
    a["list"] = JsonValue(JsonValue::ArrayType{JsonValue(1), JsonValue("x")});
    a["nested"] = JsonValue(JsonValue::ObjectType{});
    a["nested"]["flag"] = JsonValue(true);
    JsonValue b = a;
    TEST_ASSERT_TRUE(a == b);
    b["nested"]["flag"] = JsonValue(false);
    TEST_ASSERT_FALSE(a == b);
    TEST_ASSERT_FALSE(JsonValue(1) == JsonValue("1"));
    TEST_ASSERT_TRUE(JsonValue() == JsonValue());
}

TEST_CASE(Config, ResetToDefaultsDiscardsEveryChange) {
    // "Reset everything" used to delete the file and call load(), which returns
    // early with no file and left every in-memory value as it was.
    ConfigManager cfg;
    cfg.set_defaults(hub::app::AppState::default_config());
    cfg.set("hub", "minimize_to_tray", JsonValue(false));
    cfg.set("combat_meter", "show_col_crit", JsonValue(false));
    cfg.set("latency_mitigator", "target_ping_ms", JsonValue(40.0));
    cfg.set_section("stray_section", JsonValue(JsonValue::ObjectType{}));

    cfg.reset_to_defaults();
    TEST_ASSERT_EQ(cfg.document().stringify(2), hub::app::AppState::default_config().stringify(2));
}

namespace {

// A plugin keeps its current value for any key absent from the file, so a key
// missing from the defaults survives a reset in-game. Every key a fresh plugin
// serializes but its master switch must be in the app's defaults and the
// payload's, at the value the plugin starts with.
void assert_defaults_cover(const JsonValue& serialized, const char* section) {
    const auto app_defaults = hub::app::AppState::default_config()[section];
    const auto payload_defaults = hub::payload::plugin_config_defaults()[section];
    for (const auto& [key, value] : serialized.as_object()) {
        if (key == hub::plugins::MASTER_SWITCH_KEY) continue;
        for (const JsonValue* defaults : {&app_defaults, &payload_defaults}) {
            if (!defaults->contains(key)) {
                TEST_ASSERT_EQ(std::string(section) + "." + key, std::string("listed in the defaults"));
                continue;
            }
            TEST_ASSERT_EQ(std::string(section) + "." + key + "=" + (*defaults)[key].stringify(0),
                           std::string(section) + "." + key + "=" + value.stringify(0));
        }
    }
    // The payload's defaults hold nothing the plugin does not write.
    for (const auto& [key, value] : payload_defaults.as_object()) {
        (void)value;
        TEST_ASSERT(serialized.contains(key));
    }
    // A default master switch would shadow the combat meter's legacy "enabled".
    TEST_ASSERT_FALSE(app_defaults.contains(hub::plugins::MASTER_SWITCH_KEY));
    TEST_ASSERT_FALSE(payload_defaults.contains(hub::plugins::MASTER_SWITCH_KEY));
}

} // namespace

TEST_CASE(Config, DefaultsCoverEveryCombatMeterKey) {
    hub::meter::CombatPlugin plugin;
    plugin.initialize();
    JsonValue out{JsonValue::ObjectType{}};
    plugin.serialize_config(out);
    assert_defaults_cover(out, "combat_meter");
    // Nor the legacy key, which the file alone may carry.
    TEST_ASSERT_FALSE(hub::app::AppState::default_config()["combat_meter"].contains(hub::meter::LEGACY_ENABLED_KEY));
}

TEST_CASE(Config, DefaultsCoverEveryLatencyMitigatorKey) {
    hub::mitigator::LatencyPlugin plugin;
    plugin.initialize();
    JsonValue out{JsonValue::ObjectType{}};
    plugin.serialize_config(out);
    assert_defaults_cover(out, "latency_mitigator");
}

TEST_CASE(Config, DefaultsKeepTheAppOnlyMeterKeys) {
    // The payload never writes these, so only the app's defaults carry them.
    const auto app = hub::app::AppState::default_config()["combat_meter"];
    TEST_ASSERT_EQ(app[hub::meter::DESKTOP_DPS_METRIC_KEY].as_int(-1), 0);
    TEST_ASSERT_EQ(app[hub::meter::PULL_HISTORY_LIMIT_KEY].as_int(-1), 100);
    const auto payload = hub::payload::plugin_config_defaults()["combat_meter"];
    TEST_ASSERT_FALSE(payload.contains(hub::meter::DESKTOP_DPS_METRIC_KEY));
    TEST_ASSERT_FALSE(payload.contains(hub::meter::PULL_HISTORY_LIMIT_KEY));
}

TEST_CASE(Config, EveryDefaultReachesThePlugin) {
    // The table reads what it writes: a plugin loaded from altered defaults
    // serializes them back, key for key.
    JsonValue meter = hub::meter::default_settings();
    meter["party_only"] = JsonValue(true);
    meter["refresh_interval_ms"] = JsonValue(250);
    meter["dps_metric"] = JsonValue(2);
    meter["overlay_opacity"] = JsonValue(0.5);
    hub::meter::CombatPlugin combat;
    combat.initialize();
    combat.deserialize_config(meter);
    JsonValue out;
    combat.serialize_config(out);
    for (const auto& [key, value] : meter.as_object()) {
        TEST_ASSERT_EQ(key + "=" + out[key].stringify(0), key + "=" + value.stringify(0));
    }

    JsonValue latency = hub::mitigator::default_settings();
    latency["target_ping_ms"] = JsonValue(22.5);
    latency["rtt_sample_window"] = JsonValue(20);
    latency["overlay_mode"] = JsonValue(2);
    hub::mitigator::LatencyPlugin mitigator;
    mitigator.initialize();
    mitigator.deserialize_config(latency);
    mitigator.serialize_config(out);
    for (const auto& [key, value] : latency.as_object()) {
        TEST_ASSERT_EQ(key + "=" + out[key].stringify(0), key + "=" + value.stringify(0));
    }
}

TEST_CASE(Config, KeyTableClampsAndIgnoresWrongTypes) {
    struct Settings {
        uint32_t bits{7};
        int count{3};
        float ratio{0.5f};
        bool flag{true};
    };
    constexpr hub::config::ConfigKey<Settings> keys[] = {
        hub::config::field<Settings, &Settings::bits>("bits"),
        hub::config::field<Settings, &Settings::count>("count"),
        hub::config::field<Settings, &Settings::ratio>("ratio"),
        hub::config::field<Settings, &Settings::flag>("flag"),
    };

    JsonValue section{JsonValue::ObjectType{}};
    section["bits"] = JsonValue(-5);
    section["count"] = JsonValue(1e20);
    section["ratio"] = JsonValue(1e300);
    section["flag"] = JsonValue("yes");
    Settings s;
    hub::config::read_keys<Settings>(keys, section, s);
    TEST_ASSERT_EQ(s.bits, 0u);
    TEST_ASSERT_EQ(s.count, INT_MAX);
    TEST_ASSERT_EQ(s.ratio, FLT_MAX);
    TEST_ASSERT_TRUE(s.flag);

    // An absent key keeps its value; a written one round-trips.
    Settings t;
    hub::config::read_keys<Settings>(keys, JsonValue(JsonValue::ObjectType{}), t);
    TEST_ASSERT_EQ(t.bits, 7u);
    JsonValue written{JsonValue::ObjectType{}};
    hub::config::write_keys<Settings>(keys, Settings{42, -2, 0.25f, false}, written);
    hub::config::read_keys<Settings>(keys, written, t);
    TEST_ASSERT_EQ(t.bits, 42u);
    TEST_ASSERT_EQ(t.count, -2);
    TEST_ASSERT_NEAR(t.ratio, 0.25f, 0.0001f);
    TEST_ASSERT_FALSE(t.flag);
}

TEST_CASE(Config, ReadsNeverInsert) {
    // A read through the old mutable root() created every section and key it
    // looked up, so code that only read still changed what the next save wrote.
    ConfigManager cfg;
    cfg.set("hub", "minimize_to_tray", JsonValue(true));
    const std::string before = cfg.document().stringify(2);

    TEST_ASSERT_FALSE(cfg.value("no_such_section", "key").has_value());
    TEST_ASSERT_FALSE(cfg.value("hub", "no_such_key").has_value());
    TEST_ASSERT_EQ(cfg.get("hub", "no_such_key", 7), 7);
    TEST_ASSERT_TRUE(cfg.section("no_such_section").is_object());
    TEST_ASSERT_TRUE(cfg.section("no_such_section").as_object().empty());
    TEST_ASSERT_EQ(cfg.document().stringify(2), before);
}

TEST_CASE(Config, SetDefaultsKeepsWhatIsAlreadyInMemory) {
    ConfigManager cfg;
    cfg.set("hub", "minimize_to_tray", JsonValue(false));
    cfg.set_defaults(hub::app::AppState::default_config());
    TEST_ASSERT_FALSE(cfg.get("hub", "minimize_to_tray", true));
    TEST_ASSERT_TRUE(cfg.get("hub", "show_notifications", false));
    cfg.reset_to_defaults();
    TEST_ASSERT_TRUE(cfg.get("hub", "minimize_to_tray", false));
}

TEST_CASE(Config, ReloadRebuildsFromTheDefaultsAndTheFile) {
    // load() used to merge over memory, so a key deleted from the file by hand
    // outlived "Reload from disk" until the next reset.
    ConfigManager cfg;
    const auto tmp = std::filesystem::temp_directory_path() / "hub_reload_test.json";
    std::filesystem::remove(tmp);
    cfg.set_custom_path_for_testing(tmp);
    cfg.set_defaults(hub::app::AppState::default_config());
    cfg.set("hub", "minimize_to_tray", JsonValue(false));
    TEST_ASSERT(cfg.save());

    {
        std::ofstream edited(tmp, std::ios::trunc);
        edited << R"({ "hub": { "show_notifications": false } })";
    }
    TEST_ASSERT(cfg.load());
    TEST_ASSERT_TRUE(cfg.get("hub", "minimize_to_tray", false));
    TEST_ASSERT_FALSE(cfg.get("hub", "show_notifications", true));

    // No file leaves the document as it was.
    std::filesystem::remove(tmp);
    TEST_ASSERT_FALSE(cfg.load());
    TEST_ASSERT_FALSE(cfg.get("hub", "show_notifications", true));
}

TEST_CASE(Config, JsonEscapesKeysAndControlCharacters) {
    JsonValue doc{JsonValue::ObjectType{}};
    doc["a \"quoted\" key"] = JsonValue(std::string("bell\x07" "and\x1f" "unit"));
    const std::string text = doc.stringify(2);
    // Nothing below 0x20 goes out raw, or the file would not parse again.
    for (const char c : text) {
        TEST_ASSERT(static_cast<unsigned char>(c) >= 0x20 || c == '\n');
    }
    TEST_ASSERT(text.find("\\u0007") != std::string::npos);

    const auto back = JsonValue::parse(text);
    TEST_ASSERT(back.has_value());
    TEST_ASSERT(back->contains("a \"quoted\" key"));
    TEST_ASSERT_EQ((*back)["a \"quoted\" key"].as_string(), std::string("bell\x07" "and\x1f" "unit"));
}

TEST_CASE(Config, JsonDecodesUnicodeEscapes) {
    const auto doc = JsonValue::parse(R"({ "s": "café — 😀", "lone": "\ud83d!" })");
    TEST_ASSERT(doc.has_value());
    TEST_ASSERT_EQ((*doc)["s"].as_string(), std::string("caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x98\x80"));
    // An unpaired surrogate is replaced, not dropped with what follows it.
    TEST_ASSERT_EQ((*doc)["lone"].as_string(), std::string("\xef\xbf\xbd!"));
    TEST_ASSERT_FALSE(JsonValue::parse(R"({ "bad": "\u12G4" })").has_value());
}

TEST_CASE(Config, JsonNumbersIgnoreTheGlobalLocale) {
    // The payload runs inside the game process, whose locale it does not choose.
    // A C++ global locale with a comma decimal point and digit grouping:
    struct CommaNumpunct : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
        char do_thousands_sep() const override { return '.'; }
        std::string do_grouping() const override { return "\3"; }
    };
    const std::locale previous = std::locale::global(std::locale(std::locale::classic(), new CommaNumpunct));

    JsonValue doc{JsonValue::ObjectType{}};
    doc["big"] = JsonValue(1234567);
    doc["ratio"] = JsonValue(0.5);
    const std::string text = doc.stringify(0);
    auto parsed = JsonValue::parse(R"({"ratio": 25.5, "big": 1234567})");
    std::locale::global(previous);

    TEST_ASSERT(text.find("1234567") != std::string::npos);
    TEST_ASSERT(text.find("0.5") != std::string::npos);
    TEST_ASSERT(parsed.has_value());
    TEST_ASSERT_NEAR((*parsed)["ratio"].as_double(), 25.5, 1e-9);
    TEST_ASSERT_EQ((*parsed)["big"].as_int(), 1234567);

    // And the C locale, which std::stod followed, where the host has one to set.
    const std::string c_before = std::setlocale(LC_NUMERIC, nullptr);
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252"}) {
        if (std::setlocale(LC_NUMERIC, name) == nullptr) continue;
        auto in_c_locale = JsonValue::parse(R"({"ratio": 25.5})");
        const std::string printed = JsonValue(0.25).stringify(0);
        std::setlocale(LC_NUMERIC, c_before.c_str());
        TEST_ASSERT(in_c_locale.has_value());
        TEST_ASSERT_NEAR((*in_c_locale)["ratio"].as_double(), 25.5, 1e-9);
        TEST_ASSERT(printed.find("0.25") != std::string::npos);
        break;
    }
    std::setlocale(LC_NUMERIC, c_before.c_str());
}

TEST_CASE(Config, JsonNumberConversionsClamp) {
    // A hand-edited config can hold any number; converting one outside int's or
    // float's range is undefined behaviour.
    TEST_ASSERT_EQ(JsonValue(1e20).as_int(), INT_MAX);
    TEST_ASSERT_EQ(JsonValue(-1e20).as_int(), INT_MIN);
    TEST_ASSERT_EQ(JsonValue(-7.9).as_int(), -7);
    TEST_ASSERT_EQ(JsonValue(std::nan("")).as_int(42), 42);
    TEST_ASSERT_EQ(JsonValue(1e300).as_float(), FLT_MAX);
    TEST_ASSERT_EQ(JsonValue(-1e300).as_float(), -FLT_MAX);
    TEST_ASSERT_NEAR(JsonValue(std::nan("")).as_float(1.5f), 1.5f, 0.0f);

    auto parsed = JsonValue::parse(R"({"huge": 1e20, "neg": -3000000000})");
    TEST_ASSERT(parsed.has_value());
    TEST_ASSERT_EQ((*parsed)["huge"].as_int(), INT_MAX);
    TEST_ASSERT_EQ((*parsed)["neg"].as_int(), INT_MIN);

    // Beyond double, as std::stod rejected it.
    TEST_ASSERT_FALSE(JsonValue::parse(R"({"x": 1e999})").has_value());
    TEST_ASSERT_FALSE(JsonValue::parse(R"({"x": -})").has_value());
}

TEST_CASE(Config, NonFiniteNumbersStringifyAsNull) {
    // "nan" would make the whole file unreadable on the next load.
    JsonValue doc{JsonValue::ObjectType{}};
    doc["x"] = JsonValue(std::nan(""));
    auto parsed = JsonValue::parse(doc.stringify(2));
    TEST_ASSERT(parsed.has_value());
    TEST_ASSERT_TRUE((*parsed)["x"].is_null());
}

TEST_CASE(Config, SaveFailureIsLoggedOncePerSpell) {
    const auto dir = std::filesystem::temp_directory_path() / "hub_save_log_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto log = dir / "test.log";
    const auto blocker = dir / "blocker";
    { std::ofstream touch(blocker); touch << "not a directory"; }

    TEST_ASSERT_TRUE(hub::os::Logger::init(log, false));
    ConfigManager cfg;
    cfg.set_custom_path_for_testing(blocker / "config.json");
    TEST_ASSERT_FALSE(cfg.save());
    TEST_ASSERT_FALSE(cfg.save());
    cfg.set_custom_path_for_testing(dir / "config.json");
    TEST_ASSERT_TRUE(cfg.save());
    hub::os::Logger::shutdown();

    std::string contents;
    {
        std::ifstream in(log);
        std::stringstream buf;
        buf << in.rdbuf();
        contents = buf.str();
    }
    size_t failures = 0;
    for (size_t at = contents.find("Could not save"); at != std::string::npos;
         at = contents.find("Could not save", at + 1)) {
        ++failures;
    }
    TEST_ASSERT_EQ(failures, 1u);
    TEST_ASSERT(contents.find("again") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE(Config, PathsOutsideAsciiSurvive) {
    // Widening a path byte by byte corrupted any user folder outside ASCII.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / std::filesystem::path(u8"hub_usu\u00e1rio_\u65e5\u672c");
    std::filesystem::remove_all(dir);
    TEST_ASSERT_EQ(hub::os::to_utf8(dir.filename()), std::string("hub_usu\xc3\xa1rio_\xe6\x97\xa5\xe6\x9c\xac"));

    ConfigManager cfg;
    cfg.set_custom_path_for_testing(dir / "config.json");
    cfg.set("hub", "marker", JsonValue("kept"));
    TEST_ASSERT_TRUE(cfg.save());
    ConfigManager reader;
    reader.set_custom_path_for_testing(dir / "config.json");
    TEST_ASSERT_TRUE(reader.load());
    TEST_ASSERT_EQ(reader.get("hub", "marker", std::string()), std::string("kept"));

    TEST_ASSERT_TRUE(hub::os::Logger::init(dir / "hub.log", false));
    hub::os::Logger::shutdown();
    TEST_ASSERT(std::filesystem::exists(dir / "hub.log"));
    TEST_ASSERT_EQ(hub::os::Logger::previous_log_path(dir / "hub.log"), dir / "hub.prev.log");

    std::filesystem::remove_all(dir);
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
