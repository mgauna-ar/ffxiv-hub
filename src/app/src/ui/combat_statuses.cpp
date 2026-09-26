#include "app/ui/combat_statuses.hpp"
#include "app/ui/combat_view_common.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

bool in_view(const meter::StatusUptimeRow& row, StatusView view) {
    switch (view) {
        case StatusView::PartyDebuffs: return !row.on_enemy && row.detrimental;
        case StatusView::PartyBuffs:   return !row.on_enemy && !row.detrimental;
        case StatusView::EnemyDebuffs: return row.on_enemy;
    }
    return false;
}

/// Every row of one status in the current view.
struct StatusGroup {
    uint16_t status{0};
    std::vector<const meter::StatusUptimeRow*> rows;
    uint32_t applications{0};
    double active_s{0.0};
    size_t targets{0};
    double uptime_pct{0.0};
};

std::vector<StatusGroup> group_rows(const meter::EncounterSummary& summary, StatusView view) {
    std::map<uint16_t, StatusGroup> groups;
    for (const meter::StatusUptimeRow& row : summary.statuses) {
        if (!in_view(row, view)) continue;
        StatusGroup& group = groups[row.status];
        group.status = row.status;
        group.rows.push_back(&row);
        group.applications += row.applications;
        group.active_s += row.active_s;
    }

    const double duration = std::max(summary.duration_seconds, 1.0);
    std::vector<StatusGroup> sorted;
    sorted.reserve(groups.size());
    for (auto& [status, group] : groups) {
        std::set<meter::EntityId> targets;
        double best = 0.0;
        for (const meter::StatusUptimeRow* row : group.rows) {
            targets.insert(row->target);
            best = std::max(best, row->uptime_pct);
        }
        group.targets = targets.size();
        // On the party, how long an affected player had it on average; on an enemy,
        // the best-kept application, which is what a DoT's uptime means.
        group.uptime_pct = view == StatusView::EnemyDebuffs
            ? best
            : std::min(group.active_s / (static_cast<double>(group.targets) * duration) * 100.0, 100.0);
        std::sort(group.rows.begin(), group.rows.end(),
                  [](const meter::StatusUptimeRow* a, const meter::StatusUptimeRow* b) {
                      return a->active_s > b->active_s;
                  });
        sorted.push_back(std::move(group));
    }
    // Debuffs on the party are mistakes, so the most frequent go first.
    std::sort(sorted.begin(), sorted.end(), [view](const StatusGroup& a, const StatusGroup& b) {
        if (view == StatusView::PartyDebuffs && a.applications != b.applications) {
            return a.applications > b.applications;
        }
        return a.active_s > b.active_s;
    });
    return sorted;
}

/// Sized to its label, like a tab, so all three stay on one line at the
/// window's minimum width.
void view_button(StatusesTabState& state, const char* label, StatusView view) {
    const bool active = state.view == view;
    if (button(label, active ? ButtonKind::Primary : ButtonKind::Secondary, ButtonSize::Fit)) {
        state.view = view;
    }
}

void render_group_row(const StatusGroup& group, const SummaryNames& names, StatusView view) {
    ImGui::TableNextRow();
    row_progress_bar(static_cast<float>(group.uptime_pct / 100.0),
                     view == StatusView::PartyBuffs ? colors::Success : colors::Danger);
    ImGui::TableSetColumnIndex(0);
    const std::string label = status_label(group.status) + "##Status" + std::to_string(group.status);
    const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);

    ImGui::TableSetColumnIndex(1);
    if (view == StatusView::EnemyDebuffs) {
        // Who applied it matters more on an enemy than how many enemies had it.
        std::set<meter::EntityId> sources;
        for (const meter::StatusUptimeRow* row : group.rows) sources.insert(row->source);
        std::string applied_by = names.name(*sources.begin());
        if (sources.size() > 1) applied_by += " +" + std::to_string(sources.size() - 1);
        text_colored_u32(colors::TextMuted, "%s", applied_by.c_str());
    } else {
        text_colored_u32(colors::TextMuted, "%zu", group.targets);
    }
    ImGui::TableSetColumnIndex(2);
    text_colored_u32(colors::TextBody, "%u", group.applications);
    ImGui::TableSetColumnIndex(3);
    text_colored_u32(colors::TextMuted, "%s", format_seconds(group.active_s).c_str());
    ImGui::TableSetColumnIndex(4);
    text_colored_u32(colors::TextPrimary, "%s", format_percentage(group.uptime_pct).c_str());

    if (!open) return;
    for (const meter::StatusUptimeRow* row : group.rows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Indent(m(18.0f));
        const game::Job job = names.job(row->target);
        text_colored_u32(job != game::Job::None ? get_job_color_u32(job) : colors::TextBody, "%s",
                         names.name(row->target).c_str());
        ImGui::Unindent(m(18.0f));
        ImGui::TableSetColumnIndex(1);
        text_colored_u32(colors::TextDim, "%s", names.name(row->source).c_str());
        ImGui::TableSetColumnIndex(2);
        text_colored_u32(colors::TextMuted, "%u", row->applications);
        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextMuted, "%s", format_seconds(row->active_s).c_str());
        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextBody, "%s", format_percentage(row->uptime_pct).c_str());
    }
    ImGui::TreePop();
}

} // namespace

void render_statuses(StatusesTabState& state, const meter::EncounterSummary& summary, float height,
                     bool tracking_on) {
    const float top = ImGui::GetCursorPosY();
    const char* buffs_label = ICON_SHIELD "  Buffs on party##StatusView1";
    const char* enemies_label = ICON_TARGET "  On enemies##StatusView2";
    view_button(state, ICON_WARNING "  Debuffs on party##StatusView0", StatusView::PartyDebuffs);
    same_line_if_room(button_width(buffs_label, ButtonSize::Fit));
    view_button(state, buffs_label, StatusView::PartyBuffs);
    same_line_if_room(button_width(enemies_label, ButtonSize::Fit));
    view_button(state, enemies_label, StatusView::EnemyDebuffs);
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    const float body_h = std::max(height - (ImGui::GetCursorPosY() - top), ImGui::GetTextLineHeight() * 3.0f);

    const std::vector<StatusGroup> groups = group_rows(summary, state.view);
    if (groups.empty()) {
        const char* title = state.view == StatusView::PartyDebuffs ? "No debuffs on the party"
                          : state.view == StatusView::PartyBuffs   ? "No buffs on the party"
                                                               : "Nothing on the enemies";
        vitals_empty_state(tracking_on, ICON_SPARKLE, title,
                           "Statuses are tracked while a pull runs, with how long each stayed up.");
        return;
    }

    const SummaryNames names(summary);
    const auto sizing = table_sizing(560.0f, 5, kCombatTableFlags);
    if (!ImGui::BeginTable("##StatusTable", 5, sizing.flags, ImVec2(0.0f, body_h))) return;
    ImGui::TableSetupColumn("Status", sizing.flex_flags(), sizing.flex_width(180.0f, 1.4f));
    ImGui::TableSetupColumn(state.view == StatusView::EnemyDebuffs ? "Applied by" : "Players",
                            sizing.flex_flags(), sizing.flex_width(110.0f, 0.8f));
    ImGui::TableSetupColumn("Applied", ImGuiTableColumnFlags_WidthFixed, m(70.0f));
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, m(80.0f));
    ImGui::TableSetupColumn("Uptime", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();
    for (const StatusGroup& group : groups) {
        render_group_row(group, names, state.view);
    }
    ImGui::EndTable();
}

#endif // HAVE_IMGUI

} // namespace hub::app::ui
