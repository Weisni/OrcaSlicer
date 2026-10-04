#pragma once

#include <cstdint>
#include "HaMaterialSource.hpp"
#include "HaInventorySelection.hpp"
#include "PresetBundle.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r::HaProjectMaterialSync {

struct Row {
    HaMaterialSource::Assignment assignment;
    std::string resolved_preset;
    std::string resolved_config;
    int64_t remaining_mg = 0;
    int64_t nominal_mg = 1000000;
    bool local_only = false;
};

struct Selection {
    size_t source_index;
    size_t project_index;
    bool color_only;
    bool profile_only = false;
};

struct Project {
    std::vector<std::string> presets, colors;
    std::vector<nlohmann::json> profiles;
};

inline std::vector<Selection> all_occupied_selections(const std::vector<Row> &rows)
{
    std::vector<Selection> selections;
    for (size_t i = 0; i < rows.size(); ++i)
        if (!rows[i].assignment.slot.empty() && !rows[i].assignment.spool_uuid.empty())
            selections.push_back({i, selections.size(), false});
    return selections;
}

inline void extend_project(PresetBundle &bundle, const Project &staged)
{
    const size_t old_count = bundle.filament_presets.size();
    if (staged.presets.empty() || staged.presets.size() != staged.colors.size() || staged.presets.size() < old_count)
        throw std::runtime_error("Project material extension must preserve all existing indices");
    if (staged.presets.size() == old_count) return;
    if (staged.presets.size() > size_t(EnforcerBlockerType::ExtruderMax))
        throw std::runtime_error("The project cannot append more materials than its painting format supports");
    const auto *colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    const auto *types = bundle.project_config.option<ConfigOptionStrings>("filament_colour_type");
    const auto *multi = bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour");
    if (!colors || !types || !multi || colors->values.size() != old_count ||
        types->values.size() != old_count || multi->values.size() != old_count)
        throw std::runtime_error("Project filament color metadata is inconsistent");
    const auto old_types = types->values;
    const auto old_multi = multi->values;
    bundle.set_num_filaments(unsigned(staged.presets.size()),
                             std::vector<std::string>(staged.colors.begin() + old_count, staged.colors.end()));
    // set_num_filaments normalizes every multicolor value, including untouched rows.
    std::copy(old_types.begin(), old_types.end(), bundle.project_config.option<ConfigOptionStrings>("filament_colour_type")->values.begin());
    std::copy(old_multi.begin(), old_multi.end(), bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values.begin());
}

// Stage only explicit mappings. Existing indices and unchecked materials remain
// stable. New full materials may be appended without remapping object/plate indices.
inline Project stage(const Project &project, const std::vector<Row> &reviewed,
                     const std::vector<Row> &fresh, const std::vector<Selection> &selections)
{
    if (project.presets.empty() || project.presets.size() != project.colors.size())
        throw std::runtime_error("Project filament colors and presets do not match");
    Project result = project;
    std::set<size_t> sources, targets;
    for (const auto &selection : selections) {
        if (selection.color_only && selection.profile_only)
            throw std::runtime_error("Select profile, color or both");
        if (selection.source_index >= reviewed.size())
            throw std::runtime_error("Select an available HA roll for every checked project material");
        if (reviewed[selection.source_index].local_only)
            throw std::runtime_error("Publish this local roll to HA before importing it");
        if (reviewed[selection.source_index].assignment.spool_uuid.empty())
            throw std::runtime_error("An empty HA slot has no material to import");
        if (selection.project_index >= project.presets.size() &&
            (selection.color_only || selection.profile_only))
            throw std::runtime_error("New project materials require both profile and color");
        if (!sources.insert(selection.source_index).second || !targets.insert(selection.project_index).second)
            throw std::runtime_error("Each HA roll and project filament may be mapped only once");
    }
    size_t count = project.presets.size();
    for (const auto target : targets) {
        if (target < project.presets.size()) continue;
        if (target != count++)
            throw std::runtime_error("Append new project materials without gaps");
    }
    result.presets.resize(count);
    result.colors.resize(count);
    for (const auto &selection : selections) {
        const auto &row = reviewed[selection.source_index];
        const auto &a = row.assignment;
        const auto current = std::find_if(fresh.begin(), fresh.end(), [&a](const Row &candidate) {
            return candidate.assignment.slot == a.slot && candidate.assignment.spool_uuid == a.spool_uuid;
        });
        if (current == fresh.end() || current->local_only)
            throw std::runtime_error("HA slot assignment changed during review; reopen synchronization");
        const auto &b = current->assignment;
        if (a.revision != b.revision || a.material_type != b.material_type || a.color != b.color ||
            a.material_preset != b.material_preset || a.manufacturer != b.manufacturer || a.product != b.product ||
            row.resolved_preset != current->resolved_preset || row.resolved_config != current->resolved_config)
            throw std::runtime_error("HA material or compatible preset changed during review; reopen synchronization");
        if (!selection.color_only) {
            if (row.resolved_preset.empty())
                throw std::runtime_error("No compatible same-type material preset; select color only or install a compatible preset");
            result.presets[selection.project_index] = row.resolved_preset;
        }
        if (!selection.profile_only)
            result.colors[selection.project_index] = a.color;
    }
    return result;
}

// Publish project metadata only. Stock, identity, lifecycle and ledger rows
// cannot enter this payload, even for an all-material operation.
inline nlohmann::json save_changes(const Project &project, const std::vector<Row> &reviewed,
                                  const std::vector<Row> &fresh, const std::vector<Selection> &selections,
                                  const nlohmann::json &remote)
{
    auto validation = selections;
    for (auto &selection : validation) {
        if (selection.color_only && selection.profile_only)
            throw std::runtime_error("Select profile, color or both");
        selection.color_only = true;
        selection.profile_only = false;
    }
    stage(project, reviewed, fresh, validation);
    nlohmann::json changes = nlohmann::json::array();
    for (const auto &selection : selections) {
        const auto &id = reviewed.at(selection.source_index).assignment.spool_uuid;
        const auto *target = HaInventorySelection::spool(remote, id);
        if (!target) throw std::runtime_error("The selected HA roll disappeared; reopen synchronization");
        nlohmann::json change = {{"spool_uuid", id}, {"fields", nlohmann::json::object()},
                                 {"expected", nlohmann::json::object()}};
        const auto add = [&](const std::string &field, const std::string &value) {
            if (target->at(field) != value) {
                change["fields"][field] = value;
                change["expected"][field] = target->at(field);
            }
        };
        if (!selection.color_only) add("filament_preset_id", project.presets.at(selection.project_index));
        if (!selection.profile_only) add("color_hex", project.colors.at(selection.project_index));
        if (!change["fields"].empty()) changes.push_back(std::move(change));
    }
    return changes;
}

} // namespace Slic3r::HaProjectMaterialSync
