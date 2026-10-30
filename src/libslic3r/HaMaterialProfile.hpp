#pragma once
#include "HaMaterialProfilePayload.hpp"
#include "Preset.hpp"
#include "Config.hpp"
#include "PrintConfig.hpp"
#include <algorithm>
#include <cstddef>
#include <exception>

namespace Slic3r::HaMaterialProfile {
inline const Preset* stored_preset(const PresetCollection& presets, const std::string& name)
{
    // find_preset normally returns the selected edit buffer. HA identity and
    // initial uploads must use the saved baseline without consuming local edits.
    for (const auto& preset : presets.get_presets())
        if (preset.name == name) return &preset;
    return nullptr;
}
inline std::set<std::string> setting_keys()
{
    const auto& options = Preset::filament_options();
    std::set<std::string> result(options.begin(), options.end());
    result.erase("inherits");
    result.erase("filament_settings_id");
    return result;
}

// Preset.config already contains inherited effective values. Keep dependency
// provenance separately, so receiving clients do not need the original parent.
inline Json capture(const Preset& preset, const Json& authority = nullptr)
{
    if (preset.type != Preset::TYPE_FILAMENT)
        throw std::runtime_error("Only filament material profiles can be synchronized");
    Json settings = Json::object();
    for (const auto& key : setting_keys()) {
        if (!preset.config.option(key))
            throw std::runtime_error("The material profile is incomplete: " + key);
        settings[key] = preset.config.opt_serialize(key);
    }
    const auto* inherits = preset.config.option<ConfigOptionString>("inherits");
    Json deps = {{"inherits", inherits ? inherits->value : ""}, {"filament_id", preset.filament_id},
                 {"vendor", preset.vendor ? preset.vendor->id : ""}};
    std::string name = preset.name;
    if (!authority.is_null()) {
        validate_summary(authority, authority.contains("settings"));
        // A project keeps its local preset name after an explicit upload. Its
        // content suffix can therefore precede the current HA revision without
        // turning the managed display name into a new material association.
        const auto current_name = managed_name(authority.contains("settings") ? profile_summary(authority) : authority);
        const auto prefix = current_name.substr(0, current_name.size() - 17);
        if (is_managed_name(name) || (name.size() == prefix.size() + 17 && name.compare(0, prefix.size(), prefix) == 0 &&
            name.back() == ']' && name.find_first_not_of("0123456789abcdef", prefix.size()) == name.size() - 1)) {
            name = authority.at("name");
            deps = authority.at("dependencies");
        }
    }
    auto captured = seal({{"schema_version", 1}, {"name", name},
        {"material_type", preset.config.opt_string("filament_type", 0u)},
        {"settings", std::move(settings)}, {"dependencies", std::move(deps)}});
    if (is_managed_name(preset.name) || !authority.is_null()) {
        bool defaults_only = true;
        for (const auto& key : beta_toolchange_setting_keys())
            defaults_only = defaults_only && preset.config.option(key)->is_nil();
        if (defaults_only) {
            auto legacy = captured;
            for (const auto& key : beta_toolchange_setting_keys()) legacy["settings"].erase(key);
            legacy = seal(std::move(legacy));
            const auto canonical = managed_name(legacy);
            const bool same_identity = preset.name == canonical ||
                (is_managed_project_name(preset.name) && preset.name.compare(0, canonical.size(), canonical) == 0);
            // Preserve the published legacy identity only when the effective
            // settings still match it. Explicit Beta overrides are never omitted.
            if (same_identity || (!authority.is_null() && digest(legacy) == digest(authority)))
                return legacy;
        }
    }
    return captured;
}

// Native controls may expand a broadcast value without changing effective settings.
// Accept that equivalence only against content verified by its canonical HA identity.
inline bool matches_managed_baseline(const Preset& effective, const Preset& baseline)
{
    if (effective.type != Preset::TYPE_FILAMENT || baseline.type != Preset::TYPE_FILAMENT ||
        !is_managed_name(baseline.name) || is_managed_project_name(baseline.name) ||
        (effective.name != baseline.name &&
         !(is_managed_project_name(effective.name) && effective.name.substr(0, 78) == baseline.name)) ||
        effective.filament_id != baseline.filament_id)
        return false;
    try {
        if (managed_name(capture(baseline)) != baseline.name) return false;
        for (const auto &key : setting_keys()) {
            const auto *current = effective.config.option(key);
            const auto *original = baseline.config.option(key);
            if (!current || !original) return false;
            if (current->serialize() == original->serialize()) continue;
            const auto *current_vector = dynamic_cast<const ConfigOptionVectorBase *>(current);
            const auto *original_vector = dynamic_cast<const ConfigOptionVectorBase *>(original);
            if (key == "filament_extruder_variant" || !filament_options_with_variant.count(key) ||
                !current_vector || !original_vector || current_vector->size() == 0 || original_vector->size() == 0)
                return false;
            const auto current_values = current_vector->vserialize();
            const auto original_values = original_vector->vserialize();
            for (size_t i = 0; i < std::max(current_values.size(), original_values.size()); ++i)
                if (current_values[i < current_values.size() ? i : 0] !=
                    original_values[i < original_values.size() ? i : 0])
                    return false;
        }
        return true;
    } catch (const std::exception &) {
        // Missing or unrecognized schema fields never justify discarding edits.
        return false;
    }
}

inline DynamicPrintConfig decode_config(const Json& payload)
{
    validate(payload);
    const auto keys = setting_keys();
    const auto& settings = payload.at("settings");
    if (!compatible_setting_keys(settings, keys))
        throw std::runtime_error("The HA material profile uses a different settings schema; resynchronize it with this Quack version");
    DynamicPrintConfig config;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (!keys.count(it.key()))
            throw std::runtime_error("Unknown or non-material setting in HA profile: " + it.key());
        config.set_deserialize_strict(it.key(), it.value().get<std::string>());
    }
    for (const auto& key : beta_toolchange_setting_keys())
        if (!settings.contains(key))
            config.set_deserialize_strict(key, "nil");
    if (config.opt_string("filament_type", 0u) != payload.at("material_type"))
        throw std::runtime_error("The HA profile material type does not match its settings");
    config.set_key_value("inherits", new ConfigOptionString());
    config.set_key_value("filament_settings_id", new ConfigOptionStrings{managed_name(payload)});
    return config;
}
} // namespace Slic3r::HaMaterialProfile
