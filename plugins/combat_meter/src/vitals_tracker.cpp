#include "meter/vitals.hpp"
#include <algorithm>

namespace hub::meter {

std::optional<LifeEventKind> VitalsTracker::observe_life(const ActorVitals& vitals) {
    // 0 of an unknown max proves nothing, same rule as the wipe check.
    if (vitals.max_hp == 0) {
        return std::nullopt;
    }
    const bool alive = vitals.hp > 0;
    Tracked& tracked = m_actors[vitals.entity];
    if (!tracked.life_known) {
        tracked.life_known = true;
        tracked.alive = alive;
        return std::nullopt;
    }
    if (tracked.alive == alive) {
        return std::nullopt;
    }
    tracked.alive = alive;
    return alive ? LifeEventKind::Raise : LifeEventKind::Death;
}

bool VitalsTracker::status_list_changed(const ipc::StatusListPacket& list) {
    Tracked& tracked = m_actors[list.entity_id];
    bool changed = !tracked.list_sent;
    const ipc::StatusListPacket& last = tracked.last_list;
    if (!changed) {
        changed = last.count != list.count || last.flags != list.flags;
    }
    if (!changed) {
        const double elapsed_s = list.timestamp_us > last.timestamp_us
            ? static_cast<double>(list.timestamp_us - last.timestamp_us) / 1e6
            : 0.0;
        const size_t count = std::min<size_t>(list.count, ipc::MAX_STATUS_LIST_ENTRIES);
        for (size_t i = 0; i < count && !changed; ++i) {
            const ipc::CombatStatusEntry& before = last.entries[i];
            const ipc::CombatStatusEntry& now = list.entries[i];
            // Slots are stable, so an unchanged set comes back in the same order.
            changed = before.status_id != now.status_id || before.param != now.param
                || before.source_id != now.source_id;
            // A timer merely counting down is not news; one that went back up was refreshed.
            if (!changed && now.remaining_s > 0.0f) {
                const double predicted = static_cast<double>(before.remaining_s) - elapsed_s;
                changed = static_cast<double>(now.remaining_s) > predicted + kRefreshSlackS;
            }
        }
    }
    if (changed) {
        tracked.last_list = list;
        tracked.list_sent = true;
    }
    return changed;
}

std::vector<EntityId> VitalsTracker::retain(std::span<const EntityId> seen) {
    std::vector<EntityId> cleared;
    for (auto it = m_actors.begin(); it != m_actors.end();) {
        if (std::find(seen.begin(), seen.end(), it->first) != seen.end()) {
            ++it;
            continue;
        }
        if (it->second.list_sent && it->second.last_list.count > 0) {
            cleared.push_back(it->first);
        }
        it = m_actors.erase(it);
    }
    std::sort(cleared.begin(), cleared.end());
    return cleared;
}

void VitalsTracker::invalidate() {
    for (auto& [id, tracked] : m_actors) {
        tracked.list_sent = false;
    }
}

} // namespace hub::meter
