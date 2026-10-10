#pragma once
#include "PresetBundle.hpp"
#include "HaMaterialBinding.hpp"

namespace Slic3r::HaMaterialLocalOverride {
inline bool has_local_edits(const Preset& preset)
{
    // Reopening a project clone makes it the editor's local baseline, but HA
    // still owns the committed content identified by its canonical name.
    return HaMaterialProfile::is_managed_project_name(preset.name) &&
        HaMaterialProfile::managed_name(HaMaterialProfile::capture(preset)) != preset.name.substr(0, 78);
}
inline bool has_local_edits(const PresetBundle& bundle)
{
    return has_local_edits(bundle.filaments.get_edited_preset());
}
inline const Preset* edit_baseline(const PresetBundle& bundle)
{
    const auto& name = bundle.filaments.get_edited_preset().name;
    return HaMaterialProfile::stored_preset(bundle.filaments,
        HaMaterialProfile::is_managed_project_name(name) ? name.substr(0, 78) : name);
}
inline std::string isolate_edit_slot(PresetBundle& bundle, size_t index)
{
    const auto name = bundle.filament_presets.at(index);
    const auto bindings = HaMaterialBinding::read(bundle.project_config, bundle.filament_presets.size());

    // A committed content-addressed preset can serve several physical rolls.
    // Keep each edit buffer in an in-memory project preset, never in that cache.
    auto& edited = bundle.filaments.get_edited_preset();
    if (edited.name == name && edited.is_project_embedded && HaMaterialProfile::is_managed_project_name(name) &&
        std::count(bundle.filament_presets.begin(), bundle.filament_presets.end(), name) == 1)
        return name;
    if (edited.is_project_embedded && HaMaterialProfile::is_managed_project_name(edited.name)) {
        auto* stored = bundle.filaments.find_preset(edited.name, false, true);
        if (stored) stored->config = edited.config;
    }
    if (bindings[index].spool_uuid.empty() || !HaMaterialProfile::is_managed_name(name)) return name;
    const auto* stored = HaMaterialProfile::stored_preset(bundle.filaments, name);
    if (!stored) throw std::runtime_error("The selected HA material preset is unavailable");
    if (stored->is_project_embedded && HaMaterialProfile::is_managed_project_name(name) &&
        std::count(bundle.filament_presets.begin(), bundle.filament_presets.end(), name) == 1)
        return name;

    // Copy before insertion because the preset collection uses a sorted deque.
    Preset local = *stored;
    const auto canonical = name.substr(0, 78); // "HA material [" + SHA-256 + "]"
    size_t instance = index + 1;
    do { local.name = canonical + " [project filament " + std::to_string(instance++) + "]"; }
    while (HaMaterialProfile::stored_preset(bundle.filaments, local.name));
    // The existing embedded-preset exporter/loader requires a parent. The
    // built-in default is always available even without this machine's HA cache;
    // authority capture retains the original publication dependency metadata.
    local.config.set_key_value("inherits", new ConfigOptionString(bundle.filaments.default_preset_for(local.config).name));
    local.config.set_key_value("filament_settings_id", new ConfigOptionStrings{local.name});
    auto& installed = bundle.filaments.load_preset({}, local.name, local.config, false, local.version);
    installed.alias = local.alias;
    installed.filament_id = local.filament_id;
    installed.is_project_embedded = true;
    installed.is_external = false;
    installed.is_compatible = local.is_compatible;
    bundle.set_filament_preset(index, local.name);
    return local.name;
}
}
