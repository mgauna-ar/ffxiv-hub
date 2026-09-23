#include "app/ui/combat_deaths.hpp"
#include "app/ui/combat_view_common.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "hub/game/status.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

/// A death is picked by who died and when, since the list is rebuilt on every snapshot.
meter::EntityId s_selected_entity = 0;
double s_selected_time = -1.0;

bool is_selected(const meter::DeathRecord& death) {
    return death.entity == s_selected_entity && death.time_s == s_selected_time;
}

std::string pull_clock(double seconds) {
    const auto total = static_cast<unsigned>(std::max(seconds, 0.0));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u:%02u", total / 60, total % 60);
    return buf;
}

/// Debuff names the player had when they died; `limit` caps how many are listed.
std::string debuffs_at_death(const meter::DeathRecord& death, size_t limit) {
    std::string out;
    size_t listed = 0;
    size_t total = 0;
    for (uint8_t i = 0; i < death.status_count; ++i) {
        if (!game::status_is_detrimental(death.statuses[i])) continue;
        ++total;
        if (listed < limit) {
            if (!out.empty()) out += ", ";
            out += status_label(death.statuses[i]);
            ++listed;
        }
    }
    if (total > listed) out += " +" + std::to_string(total - listed);
    return out;
}

std::string buffs_at_death(const meter::DeathRecord& death) {
    std::string out;
    for (uint8_t i = 0; i < death.status_count; ++i) {
        if (game::status_is_detrimental(death.statuses[i])) continue;
        if (!out.empty()) out += ", ";
        out += status_label(death.statuses[i]);
    }
    return out;
}

void render_death_log(const meter::EncounterSummary& summary, const SummaryNames& names, float height) {
    const auto sizing = table_sizing(760.0f, kCombatTableFlags);
    if (!ImGui::BeginTable("##DeathLog", 7, sizing.flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, m(50.0f));
    ImGui::TableSetupColumn("Player", sizing.flex_flags(), sizing.flex_width(140.0f, 1.0f));
    ImGui::TableSetupColumn("Killing blow", sizing.flex_flags(), sizing.flex_width(140.0f, 1.0f));
    ImGui::TableSetupColumn("Source", sizing.flex_flags(), sizing.flex_width(110.0f, 0.8f));
    ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, m(80.0f));
    ImGui::TableSetupColumn("Debuffs", sizing.flex_flags(), sizing.flex_width(150.0f, 1.2f));
    ImGui::TableSetupColumn("Raised", ImGuiTableColumnFlags_WidthFixed, m(70.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();

    for (size_t i = 0; i < summary.deaths.size(); ++i) {
        const meter::DeathRecord& death = summary.deaths[i];
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        const std::string label = pull_clock(death.time_s) + "##Death" + std::to_string(i);
        const bool selected = is_selected(death);
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
            s_selected_entity = selected ? 0 : death.entity;
            s_selected_time = selected ? -1.0 : death.time_s;
        }

        ImGui::TableSetColumnIndex(1);
        const game::Job job = names.job(death.entity);
        text_colored_u32(job != game::Job::None ? get_job_color_u32(job) : colors::TextBody, "%s",
                         names.name(death.entity).c_str());

        const meter::RecapEvent* blow = death.killing_blow >= 0 ? &death.recap[death.killing_blow] : nullptr;
        ImGui::TableSetColumnIndex(2);
        text_colored_u32(blow ? colors::TextBody : colors::TextFaint, "%s",
                         blow ? ability_label(blow->action_key).c_str() : "Unknown");
        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextMuted, "%s", blow ? names.name(blow->source).c_str() : "-");
        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::DangerLight, "%s", blow ? format_damage(blow->amount).c_str() : "-");

        ImGui::TableSetColumnIndex(5);
        const std::string debuffs = debuffs_at_death(death, 3);
        text_colored_u32(debuffs.empty() ? colors::TextFaint : colors::WarningLight, "%s",
                         debuffs.empty() ? "-" : debuffs.c_str());
        if (ImGui::IsItemHovered() && death.status_count > 0) {
            const std::string all_debuffs = debuffs_at_death(death, meter::MAX_DEATH_STATUSES);
            const std::string buffs = buffs_at_death(death);
            ImGui::SetTooltip("Debuffs: %s\nBuffs: %s", all_debuffs.empty() ? "none" : all_debuffs.c_str(),
                              buffs.empty() ? "none" : buffs.c_str());
        }

        ImGui::TableSetColumnIndex(6);
        if (death.raised_after_s >= 0.0) {
            text_colored_u32(colors::SuccessLight, "%s", format_seconds(death.raised_after_s).c_str());
        } else {
            text_colored_u32(colors::TextFaint, "%s", "-");
        }
    }
    ImGui::EndTable();
}

const char* recap_kind_label(meter::RecapKind kind) {
    switch (kind) {
        case meter::RecapKind::Damage:  return "Hit";
        case meter::RecapKind::Heal:    return "Heal";
        case meter::RecapKind::DotTick: return "DoT";
        case meter::RecapKind::HotTick: return "HoT";
        default:                        return "?";
    }
}

bool recap_is_heal(meter::RecapKind kind) {
    return kind == meter::RecapKind::Heal || kind == meter::RecapKind::HotTick;
}

void render_recap(const meter::DeathRecord& death, const SummaryNames& names, float height) {
    const std::string heading = "LAST HITS - " + names.name(death.entity);
    section_header(ICON_SKULL, heading.c_str(), colors::Danger);
    if (death.recap_count == 0) {
        empty_state(ICON_SKULL, "No recap", "Nothing was seen landing on this player before they died.");
        return;
    }

    const auto sizing = table_sizing(560.0f, kCombatTableFlags);
    if (!ImGui::BeginTable("##DeathRecap", 5, sizing.flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthFixed, m(70.0f));
    ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthFixed, m(55.0f));
    ImGui::TableSetupColumn("Ability", sizing.flex_flags(), sizing.flex_width(160.0f, 1.2f));
    ImGui::TableSetupColumn("Source", sizing.flex_flags(), sizing.flex_width(120.0f, 1.0f));
    ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, m(100.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();

    for (uint8_t i = 0; i < death.recap_count; ++i) {
        const meter::RecapEvent& event = death.recap[i];
        const bool heal = recap_is_heal(event.kind);
        const bool killing_blow = static_cast<int>(i) == death.killing_blow;
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        text_colored_u32(colors::TextDim, "%s", format_seconds(event.offset_s).c_str());
        ImGui::TableSetColumnIndex(1);
        text_colored_u32(heal ? colors::SuccessLight : colors::DangerLight, "%s", recap_kind_label(event.kind));
        ImGui::TableSetColumnIndex(2);
        if (killing_blow) ImGui::PushFont(bold_font());
        text_colored_u32(colors::TextBody, "%s", ability_label(event.action_key).c_str());
        if (killing_blow) ImGui::PopFont();
        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextMuted, "%s", names.name(event.source).c_str());
        ImGui::TableSetColumnIndex(4);
        const bool crit = (event.hit_flags & meter::HitFlags::Crit) != 0;
        const bool dh = (event.hit_flags & meter::HitFlags::DirectHit) != 0;
        text_colored_u32(heal ? colors::SuccessLight : colors::DangerLight, "%s%s%s%s", heal ? "+" : "-",
                         format_damage(event.amount).c_str(), crit ? " !" : "", dh ? " DH" : "");
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    text_colored_u32(colors::TextDim, "%s", format_seconds(0.0).c_str());
    ImGui::TableSetColumnIndex(1);
    text_colored_u32(colors::Danger, "%s", ICON_SKULL);
    ImGui::TableSetColumnIndex(2);
    text_colored_u32(colors::TextPrimary, "%s", "Died");
    ImGui::EndTable();
}

} // namespace

void render_deaths(const meter::EncounterSummary& summary, float height, bool tracking_on) {
    if (summary.deaths.empty()) {
        vitals_empty_state(tracking_on, ICON_SKULL, "No deaths",
                           "Deaths show here with their killing blow and the hits before them.");
        return;
    }

    const SummaryNames names(summary);
    const meter::DeathRecord* selected = nullptr;
    for (const meter::DeathRecord& death : summary.deaths) {
        if (is_selected(death)) {
            selected = &death;
            break;
        }
    }
    if (selected == nullptr) {
        s_selected_entity = 0;
        s_selected_time = -1.0;
    }

    const float row_h = ImGui::GetTextLineHeight() + m(10.0f);
    const float log_h = selected != nullptr
        ? std::min(height * 0.45f, (static_cast<float>(summary.deaths.size()) + 1.5f) * row_h)
        : height;
    render_death_log(summary, names, log_h);
    if (selected == nullptr) return;

    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    render_recap(*selected, names, fill_h(0.0f));
}

#endif // HAVE_IMGUI

} // namespace hub::app::ui
