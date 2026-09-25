// Renders the README screenshots from the app's and the payload's own drawing code.
//
//   hub_screenshots --windows-dir DIR --out DIR
//
// DIR\Fonts holds segoeui.ttf, segoeuib.ttf and seguisb.ttf, as C:\Windows\Fonts
// does; `make screenshots` fills it with Selawik, Microsoft's open stand-in for
// Segoe UI. Nothing here needs Windows or the game: the desktop window and both
// overlays are drawn by their real code into a software rasterizer, over a raid
// night played into the app the way the payload would have sent it.

#include "app_access.hpp"
#include "backend.hpp"
#include "canvas.hpp"
#include "demo.hpp"
#include "desktop_scene.hpp"
#include "overlay_scene.hpp"

#include "app/app_state.hpp"
#include "app/ui/theme.hpp"
#include "common/ipc/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

/// Pixel density of the pictures in the plugin READMEs: a 150% monitor, and the
/// overlays at scale 1.5. The README column is about 880 pixels wide, so a desktop
/// window 1180 wide shows at three quarters of its size, sharp on a 2x screen.
constexpr float kDensity = 1.5f;
/// The hero is two windows side by side and shows at under half its size, so it is
/// drawn at 100%: a 2x screen then shows it close to pixel for pixel.
constexpr float kHeroDensity = 1.0f;
/// Desktop window client areas. The Combat Meter's is a little larger than the
/// window opens at, which clips its tab labels.
constexpr float kCombatWindowW = 1180.0f;
constexpr float kCombatWindowH = 740.0f;
constexpr float kLatencyWindowW = 1280.0f;
constexpr float kLatencyWindowH = 800.0f;
/// ffxiv_dx11.exe's process id, as the tray and the dashboard would show it.
constexpr uint32_t kGamePid = 18244;

int usage() {
    std::fprintf(stderr, "usage: hub_screenshots --windows-dir DIR --out DIR\n");
    return 2;
}

int px(float v, float density = kDensity) { return static_cast<int>(std::lround(v * density)); }

shots::Rect around(const shots::Rect& r, int margin) {
    return shots::Rect{r.x - margin, r.y - margin, r.w + margin * 2, r.h + margin * 2};
}

bool save(const shots::Canvas& canvas, const std::filesystem::path& path) {
    if (!canvas.write_png(path.string())) {
        std::fprintf(stderr, "could not write %s\n", path.string().c_str());
        return false;
    }
    std::printf("wrote %s (%dx%d)\n", path.string().c_str(), canvas.width(), canvas.height());
    return true;
}

shots::Canvas desktop_canvas(const shots::DesktopCapture& capture) {
    shots::Canvas canvas(capture.width, capture.height, shots::from_im_col32(hub::app::ui::colors::Canvas));
    canvas.draw(capture.frame);
    return canvas;
}

} // namespace

int main(int argc, char** argv) {
    std::string windows_dir;
    std::filesystem::path out;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--windows-dir") == 0) {
            windows_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--out") == 0) {
            out = argv[i + 1];
        } else {
            return usage();
        }
    }
    if (windows_dir.empty() || out.empty()) return usage();
    std::filesystem::create_directories(out);
    shots::set_windows_dir(windows_dir);

    // A config of defaults, somewhere the user's own is not.
    const std::filesystem::path appdata = std::filesystem::temp_directory_path() / "hub_screenshots";
    std::filesystem::remove_all(appdata);
    std::filesystem::create_directories(appdata);
    setenv("APPDATA", appdata.string().c_str(), 1);

    hub::app::AppState app;
    app.initialize();
    const shots::HudReading hud = shots::play_raid_night(app);

    hub::ipc::StatusPayload status{};
    status.game_pid = kGamePid;
    std::snprintf(status.status_message, sizeof(status.status_message), "Hooks installed (OK)");
    app.pipe_server().process_raw_packet(
        hub::ipc::serialize_typed_packet(hub::PluginId::Core, hub::MessageType::Status, 1, status));
    shots::access::pose_attached(app, kGamePid, hud.network_ping_ms);

    // Every frame is captured before any is drawn, so the live pull reads the same
    // elapsed time in each picture.
    hub::meter::EncounterEngine& engine = shots::access::engine(app);

    // The overlays where a player might keep them, on a game frame at 1180x640.
    shots::OverlayLayout layout;
    layout.display_w = px(1180.0f);
    layout.display_h = px(640.0f);
    layout.scale = kDensity;
    layout.meter = shots::Rect{px(36.0f), px(150.0f), px(560.0f), px(244.0f)};
    layout.hud_x = static_cast<float>(px(36.0f));
    layout.hud_y = static_cast<float>(px(92.0f));
    const shots::OverlayCapture overlays =
        shots::capture_overlays(engine, hud.network_ping_ms, hud.smoothed_rtt_ms, layout);
    const shots::DesktopCapture combat = shots::capture_desktop(app, hub::app::DesktopView::CombatMeter,
                                                                kCombatWindowW, kCombatWindowH, kDensity);
    const shots::DesktopCapture latency = shots::capture_desktop(app, hub::app::DesktopView::LatencyMitigator,
                                                                 kLatencyWindowW, kLatencyWindowH, kDensity);

    // The hero: the overlays on the left of the game's frame, the Combat Meter's
    // window floating on the right of it.
    // At scale 1 the meter's cell padding does not shrink with its columns, so it
    // needs a little more room than at 1.5 to keep the names whole.
    const float hero_margin = 40.0f;
    const float hero_meter_w = 620.0f;
    const float hero_meter_h = 272.0f;
    const float hero_w = hero_margin + hero_meter_w + 48.0f + kCombatWindowW + hero_margin;
    const float hero_h = kCombatWindowH + hero_margin * 2.0f;
    shots::OverlayLayout hero_layout;
    hero_layout.display_w = px(hero_w, kHeroDensity);
    hero_layout.display_h = px(hero_h, kHeroDensity);
    hero_layout.scale = kHeroDensity;
    hero_layout.hud_x = static_cast<float>(px(hero_margin, kHeroDensity));
    hero_layout.hud_y = static_cast<float>(px(240.0f, kHeroDensity));
    hero_layout.meter = shots::Rect{px(hero_margin, kHeroDensity), px(298.0f, kHeroDensity),
                                    px(hero_meter_w, kHeroDensity), px(hero_meter_h, kHeroDensity)};
    const shots::OverlayCapture hero_overlays =
        shots::capture_overlays(engine, hud.network_ping_ms, hud.smoothed_rtt_ms, hero_layout);
    const shots::DesktopCapture hero_combat = shots::capture_desktop(
        app, hub::app::DesktopView::CombatMeter, kCombatWindowW, kCombatWindowH, kHeroDensity);

    // The game's frame: a stand-in backdrop with both overlays drawn over it.
    shots::Canvas game = shots::make_backdrop(layout.display_w, layout.display_h);
    game.draw(overlays.frame);

    bool ok = true;
    ok &= save(game.crop(around(overlays.meter, px(24.0f))), out / "combat-overlay.png");
    ok &= save(game.crop(around(overlays.hud, px(24.0f))), out / "latency-hud.png");
    ok &= save(desktop_canvas(combat), out / "combat-damage-tab.png");
    ok &= save(desktop_canvas(latency), out / "latency-view.png");

    shots::Canvas hero = shots::make_backdrop(hero_layout.display_w, hero_layout.display_h);
    hero.draw(hero_overlays.frame);
    const shots::Rect window{hero.width() - hero_combat.width - px(hero_margin, kHeroDensity),
                             px(hero_margin, kHeroDensity), hero_combat.width, hero_combat.height};
    const auto corner = static_cast<float>(px(8.0f, kHeroDensity));
    hero.drop_shadow(window, corner, static_cast<float>(px(18.0f, kHeroDensity)), 0.55f, px(10.0f, kHeroDensity));
    hero.paste_rounded(desktop_canvas(hero_combat), window.x, window.y, corner);
    hero.outline(window, corner, shots::from_im_col32(hub::app::ui::colors::BorderStrong));
    ok &= save(hero, out / "hero.png");

    app.shutdown();
    std::filesystem::remove_all(appdata);
    return ok ? 0 : 1;
}
