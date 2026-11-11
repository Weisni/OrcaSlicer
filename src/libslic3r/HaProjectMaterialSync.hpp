#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "Config.hpp"
#include "FilamentMixer.hpp"
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
    // Missing entries describe physical slots, including legacy project snapshots.
    std::vector<unsigned char> is_mixed;
};

// Compare the reviewed slots before normalization can change their indices.
inline std::vector<int> flushing_volume_updates(const Project &original, const Project &staged,
                                                const std::vector<Selection> &selections,
                                                const std::vector<std::vector<std::string>> &ams_colors,
                                                std::string_view auto_flush)
{
    std::vector<int> updates;
    if (auto_flush == "disabled") return updates;

    for (const auto &selection : selections) {
        const size_t index = selection.project_index;
        if (index >= original.presets.size()) {
            updates.push_back(int(index));
            continue;
        }
        bool color_changed = false;
        if (!selection.profile_only) {
            const auto &new_color = staged.colors[index];
            color_changed = index < ams_colors.size() && !ams_colors[index].empty()
                ? ams_colors[index].size() != 1 || ams_colors[index].front() != new_color
                : original.colors[index] != new_color;
        }
        if (color_changed || (auto_flush == "all" && original.presets[index] != staged.presets[index]))
            updates.push_back(int(index));
    }
    return updates;
}

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

inline void remap_material_references(DynamicPrintConfig &config, const std::map<int, int> &remap)
{
    for (const auto *key : {"support_filament", "support_interface_filament"}) {
        auto *option = config.option<ConfigOptionInt>(key);
        if (!option || option->value <= 0) continue;
        const auto moved = remap.find(option->value - 1);
        if (moved != remap.end()) option->value = moved->second + 1;
    }
}

// Return changed zero-based slot IDs for the caller's model/plate remapping.
// Physical slots keep their relative order and precede all virtual mixed slots.
inline std::map<int, int> normalize_slot_order(PresetBundle &bundle)
{
    const size_t count = bundle.filament_presets.size();
    const auto *mixed = bundle.project_config.option<ConfigOptionBools>("filament_is_mixed");
    std::vector<size_t> order;
    order.reserve(count);
    for (const bool want_mixed : {false, true})
        for (size_t i = 0; i < count; ++i)
            if (bool(mixed && i < mixed->values.size() && mixed->values[i]) == want_mixed)
                order.push_back(i);
    std::map<int, int> remap;
    for (size_t i = 0; i < count; ++i)
        if (order[i] != i) remap.emplace(int(order[i]), int(i));
    if (remap.empty()) return remap;

    // Stage the complete permutation so malformed parallel metadata cannot leave
    // even the caller's private bundle partially reordered.
    auto config = bundle.project_config;
    for (const auto *key : {"filament_is_mixed", "filament_mixed_gradient", "filament_mixed_gradient_per_part"})
        config.option<ConfigOptionBools>(key, true)->values.resize(count, false);
    for (const auto *key : {"filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_gradient_range",
                            "filament_mixed_gradient_curve", "ha_material_bindings"})
        config.option<ConfigOptionStrings>(key, true)->values.resize(count);
    for (const auto *key : {"filament_is_mixed", "filament_mixed_components", "filament_mixed_sublayer_ratios",
                            "filament_mixed_gradient", "filament_mixed_gradient_range", "filament_mixed_gradient_curve",
                            "filament_mixed_gradient_per_part", "filament_colour", "filament_multi_colour",
                            "filament_colour_type", "filament_map", "filament_nozzle_map", "filament_volume_map",
                            "ha_material_bindings"}) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (!option || option->size() != count)
            throw std::runtime_error("Project filament metadata does not match its material slots");
        const std::unique_ptr<ConfigOption> original(option->clone());
        for (size_t i = 0; i < count; ++i) option->set_at(original.get(), i, order[i]);
    }
    auto *components = config.option<ConfigOptionStrings>("filament_mixed_components");
    const auto *ordered_mixed = config.option<ConfigOptionBools>("filament_is_mixed");
    for (size_t i = 0; i < count; ++i) {
        if (!ordered_mixed->values[i]) continue;
        auto ids = parse_mixed_components(components->values[i]);
        bool changed = false;
        for (auto &id : ids) {
            if (id < 1 || id > count) continue;
            const auto moved = remap.find(int(id - 1));
            if (moved != remap.end()) { id = unsigned(moved->second + 1); changed = true; }
        }
        if (changed) components->values[i] = format_mixed_components(ids);
    }

    // Each nozzle has a contiguous NxN purge matrix; the input/output vector
    // instead stores one adjacent pair per material. Nozzle settings stay put.
    if (auto *matrix = config.option<ConfigOptionFloats>("flush_volumes_matrix")) {
        const auto original = matrix->values;
        const size_t block_size = count * count;
        if (original.size() % block_size != 0)
            throw std::runtime_error("Project purge matrix does not match its material slots");
        for (size_t block = 0; block < original.size(); block += block_size)
            for (size_t row = 0; row < count; ++row)
                for (size_t col = 0; col < count; ++col)
                    matrix->values[block + row * count + col] = original[block + order[row] * count + order[col]];
    }
    if (auto *volumes = config.option<ConfigOptionFloats>("flush_volumes_vector"); volumes && !volumes->values.empty()) {
        const auto original = volumes->values;
        if (original.size() != count * 2)
            throw std::runtime_error("Project purge volumes do not match its material slots");
        for (size_t i = 0; i < count; ++i)
            for (size_t direction = 0; direction < 2; ++direction)
                volumes->values[2 * i + direction] = original[2 * order[i] + direction];
    }
    auto presets = bundle.filament_presets;
    auto ams_colors = bundle.ams_multi_color_filment;
    ams_colors.resize(count);
    const auto original_ams_colors = ams_colors;
    for (size_t i = 0; i < count; ++i) {
        presets[i] = bundle.filament_presets[order[i]];
        ams_colors[i] = original_ams_colors[order[i]];
    }
    remap_material_references(config, remap);
    bundle.project_config.swap(config);
    bundle.filament_presets.swap(presets);
    bundle.ams_multi_color_filment.swap(ams_colors);
    return remap;
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
        if (selection.project_index < project.presets.size() &&
            selection.project_index < project.is_mixed.size() && project.is_mixed[selection.project_index])
            throw std::runtime_error("A physical HA roll cannot replace a virtual mixed filament; select a physical project filament");
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
