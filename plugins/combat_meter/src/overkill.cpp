#include "meter/overkill.hpp"
#include <algorithm>

namespace hub::meter {

uint32_t OverkillLedger::book(EntityId target, uint32_t damage, uint32_t hp, uint64_t now_us) {
    const auto expired = [now_us](const Pending& pending) {
        return now_us > pending.booked_us && now_us - pending.booked_us > kPendingTtlUs;
    };
    if (m_targets.size() > kPruneAbove) {
        std::erase_if(m_targets, [&](const auto& entry) { return expired(entry.second); });
    }

    Pending& pending = m_targets[target];
    if (expired(pending)) {
        pending.amount = 0;
    } else if (hp < pending.hp_seen) {
        pending.amount -= std::min<uint64_t>(pending.amount, pending.hp_seen - hp);
    }

    const uint64_t left = hp > pending.amount ? hp - pending.amount : 0;
    const uint32_t overkill = damage > left ? static_cast<uint32_t>(damage - left) : 0;
    pending.amount += damage - overkill;
    pending.hp_seen = hp;
    pending.booked_us = now_us;
    return overkill;
}

} // namespace hub::meter
