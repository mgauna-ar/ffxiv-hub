// Built as if on Windows (see win32_mode.hpp): the overlays draw only under _WIN32.

#include "overlay_scene.hpp"
#include "backend.hpp"

#include "meter/combat_overlay.hpp"
#include "mitigator/latency_overlay.hpp"
#include "payload/overlay_host.hpp"

#include "imgui.h"

#include <cmath>
#include <memory>

namespace shots {

namespace {

Rect window_rect(const hub::Rect& r) {
    return Rect{static_cast<int>(std::floor(r.x)), static_cast<int>(std::floor(r.y)),
                static_cast<int>(std::ceil(r.width)), static_cast<int>(std::ceil(r.height))};
}

} // namespace

OverlayCapture capture_overlays(hub::meter::EncounterEngine& engine, double ping_ms, double rtt_ms,
                                const OverlayLayout& layout) {
    auto& host = hub::payload::OverlayHost::instance();
    ImGuiContext* const previous = ImGui::GetCurrentContext();

    // The host owns its ImGui context, style and fonts, as it does in the game. The
    // window and device only have to be non-null for the stand-in backends.
    int window = 0;
    int device = 0;
    int device_context = 0;
    if (!host.initialize(&window, &device, &device_context)) return {};

    auto meter = std::make_shared<hub::meter::CombatOverlay>(&engine);
    meter->apply_config(hub::meter::CombatConfig{}.overlay);
    meter->set_locked(true);
    meter->set_scale(layout.scale);
    meter->set_geometry(hub::Rect{static_cast<float>(layout.meter.x), static_cast<float>(layout.meter.y),
                                  static_cast<float>(layout.meter.w), static_cast<float>(layout.meter.h)});

    auto hud = std::make_shared<hub::mitigator::LatencyOverlay>();
    hud->apply_config(hub::mitigator::default_overlay_config());
    hud->set_locked(true);
    hud->set_scale(layout.scale);
    hud->set_geometry(hub::Rect{layout.hud_x, layout.hud_y, hub::mitigator::constants::DEFAULT_OVERLAY_WIDTH,
                                hub::mitigator::constants::DEFAULT_OVERLAY_HEIGHT});
    hud->set_mitigation_mode(true, false);
    hud->update_network_ping(ping_ms);
    hud->update_rtt(rtt_ms, true);

    host.register_overlay(meter);
    host.register_overlay(hud);

    OverlayCapture capture;
    // A few frames first: tables and auto-sized windows settle over two or three.
    for (int i = 0; i < 4; ++i) {
        set_capture(i == 3 ? &capture.frame : nullptr);
        prepare_frame(static_cast<float>(layout.display_w), static_cast<float>(layout.display_h));
        host.render_frame();
    }
    set_capture(nullptr);

    capture.meter = window_rect(meter->get_geometry());
    capture.hud = window_rect(hud->get_geometry());

    host.unregister_overlay(meter->overlay_id());
    host.unregister_overlay(hud->overlay_id());
    // Now, while its context is current: the host is a static, and at exit its
    // destructor would tear down whichever context happened to be current then.
    host.shutdown();
    ImGui::SetCurrentContext(previous);
    return capture;
}

} // namespace shots
