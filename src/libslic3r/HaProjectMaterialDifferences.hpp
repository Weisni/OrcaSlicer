#pragma once

#include <cctype>
#include <map>

#include "HaMaterialBinding.hpp"
#include "HaProjectRollPublish.hpp"

namespace Slic3r::HaProjectMaterialDifferences {

enum class Field { Profile, Color };
struct Difference {
    size_t source_index, project_index;
    Field field;
    std::string before, after, problem;
    std::string before_profile_sha256, after_profile_sha256;
};

namespace detail {
inline std::string canonical_color(std::string color)
{
    HaProjectRollPublish::require_color(color);
    for (char &c : color) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return color;
}

inline bool unchanged_fallback(const HaProjectMaterialSync::Row &row, const std::string &preset)
{
    if (preset != row.resolved_preset || preset == row.assignment.material_preset || row.assignment.material_type.empty())
        return false;
    const auto prefix = "Generic " + row.assignment.material_type;
    return preset.compare(0, prefix.size(), prefix) == 0 &&
           (preset.size() == prefix.size() || preset[prefix.size()] == ' ' || preset[prefix.size()] == '@');
}
} // namespace detail

// The physical UUID is the comparison key. Multiple project filaments may use
// one roll, but they must agree on each field before that field can be sent.
inline std::vector<Difference> compare(const HaProjectMaterialSync::Project &project,
    const std::vector<HaProjectMaterialSync::Row> &rows, const std::vector<HaMaterialBinding::Binding> &bindings,
    const std::string &endpoint, const std::string &printer_id)
{
    if (project.presets.size() != project.colors.size())
        throw std::runtime_error("Project filament colors and presets do not match");
    if (!project.profiles.empty() && project.profiles.size() != project.presets.size())
        throw std::runtime_error("Project material profiles do not match its filaments");
    if (endpoint.empty() || printer_id.empty()) return {};

    struct Group { size_t source_index; std::vector<size_t> project_indices; };
    std::vector<Group> groups;
    std::map<std::string, size_t> by_uuid;
    for (size_t i = 0; i < project.presets.size() && i < bindings.size(); ++i) {
        const auto &binding = bindings[i];
        if (binding.source != endpoint || binding.printer_id != printer_id || binding.spool_uuid.empty()) continue;
        const auto found = by_uuid.find(binding.spool_uuid);
        if (found != by_uuid.end()) {
            groups[found->second].project_indices.push_back(i);
            continue;
        }
        for (size_t source = 0; source < rows.size(); ++source) {
            if (rows[source].local_only || rows[source].assignment.spool_uuid != binding.spool_uuid) continue;
            by_uuid.emplace(binding.spool_uuid, groups.size());
            groups.push_back({source, {i}});
            break;
        }
    }

    std::vector<Difference> result;
    for (const auto &group : groups) {
        const auto &row = rows[group.source_index];
        const size_t first = group.project_indices.front();
        for (const auto field : {Field::Profile, Field::Color}) {
            const auto &values = field == Field::Profile ? project.presets : project.colors;
            const auto &baseline = field == Field::Profile ? row.assignment.material_preset : row.assignment.color;
            Difference difference{group.source_index, first, field, baseline, values[first], {}};
            try {
                if (field == Field::Profile && !project.profiles.empty()) {
                    const auto &profile = project.profiles.at(first);
                    HaMaterialProfile::validate(profile);
                    difference.after = profile.at("name").get<std::string>();
                    difference.before_profile_sha256 = HaMaterialProfile::digest(row.assignment.material_profile);
                    difference.after_profile_sha256 = HaMaterialProfile::digest(profile);
                    for (const auto index : group.project_indices) {
                        HaMaterialProfile::validate(project.profiles.at(index));
                        if (HaMaterialProfile::digest(project.profiles.at(index)) != difference.after_profile_sha256) {
                            difference.problem = "This roll has conflicting project material settings; choose the same profile and settings for every use of the roll.";
                            break;
                        }
                    }
                    if (difference.problem.empty() && difference.before_profile_sha256 == difference.after_profile_sha256)
                        continue;
                    // A legacy HA association has no settings yet. Offer the
                    // complete profile explicitly, even when its name matches.
                    result.push_back(std::move(difference));
                    continue;
                }
                const auto normalized = [&](const std::string &value) {
                    if (field == Field::Color) return detail::canonical_color(value);
                    HaProjectRollPublish::require_text(value, 256);
                    return value;
                };
                const auto after = normalized(values[first]);
                for (const auto index : group.project_indices) {
                    if (normalized(values[index]) != after) {
                        difference.problem = field == Field::Profile
                            ? "This roll has conflicting project profiles; choose the same profile for every use of the roll."
                            : "This roll has conflicting project colors; choose the same color for every use of the roll.";
                        break;
                    }
                }
                if (difference.problem.empty()) {
                    if (field == Field::Profile) {
                        if (values[first] == baseline || detail::unchanged_fallback(row, values[first])) continue;
                    } else if (after == detail::canonical_color(baseline)) continue;
                }
            } catch (const std::exception &error) {
                difference.problem = error.what();
            }
            result.push_back(std::move(difference));
        }
    }
    return result;
}

// Selections preserve HA stock exactly. The publication caller must retain its
// fresh-baseline guard before constructing a metadata-only network request.
inline std::vector<HaProjectRollPublish::RollPublish> selected_rolls(const std::vector<Difference> &differences,
    const std::vector<bool> &checked, const std::vector<HaProjectMaterialSync::Row> &rows)
{
    if (differences.size() != checked.size()) throw std::runtime_error("The selected comparison fields changed; refresh the comparison");
    struct Selection {
        HaProjectRollPublish::RollPublish roll;
        std::map<Field, std::string> values;
    };
    std::vector<Selection> selections;
    std::map<std::string, size_t> by_uuid;
    for (size_t i = 0; i < differences.size(); ++i) {
        if (!checked[i]) continue;
        const auto &difference = differences[i];
        if (!difference.problem.empty()) throw std::runtime_error(difference.problem);
        if (difference.source_index >= rows.size()) throw std::runtime_error("The HA roll disappeared; refresh the comparison");
        const auto &row = rows[difference.source_index];
        if (row.local_only || !HaProjectRollPublish::canonical_uuid(row.assignment.spool_uuid))
            throw std::runtime_error("Select an existing HA physical roll");
        if (difference.field != Field::Profile && difference.field != Field::Color)
            throw std::runtime_error("Select a valid profile or color field");
        const bool profile = difference.field == Field::Profile;
        const auto normalize = [&](const std::string &value) {
            if (!profile) return detail::canonical_color(value);
            HaProjectRollPublish::require_text(value, 256, true);
            return value;
        };
        const auto &baseline = profile ? row.assignment.material_preset : row.assignment.color;
        if (normalize(difference.before) != normalize(baseline))
            throw std::runtime_error("The HA profile or color changed; refresh the comparison");
        if (profile && !difference.after_profile_sha256.empty() &&
            difference.before_profile_sha256 != HaMaterialProfile::digest(row.assignment.material_profile))
            throw std::runtime_error("The HA material settings changed; refresh the comparison");
        if (profile) HaProjectRollPublish::require_text(difference.after, 256);
        const auto after = normalize(difference.after);
        const auto inserted = by_uuid.emplace(row.assignment.spool_uuid, selections.size());
        if (inserted.second) {
            HaProjectRollPublish::RollPublish roll;
            roll.project_index = difference.project_index;
            roll.uuid = row.assignment.spool_uuid;
            roll.remaining_mg = HaProjectRollPublish::checked_weight(row.remaining_mg);
            selections.push_back({std::move(roll), {}});
        }
        auto &selection = selections[inserted.first->second];
        if (profile) selection.roll.expected_profile_sha256 = difference.before_profile_sha256;
        if (selection.roll.remaining_mg != row.remaining_mg)
            throw std::runtime_error("The HA stock baseline changed; refresh the comparison");
        const auto existing = selection.values.find(difference.field);
        if (existing != selection.values.end()) {
            if (existing->second != after) throw std::runtime_error("Selected project values conflict for the same physical roll");
            continue;
        }
        if (!selection.values.empty() && selection.roll.project_index != difference.project_index)
            throw std::runtime_error("Selected fields need one consistent project filament for this physical roll");
        selection.values.emplace(difference.field, after);
    }
    if (selections.size() > 100) throw std::runtime_error("Select at most 100 physical rolls");
    std::vector<HaProjectRollPublish::RollPublish> result;
    for (auto &selection : selections) {
        selection.roll.color_only = selection.values.count(Field::Profile) == 0;
        selection.roll.profile_only = selection.values.count(Field::Color) == 0;
        result.push_back(std::move(selection.roll));
    }
    return result;
}
} // namespace Slic3r::HaProjectMaterialDifferences
