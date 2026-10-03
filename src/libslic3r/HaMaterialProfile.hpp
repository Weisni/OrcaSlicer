#pragma once
#include "HaMaterialProfilePayload.hpp"
#include "Preset.hpp"

namespace Slic3r::HaMaterialProfile {
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
        const auto current_name = managed_name(authority);
        const auto prefix = current_name.substr(0, current_name.size() - 17);
        if (name.size() == prefix.size() + 17 && name.compare(0, prefix.size(), prefix) == 0 &&
            name.back() == ']' && name.find_first_not_of("0123456789abcdef", prefix.size()) == name.size() - 1) {
            name = authority.at("name");
            deps = authority.at("dependencies");
        }
    }
    return seal({{"schema_version", 1}, {"name", name},
        {"material_type", preset.config.opt_string("filament_type", 0u)},
        {"settings", std::move(settings)}, {"dependencies", std::move(deps)}});
}

inline DynamicPrintConfig decode_config(const Json& payload)
{
    validate(payload);
    const auto keys = setting_keys();
    const auto& settings = payload.at("settings");
    if (settings.size() != keys.size())
        throw std::runtime_error("The HA material profile uses a different settings schema; resynchronize it with this Quack version");
    DynamicPrintConfig config;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (!keys.count(it.key()))
            throw std::runtime_error("Unknown or non-material setting in HA profile: " + it.key());
        config.set_deserialize_strict(it.key(), it.value().get<std::string>());
    }
    if (config.opt_string("filament_type", 0u) != payload.at("material_type"))
        throw std::runtime_error("The HA profile material type does not match its settings");
    config.set_key_value("inherits", new ConfigOptionString());
    config.set_key_value("filament_settings_id", new ConfigOptionStrings{managed_name(payload)});
    return config;
}
} // namespace Slic3r::HaMaterialProfile
