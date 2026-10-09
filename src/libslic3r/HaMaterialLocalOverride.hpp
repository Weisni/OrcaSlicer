#pragma once
#include <optional>
#include <vector>
#include "nlohmann/json.hpp"

namespace Slic3r::HaMaterialLocalOverride {
inline std::optional<size_t> replacement_index(const std::vector<std::string>& before,
                                             const std::vector<std::string>& after,
                                             const std::string& edited_name)
{
    if (after.size() < before.size()) throw std::runtime_error("Project materials cannot disappear while retaining temporary settings");
    std::optional<size_t> index;
    for (size_t i = 0; i < before.size(); ++i)
        if (before[i] == edited_name && after[i] != edited_name) { index = i; break; }
    if (index) {
        // One edited preset supplies all project entries with that name. It
        // cannot retain distinct overrides over two different new bases.
        for (size_t i = 0; i < before.size(); ++i)
            if (before[i] == edited_name && after[i] != after[*index])
                throw std::runtime_error("These project filaments share temporary material settings. Load the same HA profile for all of them, or keep their current profile.");
        for (size_t i = 0; i < after.size(); ++i)
            if (after[i] == after[*index] && (i >= before.size() || before[i] != edited_name))
                throw std::runtime_error("The new HA profile is also used by another project filament. Keep distinct profile assignments to preserve temporary settings.");
    }
    return index;
}
inline nlohmann::json merge_settings(const nlohmann::json &base,
                                    const nlohmann::json &edited,
                                    nlohmann::json updated)
{
    if (!base.is_object() || !edited.is_object() || !updated.is_object())
        throw std::runtime_error("Invalid temporary material settings");
    for (auto setting = edited.begin(); setting != edited.end(); ++setting) {
        const auto previous = base.find(setting.key());
        if (previous != base.end() && updated.contains(setting.key()) && *previous != setting.value())
            updated[setting.key()] = setting.value();
    }
    return updated;
}
} // namespace Slic3r::HaMaterialLocalOverride
