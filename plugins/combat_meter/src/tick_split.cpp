#include "meter/tick_split.hpp"
#include "hub/game/entity.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace hub::meter {

std::vector<uint32_t> split_exact(uint32_t total, std::span<const double> weights) {
    std::vector<uint32_t> parts(weights.size(), 0);
    if (weights.empty()) {
        return parts;
    }
    const double sum = std::accumulate(weights.begin(), weights.end(), 0.0);
    const bool even = !(sum > 0.0);

    uint64_t assigned = 0;
    std::vector<double> fractions(weights.size(), 0.0);
    for (size_t i = 0; i < weights.size(); ++i) {
        const double share = even ? 1.0 / static_cast<double>(weights.size()) : std::max(weights[i], 0.0) / sum;
        const double exact = static_cast<double>(total) * share;
        const double whole = std::floor(exact);
        parts[i] = static_cast<uint32_t>(std::min(whole, static_cast<double>(total)));
        fractions[i] = exact - whole;
        assigned += parts[i];
    }
    // Rounding can only leave units over, one per part at most; they go to the largest
    // remainders. A stray unit from floating point is taken back the same way.
    std::vector<size_t> order(weights.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return fractions[a] > fractions[b]; });
    for (size_t k = 0; assigned < total; k = (k + 1) % order.size()) {
        ++parts[order[k]];
        ++assigned;
    }
    for (size_t k = order.size(); assigned > total && k-- > 0;) {
        const size_t i = order[k];
        if (parts[i] > 0) {
            --parts[i];
            --assigned;
        }
    }
    return parts;
}

std::vector<TickShare> TickSplitter::split(game::TickKind kind, uint32_t amount, uint32_t overheal,
                                           std::span<const ipc::CombatStatusEntry> on_target) {
    struct Candidate {
        EntityId source;
        uint16_t status_id;
        uint16_t potency;
    };
    std::vector<Candidate> candidates;
    for (const ipc::CombatStatusEntry& entry : on_target) {
        const game::TickStatus* status = game::find_tick_status(entry.status_id);
        if (status == nullptr || status->kind != kind || m_own_tick.contains(entry.status_id)
            || !hub::game::is_real_entity_id(entry.source_id)) {
            continue;
        }
        candidates.push_back(Candidate{entry.source_id, entry.status_id, status->potency});
    }
    if (candidates.empty() || amount == 0) {
        return {};
    }

    // Every status from one source: the whole tick is theirs, which is exactly the
    // reading their strength is learned from.
    const EntityId first = candidates.front().source;
    const bool one_source = std::all_of(candidates.begin(), candidates.end(),
        [first](const Candidate& c) { return c.source == first; });
    if (one_source) {
        double potency = 0.0;
        for (const Candidate& c : candidates) potency += c.potency;
        if (potency > 0.0) {
            const double observed = static_cast<double>(amount) / potency;
            auto [it, inserted] = m_strength.try_emplace(key(first, kind), observed);
            if (!inserted) it->second += kLearnRate * (observed - it->second);
        }
    }

    std::vector<double> weights;
    weights.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        weights.push_back(static_cast<double>(c.potency) * strength(c.source, kind));
    }
    const std::vector<uint32_t> amounts = split_exact(amount, weights);
    const std::vector<double> amount_weights(amounts.begin(), amounts.end());
    const std::vector<uint32_t> overheals = split_exact(std::min(overheal, amount), amount_weights);

    std::vector<TickShare> shares;
    shares.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (amounts[i] == 0) continue;
        shares.push_back(TickShare{candidates[i].source, candidates[i].status_id, amounts[i],
                                   std::min(overheals[i], amounts[i])});
    }
    return shares;
}

void TickSplitter::note_own_tick(uint16_t status_id) {
    if (game::find_tick_status(status_id) != nullptr) {
        m_own_tick.insert(status_id);
    }
}

double TickSplitter::strength(EntityId source, game::TickKind kind) const {
    if (const auto it = m_strength.find(key(source, kind)); it != m_strength.end()) {
        return it->second;
    }
    const uint64_t heal_bit = kind == game::TickKind::Heal ? 1 : 0;
    double sum = 0.0;
    size_t known = 0;
    for (const auto& [k, value] : m_strength) {
        if ((k & 1) != heal_bit) continue;
        sum += value;
        ++known;
    }
    return known > 0 ? sum / static_cast<double>(known) : 1.0;
}

} // namespace hub::meter
