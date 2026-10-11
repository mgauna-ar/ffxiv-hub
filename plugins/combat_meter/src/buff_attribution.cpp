#include "meter/buff_attribution.hpp"
#include "hub/game_definitions.hpp"
#include "hub/game/actions.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/guaranteed_hits.hpp"
#include "hub/game/raid_buffs.hpp"
#include "hub/game/tick_statuses.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace hub::meter {

namespace {

/// Crit damage is the crit rate plus 1.35: both come from the same stat term.
constexpr double kCritDamageOverRate = 1.35;
constexpr double kDirectHitBonus = 0.25;

double estimate(uint32_t hits, uint32_t landed, double prior) {
    const double rate = (static_cast<double>(landed) + prior * RateEstimator::kPriorWeight)
        / (static_cast<double>(hits) + RateEstimator::kPriorWeight);
    return std::clamp(rate, RateEstimator::kMinRate, RateEstimator::kMaxRate);
}

/// A raid buff present on the hit, with its strength as a fraction.
struct PresentBuff {
    const game::RaidBuff* buff{nullptr};
    EntityId giver{0};
    double value{0.0};
    bool external{false};
};

double strength_of(const game::RaidBuff& buff, EntityId giver, Role receiver, const BuffStrengths& strengths) {
    uint16_t permille = buff.permille;
    switch (buff.value) {
        case game::RaidBuffValue::Flat:
            break;
        case game::RaidBuffValue::MeleeCard:
            if (receiver != Role::None && receiver != Role::Melee && receiver != Role::Tank) permille /= 2;
            break;
        case game::RaidBuffValue::RangedCard:
            if (receiver == Role::Melee || receiver == Role::Tank) permille /= 2;
            break;
        case game::RaidBuffValue::ByApplication:
            permille = strengths.permille(giver, buff.status_id, buff.permille);
            break;
    }
    return static_cast<double>(permille) / 1000.0;
}

bool has_crit(game::RaidBuffKind kind) {
    return kind == game::RaidBuffKind::CritRate || kind == game::RaidBuffKind::CritAndDirectHitRate;
}

bool has_direct_hit(game::RaidBuffKind kind) {
    return kind == game::RaidBuffKind::DirectHitRate || kind == game::RaidBuffKind::CritAndDirectHitRate;
}

/// What the hit was bound to land as. Ticks roll their own and are never guaranteed.
uint8_t guaranteed_bits(const AttributedHit& hit, std::span<const ipc::CombatStatusEntry> on_source) {
    if (hit.is_tick) return 0;
    if (const auto by_action = game::guaranteed_hit_of_action(hit.action_id); by_action != game::GuaranteedHit::None) {
        return static_cast<uint8_t>(by_action);
    }
    uint8_t bits = 0;
    if (game::is_form_bonus_crit_action(hit.action_id)) {
        for (const auto& entry : on_source) {
            if (game::is_form_bonus_status(entry.status_id)) bits |= static_cast<uint8_t>(game::GuaranteedHit::Crit);
        }
    }
    // Life Surge, Reassembled and the like guarantee weaponskills only, all on the GCD.
    if (game::is_gcd_action(hit.action_id)) {
        for (const auto& entry : on_source) {
            bits |= static_cast<uint8_t>(game::guaranteed_hit_of_status(entry.status_id));
        }
    }
    return bits;
}

} // namespace

void RateEstimator::observe(EntityId entity, bool crit, bool direct_hit) {
    Counts& counts = m_counts[entity];
    ++counts.hits;
    if (crit) ++counts.crits;
    if (direct_hit) ++counts.direct_hits;
}

double RateEstimator::crit_rate(EntityId entity) const {
    const auto it = m_counts.find(entity);
    return it == m_counts.end() ? estimate(0, 0, kPriorCrit) : estimate(it->second.hits, it->second.crits, kPriorCrit);
}

double RateEstimator::direct_hit_rate(EntityId entity) const {
    const auto it = m_counts.find(entity);
    return it == m_counts.end()
        ? estimate(0, 0, kPriorDirectHit)
        : estimate(it->second.hits, it->second.direct_hits, kPriorDirectHit);
}

void BuffStrengths::on_action(EntityId source, ActionId action_id) {
    if (const game::DanceFinish* finish = game::find_dance_finish(action_id)) {
        if (finish->technical) {
            m_applied[key(source, game::TECHNICAL_FINISH_STATUS)] = finish->permille;
        } else {
            m_applied[key(source, game::STANDARD_FINISH_STATUS)] = finish->permille;
            m_applied[key(source, game::STANDARD_FINISH_PARTNER_STATUS)] = finish->permille;
        }
        return;
    }
    for (size_t i = 0; i < game::BARD_SONG_ACTIONS.size(); ++i) {
        if (game::BARD_SONG_ACTIONS[i] == action_id) {
            m_codas[source] |= static_cast<uint8_t>(1u << i);
            return;
        }
    }
    if (action_id == game::RADIANT_FINALE_ACTION) {
        const auto it = m_codas.find(source);
        // Songs sung before the meter was watching leave the count unknown.
        if (it != m_codas.end() && it->second != 0) {
            m_applied[key(source, game::RADIANT_FINALE_STATUS)] =
                static_cast<uint16_t>(game::RADIANT_FINALE_PER_CODA_PERMILLE * std::popcount(it->second));
            it->second = 0;
        } else {
            m_applied.erase(key(source, game::RADIANT_FINALE_STATUS));
        }
    }
}

uint16_t BuffStrengths::permille(EntityId giver, uint16_t status_id, uint16_t fallback) const {
    const auto it = m_applied.find(key(giver, status_id));
    return it != m_applied.end() ? it->second : fallback;
}

Attribution attribute_hit(
    const AttributedHit& hit,
    std::span<const ipc::CombatStatusEntry> on_source,
    std::span<const ipc::CombatStatusEntry> on_target,
    const RateEstimator& rates,
    const BuffStrengths& strengths
) {
    Attribution out;
    if (hit.damage == 0 || !game::is_real_entity_id(hit.source)) {
        return out;
    }

    std::array<PresentBuff, 2 * game::definitions::MAX_STATUS_SLOTS> present{};
    size_t present_count = 0;
    const auto collect = [&](std::span<const ipc::CombatStatusEntry> entries, bool enemy_side) {
        for (const auto& entry : entries) {
            const game::RaidBuff* buff = game::find_raid_buff(entry.status_id);
            if (buff == nullptr || buff->on_enemy != enemy_side || present_count == present.size()) continue;
            const EntityId giver = entry.source_id;
            present[present_count++] = PresentBuff{
                buff, giver, strength_of(*buff, giver, hit.source_role, strengths),
                game::is_real_entity_id(giver) && giver != hit.source};
        }
    };
    collect(on_source, false);
    collect(on_target, true);

    // Rate bonuses from every buff count toward the chance, the attacker's own included;
    // only the external ones are credited.
    double crit_all = 0.0, crit_external = 0.0, dh_all = 0.0, dh_external = 0.0;
    for (size_t i = 0; i < present_count; ++i) {
        const PresentBuff& p = present[i];
        if (has_crit(p.buff->kind)) {
            crit_all += p.value;
            if (p.external) crit_external += p.value;
        }
        if (has_direct_hit(p.buff->kind)) {
            dh_all += p.value;
            if (p.external) dh_external += p.value;
        }
    }

    const uint8_t guaranteed = guaranteed_bits(hit, on_source);
    const bool sure_crit = (guaranteed & static_cast<uint8_t>(game::GuaranteedHit::Crit)) != 0;
    const bool sure_dh = (guaranteed & static_cast<uint8_t>(game::GuaranteedHit::DirectHit)) != 0;
    out.clean = !hit.is_tick && guaranteed == 0 && crit_all == 0.0 && dh_all == 0.0;

    const double crit_rate = rates.crit_rate(hit.source);
    const double dh_rate = rates.direct_hit_rate(hit.source);
    const double crit_bonus = crit_rate + kCritDamageOverRate - 1.0;

    // A tick has no outcome, so its rate buffs earn what they add on average.
    const auto expected = [](double rate, double bonus) { return 1.0 + std::min(rate, 1.0) * bonus; };
    const double tick_crit = crit_external > 0.0
        ? expected(crit_rate + crit_all, crit_bonus) / expected(crit_rate + crit_all - crit_external, crit_bonus)
        : 1.0;
    const double tick_dh = dh_external > 0.0
        ? expected(dh_rate + dh_all, kDirectHitBonus) / expected(dh_rate + dh_all - dh_external, kDirectHitBonus)
        : 1.0;

    const auto crit_multiplier = [&](double rate) {
        if (hit.is_tick) return std::pow(tick_crit, rate / crit_external);
        // A guaranteed crit turns the rate into damage.
        if (sure_crit) return 1.0 + rate * crit_bonus;
        // A rolled crit's bonus is shared by everything that made the crit likely.
        if (hit.crit) return std::pow(1.0 + crit_bonus, rate / (crit_rate + crit_all));
        return 1.0;
    };
    const auto dh_multiplier = [&](double rate) {
        if (hit.is_tick) return std::pow(tick_dh, rate / dh_external);
        if (sure_dh) return 1.0 + rate * kDirectHitBonus;
        if (hit.direct_hit) return std::pow(1.0 + kDirectHitBonus, rate / (dh_rate + dh_all));
        return 1.0;
    };

    std::array<double, 2 * game::definitions::MAX_STATUS_SLOTS> logs{};
    double log_total = 0.0;
    for (size_t i = 0; i < present_count; ++i) {
        const PresentBuff& p = present[i];
        if (!p.external) continue;
        double multiplier = 1.0;
        switch (p.buff->kind) {
            case game::RaidBuffKind::Damage: multiplier = 1.0 + p.value; break;
            case game::RaidBuffKind::CritRate: multiplier = crit_multiplier(p.value); break;
            case game::RaidBuffKind::DirectHitRate: multiplier = dh_multiplier(p.value); break;
            case game::RaidBuffKind::CritAndDirectHitRate:
                multiplier = crit_multiplier(p.value) * dh_multiplier(p.value);
                break;
        }
        logs[i] = multiplier > 1.0 ? std::log(multiplier) : 0.0;
        log_total += logs[i];
    }
    if (log_total <= 0.0) {
        return out;
    }

    // What the hit would have done without them, and each buff's log share of the rest.
    const double damage = static_cast<double>(hit.damage);
    const double bonus = damage - damage / std::exp(log_total);
    std::array<double, ipc::MAX_BUFF_CREDITS> amounts{};
    for (size_t i = 0; i < present_count; ++i) {
        if (logs[i] <= 0.0) continue;
        const PresentBuff& p = present[i];
        const uint8_t single = p.buff->single_target ? 1 : 0;
        size_t slot = 0;
        while (slot < out.credits.count
               && !(out.credits.entries[slot].giver_id == p.giver && out.credits.entries[slot].single_target == single)) {
            ++slot;
        }
        if (slot == out.credits.count) {
            if (out.credits.count == ipc::MAX_BUFF_CREDITS) continue;
            out.credits.entries[slot].giver_id = p.giver;
            out.credits.entries[slot].single_target = single;
            ++out.credits.count;
        }
        amounts[slot] += bonus * logs[i] / log_total;
    }
    for (size_t slot = 0; slot < out.credits.count; ++slot) {
        out.credits.entries[slot].amount = static_cast<uint32_t>(std::floor(amounts[slot]));
    }
    return out;
}

uint64_t credited_total(const ipc::CombatBuffCredits& credits) noexcept {
    uint64_t total = 0;
    const size_t count = std::min<size_t>(credits.count, ipc::MAX_BUFF_CREDITS);
    for (size_t i = 0; i < count; ++i) {
        total += credits.entries[i].amount;
    }
    return total;
}

bool is_attribution_status(uint32_t status_id) noexcept {
    return game::find_raid_buff(status_id) != nullptr
        || game::guaranteed_hit_of_status(status_id) != game::GuaranteedHit::None
        || game::is_form_bonus_status(status_id)
        || game::find_tick_status(status_id) != nullptr;
}

} // namespace hub::meter
