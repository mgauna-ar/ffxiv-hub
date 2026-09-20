#pragma once

#include <cstdint>
#include <string>

namespace hub::game {

using ActionId = uint32_t;

/// Display name for a known FFXIV action/spell ID; falls back to "Action <id>" when unknown.
[[nodiscard]] inline std::string action_name(ActionId action_id) {
    switch (action_id) {
        case 31: return "Heavy Swing";
        case 120: return "Cure";
        case 124: return "Medica";
        case 135: return "Cure II";
        case 137: return "Regen";
        case 167: return "Energy Drain";
        case 185: return "Adloquium";
        case 186: return "Succor";
        case 1205: return "Dia";
        case 3571: return "Assize";
        case 3576: return "Blizzard IV";
        case 3577: return "Fire IV";
        case 3594: return "Benefic";
        case 3595: return "Aspected Benefic";
        case 3601: return "Aspected Helios";
        case 3610: return "Benefic II";
        case 7388: return "Rampart";
        case 7449: return "Akh Morn";
        case 16407: return "Glare III";
        case 16505: return "Despair";
        case 16507: return "Xenoglossy";
        case 16518: return "Revelation";
        case 16534: return "Afflatus Solace";
        case 16535: return "Afflatus Rapture";
        case 16537: return "Whispering Dawn";
        case 16540: return "Biolysis";
        case 16554: return "Combust III";
        case 24283: return "Dosis III";
        case 24284: return "Diagnosis";
        case 24286: return "Prognosis";
        case 24293: return "Eukrasian Dosis III";
        case 25797: return "Paradox";
        case 25820: return "Astral Impulse";
        case 25821: return "Sunflare";
        case 25865: return "Broil IV";
        case 25871: return "Fall Malefic";
        case 34606: return "Steel Fangs";
        case 34607: return "Reaving Fangs";
        case 34614: return "Dreadwinder";
        case 34650: return "Fire in Red";
        case 34651: return "Aero in Green";
        case 34652: return "Water in Blue";
        default: return "Action " + std::to_string(action_id);
    }
}

} // namespace hub::game
