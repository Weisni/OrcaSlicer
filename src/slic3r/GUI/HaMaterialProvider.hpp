#pragma once

#include "HaInventoryExplicit.hpp"
#include "HaInventoryAuthority.hpp"
#include "libslic3r/HaMaterialCatalog.hpp"
#include "libslic3r/HaMaterialBinding.hpp"
#include "libslic3r/HaMaterialRefresh.hpp"
#include "libslic3r/HaMaterialProfile.hpp"
#include <mutex>

namespace Slic3r::GUI::HaMaterialProvider {
using Json = nlohmann::json;
inline bool enabled() { return wxGetApp().app_config->get_bool("ha_material_provider_enabled"); }
inline std::string endpoint() { return wxGetApp().app_config->get("ha_material_demo_endpoint"); }
inline std::string physical_device_id() { return wxGetApp().app_config->get("ha_material_provider_device_id"); }
struct Cache {
    std::mutex mutex;
    Json data;
    std::string source, error;
    HaMaterialRefresh::DisplayRevision display;
    std::chrono::steady_clock::time_point attempted{};
};
inline Cache &cache() { static Cache value; return value; }
inline Json snapshot() {
    auto &c = cache(); std::lock_guard<std::mutex> lock(c.mutex);
    return enabled() && c.source == endpoint() ? c.data : Json();
}
inline std::string last_error() {
    auto &c = cache(); std::lock_guard<std::mutex> lock(c.mutex); return c.error;
}
inline std::uint64_t display_generation() {
    auto &c = cache(); std::lock_guard<std::mutex> lock(c.mutex); return c.display.generation();
}
inline Json fresh_snapshot() {
    if (!enabled()) throw std::runtime_error("Enable the Home Assistant material source first");
    const auto source = endpoint();
    auto result = ha_inventory_material_snapshot(source, false);
    validate_ha_material_snapshot(result);
    if (result.value("provider_api_version", 0) != 1 || result.value("provider_printer_id", std::string()).empty() ||
        result.at("provider_printer_id") != physical_device_id())
        throw std::runtime_error("Connect the HA material source to its configured physical printer first");
    HaMaterialCatalog::available(result);
    if (source != endpoint() || !enabled()) throw std::runtime_error("The HA material source changed during refresh");
    result = HaInventoryAuthority::refresh(result);
    if (source != endpoint() || !enabled() || result.value("provider_api_version", 0) != 1 ||
        result.value("provider_printer_id", std::string()) != physical_device_id())
        throw std::runtime_error("The HA material source or its physical printer changed during refresh");
    HaMaterialCatalog::available(result);
    auto &c = cache(); std::lock_guard<std::mutex> lock(c.mutex);
    c.source = source; c.data = result; c.data.erase("native_bundle"); c.data.erase("jobs"); c.data.erase("orders");
    c.error.clear(); c.attempted = std::chrono::steady_clock::now();
    c.display.update(c.source, c.data, c.error);
    return result;
}
inline void refresh_if_due(bool force = false) {
    if (!enabled()) return;
    auto &c = cache();
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!force && c.source == endpoint() && std::chrono::steady_clock::now() - c.attempted < std::chrono::seconds(15)) return;
        if (c.source != endpoint()) c.data = Json();
        c.source = endpoint(); c.attempted = std::chrono::steady_clock::now();
    }
    try { fresh_snapshot(); }
    catch (const std::exception &e) {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.error = e.what(); c.display.update(c.source, c.data, c.error);
    }
}
// Full settings are fetched only for an explicit user selection or download.
// Polling keeps the small digest summary and never installs executable settings.
inline Json profile_payload(const HaMaterialSource::Assignment &assignment) {
    if (assignment.material_profile.is_null()) return nullptr;
    const auto source = endpoint();
    HaMaterialBinding::validate({assignment.spool_uuid, source, "profile-download"});
    const auto expected = HaMaterialProfile::digest(assignment.material_profile);
    if (assignment.material_profile.at("material_type") != assignment.material_type)
        throw std::runtime_error("The HA material profile belongs to another material type");
    static std::mutex mutex;
    static std::map<std::string, Json> payloads;
    const auto key = source + "\n" + assignment.spool_uuid + "\n" + expected;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = payloads.find(key);
        if (found != payloads.end()) return found->second;
    }
    const std::string suffix = "/materials";
    if (source.size() < suffix.size() || source.substr(source.size() - suffix.size()) != suffix)
        throw std::runtime_error("Configure a valid HA materials endpoint");
    auto payload = assignment.material_profile.contains("settings") ? assignment.material_profile :
        ha_inventory_request_raw(source.substr(0, source.size() - suffix.size()) + "/profile?spool_uuid=" +
            assignment.spool_uuid + "&sha256=" + expected, nullptr, 2 * 1024 * 1024);
    HaMaterialProfile::validate(payload);
    if (source != endpoint() || HaMaterialProfile::digest(payload) != expected ||
        HaMaterialProfile::profile_summary(payload) != (assignment.material_profile.contains("settings") ?
            HaMaterialProfile::profile_summary(assignment.material_profile) : assignment.material_profile))
        throw std::runtime_error("The HA profile changed during download; refresh and select the roll again");
    std::lock_guard<std::mutex> lock(mutex);
    if (payloads.size() >= 64) payloads.clear();
    payloads.emplace(key, payload);
    return payload;
}

inline std::string install_profile(PresetBundle &bundle, const HaMaterialSource::Assignment &assignment) {
    if (assignment.material_profile.is_null()) return {};
    const auto payload = profile_payload(assignment);
    const auto name = HaMaterialProfile::managed_name(payload);
    const auto expected = HaMaterialProfile::digest(payload);
    Preset candidate(Preset::TYPE_FILAMENT, name, false);
    candidate.config = HaMaterialProfile::decode_config(payload);
    candidate.filament_id = payload.at("dependencies").at("filament_id").get<std::string>();
    candidate.loaded = true;
    candidate.version = bundle.filaments.get_selected_preset().version;
    if (!is_compatible_with_printer({candidate, nullptr}, bundle.printers.get_edited_preset_with_vendor_profile()))
        throw std::runtime_error("The HA material profile is not compatible with this printer and nozzle");
    if (const auto *existing = bundle.filaments.find_preset(name)) {
        if (existing->is_system || existing->is_default ||
            HaMaterialProfile::digest(HaMaterialProfile::capture(*existing, payload)) != expected)
            throw std::runtime_error("The downloaded HA profile name is already used by different local settings; save those settings under another name first");
        return name;
    }
    // managed_name sanitizes and bounds the display prefix. The normal loader
    // derives user profile identity from the filename, so both must agree.
    const auto path = bundle.filaments.path_from_name(name);
    candidate.file = path;
    if (boost::filesystem::exists(path)) {
        DynamicPrintConfig saved;
        std::map<std::string, std::string> values;
        std::string reason;
        saved.load_from_json(path, ForwardCompatibilitySubstitutionRule::Disable, values, reason);
        Preset prior = candidate;
        prior.config = std::move(saved);
        if (HaMaterialProfile::digest(HaMaterialProfile::capture(prior, payload)) != expected)
            throw std::runtime_error("The saved HA profile was modified locally; preserve it under another name before downloading again");
    } else {
        boost::filesystem::create_directories(boost::filesystem::path(path).parent_path());
        const auto temporary = path + ".pending";
        auto saved_config = candidate.config;
        if (!candidate.filament_id.empty())
            saved_config.set_key_value(BBL_JSON_KEY_FILAMENT_ID, new ConfigOptionString(candidate.filament_id));
        saved_config.save_to_json(temporary, name, "User", candidate.version.to_string());
        if (!wxRenameFile(from_u8(temporary), from_u8(path), false))
            throw std::runtime_error("Cannot persist the downloaded HA material profile");
    }
    auto &installed = bundle.filaments.load_preset(path, name, candidate.config, false, candidate.version);
    installed.filament_id = candidate.filament_id;
    installed.is_compatible = true;
    return name;
}

inline const Preset *resolve(const PresetBundle &bundle, const HaMaterialSource::Assignment &assignment) {
    const Preset *preset = nullptr;
    if (!assignment.material_profile.is_null()) {
        const auto name = HaMaterialProfile::managed_name(assignment.material_profile);
        preset = bundle.filaments.find_preset(name);
        if (preset && (preset->config.opt_string("filament_type", 0u) != assignment.material_type ||
            HaMaterialProfile::digest(HaMaterialProfile::capture(*preset, assignment.material_profile)) !=
                HaMaterialProfile::digest(assignment.material_profile))) return nullptr;
    } else {
        preset = HaMaterialSource::resolve_preset(bundle.filaments, assignment.material_preset, assignment.material_type);
    }
    if (preset && !is_compatible_with_printer(bundle.filaments.get_preset_with_vendor_profile(*preset),
                                             bundle.printers.get_edited_preset_with_vendor_profile())) return nullptr;
    return preset;
}
}
