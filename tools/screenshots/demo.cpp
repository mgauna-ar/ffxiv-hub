#include "demo.hpp"
#include "app_access.hpp"

#include "common/ipc/protocol.hpp"
#include "hub/game_state.hpp"
#include "meter/encounter_engine.hpp"
#include "mitigator/animation_lock.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace shots {

namespace {

using hub::game::Job;
using hub::ipc::CombatActionPayload;
using hub::meter::EffectType;
using hub::meter::HitSeverity;
namespace HitFlags = hub::meter::HitFlags;

/// Deterministic across standard libraries, unlike <random>'s distributions.
class Rng {
public:
    explicit Rng(uint64_t seed) : m_state(seed) {}
    uint64_t next() {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    bool chance(double p) { return uniform() < p; }

private:
    uint64_t m_state;
};

struct Move {
    uint32_t action;
    double potency;
};

struct Member {
    uint32_t id;
    const char* name;
    Job job;
    uint32_t max_hp;
    double dps;           ///< Over a clean pull
    double crit;          ///< Rates, 0..1
    double direct_hit;
    double gcd_s;
    uint32_t auto_action; ///< 0 for none
    int weave_every;      ///< One oGCD after every this many GCDs
    std::vector<Move> gcds;
    std::vector<Move> ogcds;
    double heal_hps{0.0}; ///< Raw healing per second, healers only
    std::vector<uint32_t> heals{};
};

constexpr uint32_t kAttack = 7;  // "attack", the melee auto-attack
constexpr uint32_t kShot = 8;    // "Shot", the ranged one

const std::vector<Member>& party() {
    static const std::vector<Member> members = {
        {0x1040A1B1, "Rowan Ashgrove", Job::PLD, 196'400, 16'900, 0.245, 0.148, 2.50, kAttack, 2,
         {{9, 220}, {15, 330}, {3539, 460}, {16460, 460}, {36918, 500}, {36919, 540}, {7384, 500},
          {16459, 500}, {25748, 260}, {25749, 380}, {25750, 500}},
         {{23, 140}, {25747, 450}, {16461, 150}, {36921, 580}, {36922, 1000}}},
        {0x1040A1B2, "Kaede Tsukimi", Job::GNB, 201'100, 18'300, 0.263, 0.181, 2.45, kAttack, 1,
         {{16137, 300}, {16139, 380}, {16145, 460}, {16146, 500}, {16147, 560}, {16150, 620},
          {25760, 1200}, {16153, 300}, {16162, 460}, {36937, 800}, {36938, 1000}, {36939, 1200}},
         {{16156, 240}, {16157, 280}, {16158, 320}, {25759, 220}, {16159, 150}, {16165, 800}}},
        {0x1040A1B3, "Lili Moonpetal", Job::WHM, 118'300, 13'700, 0.224, 0.112, 2.44, 0, 6,
         {{37009, 310}, {37009, 310}, {37009, 310}, {16532, 75}, {16535, 1360}},
         {{3571, 400}},
         11'600, {16531, 16534, 37010, 135}},
        {0x1040A1B4, "Seren Valewind", Job::SGE, 117'200, 13'200, 0.217, 0.106, 2.45, 0, 8,
         {{24312, 380}, {24312, 380}, {24312, 380}, {24314, 80}, {24313, 600}, {24318, 380}},
         {{37033, 600}},
         9'800, {24296, 24298, 37034, 24318}},
        {0x1040A1B5, "Hayato Kurogane", Job::SAM, 131'000, 26'900, 0.302, 0.438, 2.07, kAttack, 2,
         {{36963, 240}, {7480, 340}, {7481, 420}, {7482, 420}, {7487, 640}, {16486, 640},
          {7489, 200}, {25781, 1000}, {25782, 1000}, {36966, 1100}, {36968, 1100}},
         {{7490, 250}, {16481, 800}, {16487, 640}, {36964, 900}}},
        {0x1040A1B6, "Nyx Thornfield", Job::VPR, 131'000, 26'300, 0.296, 0.425, 2.08, kAttack, 1,
         {{34606, 200}, {34607, 200}, {34608, 300}, {34609, 300}, {34610, 400}, {34613, 400},
          {34620, 500}, {34621, 570}, {34622, 570}, {34627, 480}, {34630, 480}, {34631, 700}},
         {{34636, 120}, {34637, 120}, {34633, 680}}},
        {0x1040A1B7, "Mira Songweald", Job::BRD, 121'300, 23'100, 0.271, 0.512, 2.48, kShot, 1,
         {{16495, 220}, {7409, 280}, {7406, 150}, {7407, 100}, {3560, 100}, {16496, 600},
          {25784, 600}, {36977, 500}, {36976, 640}},
         {{3558, 260}, {3562, 400}, {7404, 360}, {36975, 180}}},
        {0x1040A1B8, "Tobias Inkwell", Job::PCT, 118'700, 27'600, 0.279, 0.381, 2.48, 0, 3,
         {{34650, 440}, {34651, 480}, {34652, 520}, {34653, 800}, {34654, 840}, {34655, 880},
          {34662, 520}, {34663, 880}, {34678, 560}, {34679, 580}, {34680, 600}, {34681, 1400},
          {34688, 1000}},
         {{34670, 1100}, {34671, 1100}, {34676, 1300}, {34677, 1400}}},
    };
    return members;
}

/// Index of the player whose client the payload runs in: the Samurai.
constexpr size_t kLocalPlayer = 4;

struct Boss {
    uint32_t id;
    const char* name;
};

constexpr Boss kBruteAbombinator{0x40010A1C, "Brute Abombinator"};
constexpr Boss kHowlingBlade{0x40010A2C, "Howling Blade"};
constexpr uint32_t kZoneM3S = 1261;   // AAC Cruiserweight M3 (Savage)
constexpr uint32_t kZoneM4S = 1263;   // AAC Cruiserweight M4 (Savage)
constexpr uint32_t kHowlingBladeHp = 98'600'000;

// Boss abilities, by their Action sheet rows.
constexpr uint32_t kExtraplanarPursuit = 41870;  // raidwide
constexpr uint32_t kGreatDivide = 41869;         // tankbuster
constexpr uint32_t kTrackingTremors = 41913;     // stack
constexpr uint32_t kHowlingHavoc = 41947;        // what the wipes end on

enum class Ending { Wipe, Kill, Live };

/// Someone who dies mid-pull and is raised.
struct Death {
    size_t member;
    double at;       ///< Seconds into the pull
    double raised;
};

struct Pull {
    uint32_t zone;
    Boss boss;
    double start;       ///< Seconds into the night
    double length;      ///< Seconds of fighting
    Ending ending;
    double efficiency;  ///< Share of the party's clean-pull output this one reached
    std::optional<Death> death;
    uint64_t seed;
};

struct Event {
    enum class Kind { Action, EnemyHp, GameState, Died, Raised, ActorHp, Zone, Tick };
    double t{0.0};
    Kind kind{Kind::Action};
    CombatActionPayload action{};
    uint32_t entity{0};
    uint32_t hp{0};
    uint32_t max_hp{0};
    uint32_t flags{0};
};

/// One of the local player's actions, for the mitigator's telemetry.
struct Press {
    double t;
    uint32_t action;
    bool cast;
};

class Night {
public:
    std::vector<Event> events;
    std::vector<Press> presses;

    void zone(double t, uint32_t zone_id) {
        Event e;
        e.t = t;
        e.kind = Event::Kind::Zone;
        e.entity = zone_id;
        events.push_back(e);
    }

    void party_alive(double t) {
        for (const Member& m : party()) actor_hp(t, m.id, m.max_hp, m.max_hp);
    }

    void play(const Pull& pull) {
        Rng rng(pull.seed);
        const double end = pull.start + pull.length;
        std::vector<Event> hits;

        const auto dead_at = [&](size_t member, double t) {
            return pull.death && pull.death->member == member &&
                   t >= pull.start + pull.death->at && t < pull.start + pull.death->raised + 4.0;
        };

        for (size_t i = 0; i < party().size(); ++i) {
            const Member& m = party()[i];
            // Tanks open the pull; everyone else joins within the first second.
            std::vector<std::pair<double, Move>> moves;
            double t = pull.start + (i < 2 ? static_cast<double>(i) * 0.4 : rng.range(0.4, 1.3));
            size_t gcd_i = 0;
            size_t ogcd_i = 0;
            for (int n = 0; t < end; ++n) {
                moves.push_back({t, m.gcds[gcd_i++ % m.gcds.size()]});
                if (!m.ogcds.empty() && n % m.weave_every == m.weave_every - 1 && t + 0.7 < end) {
                    moves.push_back({t + 0.7, m.ogcds[ogcd_i++ % m.ogcds.size()]});
                }
                t += m.gcd_s * rng.range(0.995, 1.012);
            }
            if (m.auto_action != 0) {
                for (double a = pull.start + (i == 0 ? 0.0 : 0.6); a < end; a += 3.0 * rng.range(0.99, 1.01)) {
                    moves.push_back({a, Move{m.auto_action, 90}});
                }
            }
            std::erase_if(moves, [&](const auto& mv) { return dead_at(i, mv.first); });

            // Scaled so the pull lands on the member's usual output.
            const double multiplier = (1.0 + m.crit * 0.6) * (1.0 + m.direct_hit * 0.25);
            double potency = 0.0;
            for (const auto& mv : moves) potency += mv.second.potency;
            const double scale = m.dps * pull.efficiency * pull.length / (potency * multiplier);

            for (const auto& [at, move] : moves) {
                const bool crit = rng.chance(m.crit);
                const bool direct = rng.chance(m.direct_hit);
                const double amount = scale * move.potency * rng.range(0.95, 1.05) *
                                      (crit ? 1.6 : 1.0) * (direct ? 1.25 : 1.0);
                hits.push_back(damage(at, m.id, pull.boss.id, move.action, amount, crit, direct));
                if (i == kLocalPlayer && move.action != kAttack) {
                    presses.push_back({at, move.action, is_iaijutsu(move.action)});
                }
            }

            if (m.heal_hps > 0.0) heal(pull, i, rng, hits, dead_at);
        }

        boss_attacks(pull, rng, hits);

        std::stable_sort(hits.begin(), hits.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
        const uint32_t max_hp = pull.ending == Ending::Kill ? total_boss_damage(hits, pull.boss.id)
                                                            : kHowlingBladeHp;
        // The pull opens on the first hit, so the boss's name has to be known before it.
        actor(pull.start - 0.5, pull.boss.id, pull.boss.name, max_hp, max_hp,
              static_cast<uint8_t>(hub::meter::ActorType::Monster));
        events.insert(events.end(), hits.begin(), hits.end());

        boss_hp(pull, hits, max_hp);
        combat_flags(pull);

        if (pull.death) {
            const Death& d = *pull.death;
            died(pull.start + d.at + 0.05, party()[d.member].id);
            actor_hp(pull.start + d.at + 0.1, party()[d.member].id, 0, party()[d.member].max_hp);
            raised(pull.start + d.raised, party()[d.member].id);
            actor_hp(pull.start + d.raised + 0.05, party()[d.member].id, party()[d.member].max_hp / 2,
                     party()[d.member].max_hp);
        }

        if (pull.ending == Ending::Wipe) {
            for (const Member& m : party()) {
                events.push_back(damage(end + 0.2, pull.boss.id, m.id, kHowlingHavoc, m.max_hp * 1.4, false, false));
                died(end + 0.3, m.id);
            }
            for (const Member& m : party()) actor_hp(end + 0.4, m.id, 0, m.max_hp);
            game_state(end + 1.0, false);
        } else if (pull.ending == Ending::Kill) {
            enemy_hp(end + 0.05, pull.boss.id, 0, max_hp);
            game_state(end + 1.0, false);
            tick(end + 3.5);
        }
    }

private:
    static bool is_iaijutsu(uint32_t action) {
        // Midare Setsugekka, Higanbana, Tendo Setsugekka: the Samurai's casts.
        return action == 7487 || action == 7489 || action == 36966;
    }

    static Event damage(double t, uint32_t source, uint32_t target, uint32_t action, double amount,
                        bool crit, bool direct) {
        Event e;
        e.t = t;
        e.kind = Event::Kind::Action;
        e.action.source_id = source;
        e.action.target_id = target;
        e.action.action_id = action;
        e.action.damage = static_cast<uint32_t>(amount);
        e.action.effect_type = static_cast<uint16_t>(EffectType::Damage);
        e.action.hit_flags = static_cast<uint16_t>((crit ? HitFlags::Crit : 0) | (direct ? HitFlags::DirectHit : 0));
        e.action.severity = static_cast<uint8_t>(crit && direct ? HitSeverity::CritDirectHit
                                                 : crit          ? HitSeverity::Critical
                                                 : direct        ? HitSeverity::DirectHit
                                                                 : HitSeverity::Normal);
        return e;
    }

    template <typename DeadFn>
    void heal(const Pull& pull, size_t healer, Rng& rng, std::vector<Event>& out, DeadFn&& dead_at) {
        const Member& m = party()[healer];
        const double end = pull.start + pull.length;
        size_t spell = 0;
        for (double t = pull.start + 6.0; t < end; t += rng.range(2.2, 3.4)) {
            if (dead_at(healer, t)) continue;
            const uint32_t action = m.heals[spell++ % m.heals.size()];
            const bool aoe = spell % 2 == 0;
            const double per_target = m.heal_hps * 2.8 / (aoe ? 8.0 : 1.0);
            const size_t first = aoe ? 0 : (rng.chance(0.7) ? 0 : 1);
            const size_t last = aoe ? party().size() : first + 1;
            for (size_t target = first; target < last; ++target) {
                const bool crit = rng.chance(m.crit);
                const double raw = per_target * rng.range(0.9, 1.1) * (crit ? 1.6 : 1.0);
                const double over = raw * rng.range(0.08, 0.45);
                Event e;
                e.t = t;
                e.kind = Event::Kind::Action;
                e.action.source_id = m.id;
                e.action.target_id = party()[target].id;
                e.action.action_id = action;
                e.action.damage = static_cast<uint32_t>(raw);
                e.action.effective_heal = static_cast<uint32_t>(raw - over);
                e.action.overheal = static_cast<uint32_t>(over);
                e.action.effect_type = static_cast<uint16_t>(EffectType::Heal);
                e.action.hit_flags = crit ? HitFlags::Crit : 0;
                e.action.severity = static_cast<uint8_t>(crit ? HitSeverity::Critical : HitSeverity::Normal);
                out.push_back(e);
            }
        }
    }

    void boss_attacks(const Pull& pull, Rng& rng, std::vector<Event>& out) {
        const double end = pull.start + pull.length;
        const auto& members = party();
        for (double t = pull.start + 2.0; t < end; t += 3.0) {
            out.push_back(damage(t, pull.boss.id, members[0].id, kAttack, rng.range(16'000, 24'000), false, false));
        }
        for (double t = pull.start + 12.0; t < end; t += 42.0) {
            for (const Member& m : members) {
                out.push_back(damage(t, pull.boss.id, m.id, kExtraplanarPursuit, rng.range(58'000, 74'000), false, false));
            }
        }
        for (double t = pull.start + 30.0; t < end; t += 71.0) {
            for (size_t tank = 0; tank < 2; ++tank) {
                out.push_back(damage(t, pull.boss.id, members[tank].id, kGreatDivide, rng.range(88'000, 112'000), false, false));
            }
        }
        for (double t = pull.start + 48.0; t < end; t += 55.0) {
            for (const Member& m : members) {
                out.push_back(damage(t, pull.boss.id, m.id, kTrackingTremors, rng.range(34'000, 42'000), false, false));
            }
        }
        if (pull.death) {
            const Member& m = members[pull.death->member];
            out.push_back(damage(pull.start + pull.death->at, pull.boss.id, m.id, kTrackingTremors,
                                 m.max_hp * 1.08, false, false));
        }
    }

    static uint32_t total_boss_damage(const std::vector<Event>& hits, uint32_t boss) {
        uint64_t total = 0;
        for (const Event& e : hits) {
            if (e.kind == Event::Kind::Action && e.action.target_id == boss &&
                e.action.effect_type == static_cast<uint16_t>(EffectType::Damage)) {
                total += e.action.damage;
            }
        }
        return static_cast<uint32_t>(total);
    }

    /// Enemy HP as the payload reads it, once a second.
    void boss_hp(const Pull& pull, const std::vector<Event>& hits, uint32_t max_hp) {
        uint64_t dealt = 0;
        size_t next = 0;
        for (double t = pull.start + 1.0; t < pull.start + pull.length; t += 1.0) {
            while (next < hits.size() && hits[next].t <= t) {
                const Event& e = hits[next++];
                if (e.action.target_id == pull.boss.id && e.action.effect_type == static_cast<uint16_t>(EffectType::Damage)) {
                    dealt += e.action.damage;
                }
            }
            const uint32_t left = dealt >= max_hp ? 1u : static_cast<uint32_t>(max_hp - dealt);
            enemy_hp(t, pull.boss.id, left, max_hp);
        }
    }

    /// The client's combat flag, which the payload reports at least once a second.
    void combat_flags(const Pull& pull) {
        for (double t = pull.start; t < pull.start + pull.length; t += 1.0) game_state(t, true);
    }

    void game_state(double t, bool in_combat) {
        Event e;
        e.t = t;
        e.kind = Event::Kind::GameState;
        e.flags = hub::to_bits(hub::GameStateFlag::Valid) |
                  (in_combat ? hub::to_bits(hub::GameStateFlag::InCombat) : 0u);
        events.push_back(e);
    }

    void enemy_hp(double t, uint32_t id, uint32_t hp, uint32_t max_hp) {
        Event e;
        e.t = t;
        e.kind = Event::Kind::EnemyHp;
        e.entity = id;
        e.hp = hp;
        e.max_hp = max_hp;
        events.push_back(e);
    }

    void actor_hp(double t, uint32_t id, uint32_t hp, uint32_t max_hp) {
        Event e;
        e.t = t;
        e.kind = Event::Kind::ActorHp;
        e.entity = id;
        e.hp = hp;
        e.max_hp = max_hp;
        events.push_back(e);
    }

    void actor(double t, uint32_t id, const char* name, uint32_t hp, uint32_t max_hp, uint8_t type) {
        Event e = Event{};
        e.t = t;
        e.kind = Event::Kind::ActorHp;
        e.entity = id;
        e.hp = hp;
        e.max_hp = max_hp;
        e.flags = type;
        e.action.action_id = 0;
        std::strncpy(m_names[id].data(), name, m_names[id].size() - 1);
        events.push_back(e);
    }

    void died(double t, uint32_t id) {
        Event e;
        e.t = t;
        e.kind = Event::Kind::Died;
        e.entity = id;
        events.push_back(e);
    }

    void raised(double t, uint32_t id) {
        Event e;
        e.t = t;
        e.kind = Event::Kind::Raised;
        e.entity = id;
        events.push_back(e);
    }

    void tick(double t) {
        Event e;
        e.t = t;
        e.kind = Event::Kind::Tick;
        events.push_back(e);
    }

public:
    /// Names of the non-party actors, as their actor info carries them.
    std::map<uint32_t, std::array<char, hub::ipc::MAX_ACTOR_NAME_LEN>> m_names;
};

hub::ipc::CombatActorInfoPayload actor_info(uint32_t id, const char* name, Job job, uint32_t hp, uint32_t max_hp,
                                            hub::meter::ActorType type) {
    hub::ipc::CombatActorInfoPayload info{};
    info.entity_id = id;
    info.job_id = static_cast<uint32_t>(job);
    info.max_hp = max_hp;
    info.current_hp = hp;
    info.world_id = 0;
    info.actor_type = static_cast<uint8_t>(type);
    std::memcpy(info.name, name, std::min(std::strlen(name), sizeof(info.name) - 1));
    return info;
}

/// Replays the night into the engine, each event at its own moment.
void replay(const Night& night, hub::meter::EncounterEngine& engine, std::chrono::steady_clock::time_point origin,
            uint64_t origin_us) {
    const auto at = [&](double t) {
        return origin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(t));
    };
    const auto us = [&](double t) { return origin_us + static_cast<uint64_t>(t * 1e6); };

    std::vector<Event> events = night.events;
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.t < b.t; });

    const auto member = [](uint32_t id) -> const Member* {
        for (const Member& m : party()) {
            if (m.id == id) return &m;
        }
        return nullptr;
    };

    for (const Event& e : events) {
        const auto now = at(e.t);
        switch (e.kind) {
            case Event::Kind::Action: {
                CombatActionPayload packet = e.action;
                packet.timestamp_us = us(e.t);
                engine.process_action(packet, now);
                break;
            }
            case Event::Kind::EnemyHp: {
                hub::ipc::CombatEnemyHpPayload hp{};
                hp.entity_id = e.entity;
                hp.current_hp = e.hp;
                hp.max_hp = e.max_hp;
                hp.timestamp_us = us(e.t);
                engine.process_enemy_hp(hp);
                break;
            }
            case Event::Kind::GameState:
                engine.set_game_state(e.flags, now);
                break;
            case Event::Kind::Died:
            case Event::Kind::Raised: {
                const auto kind = e.kind == Event::Kind::Died ? hub::ipc::LifeEventKind::Death
                                                              : hub::ipc::LifeEventKind::Raise;
                engine.process_life_event(engine.build_life_event(e.entity, kind, us(e.t)));
                break;
            }
            case Event::Kind::ActorHp: {
                if (const Member* m = member(e.entity)) {
                    engine.process_actor_info(actor_info(m->id, m->name, m->job, e.hp, e.max_hp,
                                                         hub::meter::ActorType::Player), now);
                } else {
                    const auto name = night.m_names.find(e.entity);
                    engine.process_actor_info(actor_info(e.entity, name != night.m_names.end() ? name->second.data() : "",
                                                         Job::None, e.hp, e.max_hp,
                                                         static_cast<hub::meter::ActorType>(e.flags)), now);
                }
                break;
            }
            case Event::Kind::Zone: {
                hub::ipc::CombatControlPayload control{};
                control.zone_id = e.entity;
                control.timestamp_us = us(e.t);
                engine.process_encounter_control(control, now);
                break;
            }
            case Event::Kind::Tick:
                engine.update(now);
                break;
        }
    }
}

/// The local player's round trips through the real mitigator, sent as the
/// payload's telemetry. Returns the smoothed RTT the HUD shows at the end.
double replay_round_trips(const Night& night, hub::app::AppState& app, double night_length,
                          std::chrono::steady_clock::time_point origin) {
    using namespace std::chrono;
    const auto at = [&](double t) {
        return origin + duration_cast<steady_clock::duration>(duration<double>(t));
    };
    // A fixed evening, 2026-09-26 21:47 UTC, so the feed's clock column renders the same
    // on every run (the Makefile pins TZ).
    constexpr long long wall_now_ms = 1'790'459'220'000;

    hub::mitigator::AnimationLockMitigator mitigator;
    Rng rng(0x51A7E);
    uint32_t sequence = 4108;
    for (const Press& press : night.presses) {
        // Round trips cluster around 214 ms, with the odd spike the filter is for.
        double rtt_ms = rng.range(203.0, 226.0);
        if (rng.chance(0.02)) rtt_ms = rng.range(420.0, 610.0);
        const double sent = press.t - rtt_ms / 1000.0;
        ++sequence;
        if (press.cast) {
            mitigator.record_cast_begin(press.action, 1.3f, at(sent));
        } else {
            mitigator.record_cast_interrupt(at(sent));
        }
        mitigator.record_action_request(press.action, sequence, at(sent), press.cast, press.cast ? 1.3f : 0.0f);
        const double landed = press.cast ? press.t + 1.3 : press.t;
        const double lock_ms = press.cast ? 100.0 : 600.0;
        const auto res = mitigator.calculate_mitigation(press.action, sequence, lock_ms, at(landed));

        hub::ipc::MitigatorTelemetryPayload payload{};
        payload.action_id = res.action_id;
        payload.sequence = res.sequence;
        payload.original_lock_ms = static_cast<float>(res.original_lock_ms);
        payload.adjusted_lock_ms = static_cast<float>(res.adjusted_lock_ms);
        payload.delay_reduced_ms = static_cast<float>(res.delay_reduced_ms);
        payload.measured_rtt_ms = static_cast<float>(res.measured_rtt_ms);
        payload.smoothed_rtt_ms = static_cast<float>(res.smoothed_rtt_ms);
        payload.jitter_ms = static_cast<float>(mitigator.get_rtt_tracker().get_jitter_ms());
        payload.clamped_floor = res.clamped_by_floor ? 1 : 0;
        payload.dry_run = 0;
        payload.applied = res.applied ? 1 : 0;
        payload.cast_active = res.cast_active ? 1 : 0;
        payload.spike_filtered = res.spike_filtered ? 1 : 0;
        payload.cold_start_guard = res.cold_start_guard ? 1 : 0;
        payload.timestamp_ms = static_cast<uint64_t>(wall_now_ms - static_cast<long long>((night_length - landed) * 1000.0));
        app.pipe_server().process_raw_packet(hub::ipc::serialize_typed_packet(
            hub::PluginId::LatencyMitigator, hub::MessageType::MitigatorTelemetry, sequence, payload));
    }
    return mitigator.get_rtt_tracker().get_smoothed_rtt_ms();
}

} // namespace

HudReading play_raid_night(hub::app::AppState& app) {
    Night night;
    night.party_alive(0.0);

    // An earlier clear of M3, then progression on M4.
    night.zone(0.5, kZoneM3S);
    night.play(Pull{kZoneM3S, kBruteAbombinator, 20.0, 511.4, Ending::Kill, 1.0, std::nullopt, 0xB0B1});
    night.zone(640.0, kZoneM4S);
    night.party_alive(641.0);
    night.play(Pull{kZoneM4S, kHowlingBlade, 700.0, 171.3, Ending::Wipe, 0.97, std::nullopt, 0xA001});
    night.party_alive(950.0);
    night.play(Pull{kZoneM4S, kHowlingBlade, 960.0, 297.6, Ending::Wipe, 0.98, std::nullopt, 0xA002});
    night.party_alive(1340.0);
    night.play(Pull{kZoneM4S, kHowlingBlade, 1350.0, 403.1, Ending::Wipe, 0.99, std::nullopt, 0xA003});
    night.party_alive(1840.0);
    constexpr double kLiveStart = 1850.0;
    night.play(Pull{kZoneM4S, kHowlingBlade, kLiveStart, kLivePullSeconds - 0.15, Ending::Live, 1.0,
                    Death{6, 192.4, 206.0}, 0xA004});
    const double night_length = kLiveStart + kLivePullSeconds;

    // The party the payload syncs, the Samurai being the local player.
    hub::ipc::CombatPartySyncPayload sync{};
    sync.party_count = static_cast<uint32_t>(party().size());
    sync.local_player_id = party()[kLocalPlayer].id;
    for (size_t i = 0; i < party().size(); ++i) {
        sync.entity_ids[i] = party()[i].id;
        sync.job_ids[i] = static_cast<uint32_t>(party()[i].job);
    }

    hub::meter::EncounterEngine& engine = access::engine(app);
    engine.process_party_sync(sync);

    const auto origin = std::chrono::steady_clock::now() -
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(night_length));
    replay(night, engine, origin, 1'000'000'000'000ull);

    HudReading hud;
    hud.smoothed_rtt_ms = replay_round_trips(night, app, night_length, origin);
    hud.network_ping_ms = 196.0;
    return hud;
}

} // namespace shots
