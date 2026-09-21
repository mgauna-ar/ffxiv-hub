#include "common/ui/job_style.hpp"
#include "common/ui/icons.hpp"

namespace hub::common::ui {

namespace {

/// Packed the way IM_COL32 does, R in the low byte, so both UIs can hand the
/// value straight to ImGui after adding their own alpha.
constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
}

// The game's own job colors, except where a hue is too dark to read against the
// slate background (GNB, DRG, SMN, VPR), which are lifted a few stops.
constexpr uint32_t PLD_BLUE    = rgb(0xA8, 0xD2, 0xE6);
constexpr uint32_t WAR_RED     = rgb(0xCF, 0x26, 0x21);
constexpr uint32_t DRK_MAGENTA = rgb(0xD1, 0x26, 0xCC);
constexpr uint32_t GNB_OLIVE   = rgb(0xB5, 0xA4, 0x5A);

constexpr uint32_t WHM_CREAM   = rgb(0xFF, 0xF0, 0xDC);
constexpr uint32_t SCH_VIOLET  = rgb(0x86, 0x57, 0xFF);
constexpr uint32_t AST_YELLOW  = rgb(0xFF, 0xE7, 0x4A);
constexpr uint32_t SGE_CYAN    = rgb(0x80, 0xFF, 0xFF);

constexpr uint32_t MNK_AMBER   = rgb(0xD6, 0x9C, 0x00);
constexpr uint32_t DRG_BLUE    = rgb(0x6B, 0x8A, 0xE8);
constexpr uint32_t NIN_ROSE    = rgb(0xAF, 0x19, 0x64);
constexpr uint32_t SAM_SAND    = rgb(0xE9, 0xB9, 0x56);
constexpr uint32_t RPR_MAUVE   = rgb(0x96, 0x5A, 0x90);
constexpr uint32_t VPR_GREEN   = rgb(0x2F, 0xBF, 0x3F);
constexpr uint32_t BST_TAN     = rgb(0xA5, 0x78, 0x3C);

constexpr uint32_t BRD_STEEL   = rgb(0x91, 0x9B, 0xBA);
constexpr uint32_t MCH_TEAL    = rgb(0x6E, 0xE1, 0xD6);
constexpr uint32_t DNC_PINK    = rgb(0xE2, 0xB0, 0xAF);

constexpr uint32_t BLM_PURPLE  = rgb(0xA5, 0x79, 0xD6);
constexpr uint32_t SMN_JADE    = rgb(0x3F, 0xBF, 0x96);
constexpr uint32_t RDM_CORAL   = rgb(0xE8, 0x7B, 0x7B);
constexpr uint32_t BLU_AZURE   = rgb(0x00, 0xB9, 0xF7);
constexpr uint32_t PCT_BLUSH   = rgb(0xFC, 0x83, 0xAC);

constexpr uint32_t CRAFT_BROWN = rgb(0xC0, 0x9B, 0x6A);
constexpr uint32_t GATHER_LEAF = rgb(0x8C, 0xB8, 0x6E);

constexpr uint32_t NEUTRAL     = rgb(0x9C, 0xA3, 0xAF);
constexpr uint32_t LB_GOLD     = rgb(0xF5, 0xB9, 0x42);

} // namespace

CombatantStyle combatant_style(game::Job job, bool is_limit_break) noexcept {
    if (is_limit_break) {
        return CombatantStyle{ICON_BOLT, LB_GOLD, "LB"};
    }

    const std::string_view label = game::job_abbreviation(job);

    switch (job) {
        // Tanks. A base class shares its job's glyph and color.
        case game::Job::GLA: case game::Job::PLD: return {ICON_SHIELD, PLD_BLUE, label};
        case game::Job::MRD: case game::Job::WAR: return {ICON_AXE, WAR_RED, label};
        case game::Job::DRK:                      return {ICON_ECLIPSE, DRK_MAGENTA, label};
        case game::Job::GNB:                      return {ICON_SWORD, GNB_OLIVE, label};

        // Healers.
        case game::Job::CNJ: case game::Job::WHM: return {ICON_HEART, WHM_CREAM, label};
        case game::Job::SCH:                      return {ICON_BOOK, SCH_VIOLET, label};
        case game::Job::AST:                      return {ICON_STAR, AST_YELLOW, label};
        case game::Job::SGE:                      return {ICON_CROSS, SGE_CYAN, label};

        // Melee.
        case game::Job::PGL: case game::Job::MNK: return {ICON_HAND, MNK_AMBER, label};
        case game::Job::LNC: case game::Job::DRG: return {ICON_ARROW_UP, DRG_BLUE, label};
        case game::Job::ROG: case game::Job::NIN: return {ICON_MASK, NIN_ROSE, label};
        case game::Job::SAM:                      return {ICON_SWORDS, SAM_SAND, label};
        case game::Job::RPR:                      return {ICON_SKULL, RPR_MAUVE, label};
        case game::Job::VPR:                      return {ICON_WORM, VPR_GREEN, label};
        case game::Job::BST:                      return {ICON_PAW, BST_TAN, label};

        // Physical ranged.
        case game::Job::ARC: case game::Job::BRD: return {ICON_MUSIC, BRD_STEEL, label};
        case game::Job::MCH:                      return {ICON_CROSSHAIR, MCH_TEAL, label};
        case game::Job::DNC:                      return {ICON_FEATHER, DNC_PINK, label};

        // Casters.
        case game::Job::THM: case game::Job::BLM: return {ICON_FLAME, BLM_PURPLE, label};
        case game::Job::ACN: case game::Job::SMN: return {ICON_GEM, SMN_JADE, label};
        case game::Job::RDM:                      return {ICON_WAND, RDM_CORAL, label};
        case game::Job::BLU:                      return {ICON_DROPLET, BLU_AZURE, label};
        case game::Job::PCT:                      return {ICON_BRUSH, PCT_BLUSH, label};

        // Crafters and gatherers only surface here through a stray party sync.
        case game::Job::CRP: case game::Job::BSM: case game::Job::ARM:
        case game::Job::GSM: case game::Job::LTW: case game::Job::WVR:
        case game::Job::ALC: case game::Job::CUL:
            return {ICON_HAMMER, CRAFT_BROWN, label};
        case game::Job::MIN: case game::Job::BTN: case game::Job::FSH:
            return {ICON_PICKAXE, GATHER_LEAF, label};

        // Monsters and actors whose job never arrived: "--" rather than "???".
        case game::Job::None:
        default:
            return {ICON_CIRCLE_DOT, NEUTRAL, "--"};
    }
}

} // namespace hub::common::ui
