#pragma once

#include "HaInventoryExplicit.hpp"
#include "HaInventoryAuthority.hpp"
#include "libslic3r/HaMaterialCatalog.hpp"
#include "libslic3r/HaMaterialBinding.hpp"
#include "libslic3r/HaMaterialRefresh.hpp"
#include "libslic3r/HaMaterialProfile.hpp"
#include "libslic3r/HaMaterialProfileCache.hpp"
#include "libslic3r/HaMaterialAsyncRefresh.hpp"
#include <mutex>

namespace Slic3r::GUI::HaMaterialProvider {
using Json = nlohmann::json;
inline Json profile_context(const PresetBundle &bundle) {
    return HaMaterialSource::profile_context(bundle.printers.get_edited_preset().config, bundle.project_config);
}
inline HaMaterialSource::Assignment for_printer(const PresetBundle &bundle, const HaMaterialSource::Assignment &assignment) {
    return HaMaterialSource::with_context(assignment, profile_context(bundle));
}
inline bool compatible(const PresetBundle &bundle, const Preset &preset, const std::string &material_type) {
    return HaMaterialSource::same_material_family(preset.config.opt_string("filament_type", 0u), material_type) &&
        is_compatible_with_printer(bundle.filaments.get_preset_with_vendor_profile(preset),
                                  bundle.printers.get_edited_preset_with_vendor_profile());
}
inline bool preserves_override(const PresetBundle &bundle, size_t index, const HaMaterialBinding::Binding &binding) {
    if (index >= bundle.filament_presets.size() || binding.profile_context.empty()) return true;
    const auto &name = bundle.filament_presets[index];
    const auto &edited = bundle.filaments.get_edited_preset();
    const auto *preset = edited.name == name ? &edited : bundle.filaments.find_preset(name);
    return HaMaterialBinding::preserves_override(binding, preset);
}
inline bool enabled() { return wxGetApp().app_config->get_bool("ha_material_provider_enabled"); }
inline std::string endpoint() { return wxGetApp().app_config->get("ha_material_demo_endpoint"); }
inline std::string physical_device_id() { return wxGetApp().app_config->get("ha_material_provider_device_id"); }
struct Cache {
    std::mutex mutex;
    Json data;
    std::string source, error;
    HaMaterialRefresh::DisplayRevision display;
    std::chrono::steady_clock::time_point attempted{};
    std::shared_ptr<HaMaterialProfile::PersistentCache> profile_contents;
    HaMaterialRefresh::AsyncRefresh worker;
    std::string paired_device;
    std::string restore_source, restore_device;
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
inline HaMaterialProfile::PersistentCache& profile_cache() {
    auto& contents = cache().profile_contents;
    if (!contents) {
        const auto keys = HaMaterialProfile::setting_keys();
        contents = std::make_shared<HaMaterialProfile::PersistentCache>(
            std::filesystem::u8path(data_dir()) / "ha_material_profiles", Json(keys).dump(), keys);
    }
    return *contents;
}
inline void validate_provider(const Json& result, const std::string& device) {
    validate_ha_material_snapshot(result);
    if (result.value("provider_api_version", 0) != 1 || device.empty() ||
        result.value("provider_printer_id", std::string()) != device)
        throw std::runtime_error("Connect the HA material source to its configured physical printer first");
    HaMaterialCatalog::available(result);
}
inline void publish_snapshot(const std::string& source, const Json& result) {
    auto &c = cache(); std::lock_guard<std::mutex> lock(c.mutex);
    c.source = source; c.data = result; c.data.erase("native_bundle"); c.data.erase("jobs"); c.data.erase("orders");
    c.paired_device = physical_device_id();
    c.error.clear(); c.attempted = std::chrono::steady_clock::now();
    c.display.update(c.source, c.data, c.error);
}
inline void preload_contents(const std::string& source, const std::string& token, const Json& result,
                             const std::function<bool()>& active = [] { return true; });
inline void preload_cached(PresetBundle& bundle, const std::string& source, const Json& result);
inline Json fresh_snapshot() {
    if (!enabled()) throw std::runtime_error("Enable the Home Assistant material source first");
    const auto source = endpoint();
    cache().worker.invalidate();
    HaInventoryAuthority::attach();
    const auto token = ha_inventory_token(source);
    auto result = HaInventoryAuthority::fetch_refresh(source, token);
    validate_provider(result, physical_device_id());
    preload_contents(source,token,result);
    if (source != endpoint() || !enabled()) throw std::runtime_error("The HA material source changed during refresh");
    result = HaInventoryAuthority::refresh(result);
    if (source != endpoint() || !enabled() || result.value("provider_api_version", 0) != 1 ||
        result.value("provider_printer_id", std::string()) != physical_device_id())
        throw std::runtime_error("The HA material source or its physical printer changed during refresh");
    validate_provider(result, physical_device_id());
    // Receipt reconciliation can advance the graph. Seal its content before
    // publishing names so explicit selection always sees the installed identity.
    preload_contents(source,token,result);
    preload_cached(*wxGetApp().preset_bundle,source,result);
    publish_snapshot(source, result);
    return result;
}
inline Json fetch_profile(const std::string& source, const std::string& token, const Json& summary,
                          const std::string& spool = {}, const std::string& context = {}) {
    const std::string suffix = "/materials";
    if (source.size() < suffix.size() || source.substr(source.size() - suffix.size()) != suffix)
        throw std::runtime_error("Configure a valid HA materials endpoint");
    if (summary.contains("settings")) return summary;
    const auto base = source.substr(0, source.size() - suffix.size());
    const auto expected = HaMaterialProfile::digest(summary);
    try { return ha_inventory_request_raw_with_token(base + "/profile_content?sha256=" + expected, token, nullptr, 262144); }
    catch (const HaInventoryRequestError& error) {
        if ((error.status != 404 && error.status != 405) || spool.empty()) throw;
    }
    // Older components expose only an association-based endpoint. Its returned
    // digest is still checked by the cache, so reassignment never substitutes data.
    return ha_inventory_request_raw_with_token(base + "/profile?spool_uuid=" + spool + "&sha256=" + expected +
        (context.empty() ? "" : "&context=" + Http::url_encode(context)), token, nullptr, 262144);
}
inline void preload_contents(const std::string& source, const std::string& token, const Json& result,
                             const std::function<bool()>& active) {
    // This helper uses an already-initialized cache and captured inputs. The
    // background path passes its shared cache directly below to avoid wx access.
    for (const auto& profile : HaMaterialProfile::published_contents(result)) {
        if (!active()) throw std::runtime_error("The HA material refresh was superseded");
        profile_cache().get(source,profile.summary,[&] {
            return fetch_profile(source,token,profile.summary,profile.spool,profile.context);
        });
    }
}
inline Json profile_payload(const HaMaterialSource::Assignment &assignment) {
    if (assignment.material_profile.is_null()) return {};
    const auto source = endpoint();
    HaMaterialBinding::validate({assignment.spool_uuid, source, "profile-download"});
    if (!HaMaterialSource::same_material_family(assignment.material_profile.at("material_type"), assignment.material_type))
        throw std::runtime_error("The HA material profile belongs to another material type");
    const auto token = ha_inventory_token(source);
    const auto payload = profile_cache().get(source, assignment.material_profile, [&] {
        return fetch_profile(source, token, assignment.material_profile, assignment.spool_uuid,
            HaMaterialSource::variant_profile(assignment).is_null() ? "" : assignment.profile_context_key);
    });
    if (source != endpoint() || !enabled()) throw std::runtime_error("The HA material source changed during download");
    return payload;
}
inline std::string cached_name(const Json& summary) {
    if (summary.is_null()) return {};
    const auto payload = profile_cache().find(endpoint(), summary);
    return HaMaterialProfile::managed_name(payload.is_null() ? summary : payload);
}
inline std::string profile_name(const HaMaterialSource::Assignment& assignment) {
    return assignment.material_profile.is_null() ? assignment.material_preset : cached_name(assignment.material_profile);
}

inline std::string install_payload(PresetBundle &bundle, const Json& payload, bool selection = true) {
    const auto name = HaMaterialProfile::managed_name(payload);
    const auto expected = HaMaterialProfile::content_identity(payload);
    Preset candidate(Preset::TYPE_FILAMENT, name, false);
    candidate.config = HaMaterialProfile::decode_config(payload);
    candidate.filament_id = payload.at("dependencies").at("filament_id").get<std::string>();
    candidate.loaded = true;
    candidate.version = bundle.filaments.get_selected_preset().version;
    if (!is_compatible_with_printer({candidate, nullptr}, bundle.printers.get_edited_preset_with_vendor_profile())) {
        if (!selection) return {};
        throw std::runtime_error("The HA material profile is not compatible with this printer and nozzle");
    }
    if (const auto *existing = HaMaterialProfile::stored_preset(bundle.filaments, name)) {
        if (existing->is_system || existing->is_default ||
            HaMaterialProfile::content_identity(HaMaterialProfile::capture(*existing, payload)) != expected)
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
        if (HaMaterialProfile::content_identity(HaMaterialProfile::capture(prior, payload)) != expected)
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
    installed.alias = "HA " + payload.at("name").get<std::string>();
    installed.is_compatible = true;
    return name;
}

inline std::string install_profile(PresetBundle &bundle, const HaMaterialSource::Assignment &assignment) {
    return assignment.material_profile.is_null() ? std::string() : install_payload(bundle, profile_payload(assignment));
}
inline void preload_cached(PresetBundle& bundle, const std::string& source, const Json& result) {
    for (const auto& profile : HaMaterialProfile::published_contents(result)) {
        const auto payload = profile_cache().find(source,profile.summary);
        if (payload.is_null()) throw std::runtime_error("The downloaded HA profile cache is incomplete");
        const auto name = HaMaterialProfile::managed_name(payload);
        if (!bundle.filaments.find_preset(name)) install_payload(bundle,payload,false);
    }
}

inline void restore_if_available(PresetBundle& bundle) {
    if (!enabled()) return;
    auto& c = cache();
    const auto source = endpoint();
    const auto device = physical_device_id();
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.data.is_object() && c.source == source && c.paired_device == device) return;
        if (c.restore_source == source && c.restore_device == device) return;
        c.restore_source = source; c.restore_device = device;
    }
    try {
        // attach restores only the graph whose persisted digest matches the
        // local database. Read its provider roster without contacting HA.
        HaInventoryAuthority::attach();
        Json restored;
        {
            auto& authority = HaInventoryAuthority::state();
            std::lock_guard<std::mutex> lock(authority.mutex);
            if (authority.endpoint != source || !authority.snapshot.is_object()) return;
            restored = authority.snapshot;
        }
        validate_provider(restored,device);
        if (!HaMaterialProfile::restorable_contents(profile_cache(),source,restored)) return;
        preload_cached(bundle,source,restored);
        if (source == endpoint() && device == physical_device_id() && enabled()) publish_snapshot(source,restored);
    } catch (const std::exception&) {
        // Incomplete, incompatible or corrupt caches remain unpublished. The
        // normal worker will report connectivity errors and repair missing data.
    }
}

inline void refresh_if_due(bool force = false) {
    auto &c = cache();
    if (!enabled()) { c.worker.invalidate(); return; }
    const auto source = endpoint();
    const auto device = physical_device_id();
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.source != source || c.paired_device != device) {
            c.worker.invalidate(); c.data = Json(); c.error.clear(); c.attempted = {};
            c.source = source; c.paired_device = device;
            c.display.update(c.source, c.data, c.error);
        }
        if (!force && std::chrono::steady_clock::now() - c.attempted < std::chrono::seconds(15)) return;
    }
    if (c.worker.pending()) return;
    try {
        HaInventoryAuthority::attach();
        const auto token = ha_inventory_token(source);
        profile_cache(); // Initialize settings schema/user path on GUI thread.
        const auto contents = c.profile_contents;
        if (c.worker.start(source, [source,device,token,contents](const auto& active) {
            auto result = HaInventoryAuthority::fetch_refresh(source,token);
            validate_provider(result,device);
            for (const auto& profile : HaMaterialProfile::published_contents(result)) {
                if (!active()) throw std::runtime_error("The HA material refresh was superseded");
                contents->get(source,profile.summary,[&] {
                    return fetch_profile(source,token,profile.summary,profile.spool,profile.context);
                });
            }
            return result;
        })) {
            std::lock_guard<std::mutex> lock(c.mutex);
            c.attempted = std::chrono::steady_clock::now();
        }
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.error = error.what(); c.attempted = std::chrono::steady_clock::now();
        c.display.update(c.source,c.data,c.error);
    }
}

inline void complete_refresh(PresetBundle& bundle) {
    auto& c = cache();
    if (!enabled()) { c.worker.invalidate(); c.worker.take({}); return; }
    restore_if_available(bundle);
    auto completion = c.worker.take(endpoint());
    if (!completion) return;
    try {
        if (!completion->error.empty()) throw std::runtime_error(completion->error);
        if (c.paired_device != physical_device_id()) return;
        validate_provider(completion->data, physical_device_id());
        // Import and preset installation happen only here, on the GUI thread.
        auto result = HaInventoryAuthority::refresh(completion->data,false);
        validate_provider(result, physical_device_id());
        preload_cached(bundle,completion->source,result);
        publish_snapshot(completion->source,result);
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.error = error.what(); c.display.update(c.source,c.data,c.error);
    }
}

inline void shutdown_refresh() { cache().worker.shutdown(); }

inline const Preset *resolve(const PresetBundle &bundle, const HaMaterialSource::Assignment &source_assignment) {
    const auto assignment = for_printer(bundle, source_assignment);
    const Preset *preset = nullptr;
    if (!assignment.material_profile.is_null()) {
        const auto payload = profile_cache().find(endpoint(), assignment.material_profile);
        if (payload.is_null()) return nullptr;
        const auto name = HaMaterialProfile::managed_name(payload);
        preset = HaMaterialProfile::stored_preset(bundle.filaments, name);
        if (preset && (!HaMaterialSource::same_material_family(preset->config.opt_string("filament_type", 0u), assignment.material_type) ||
            HaMaterialProfile::content_identity(HaMaterialProfile::capture(*preset, payload)) !=
                HaMaterialProfile::content_identity(payload))) return nullptr;
    } else {
        preset = HaMaterialSource::resolve_preset(bundle.filaments, assignment.material_preset, assignment.material_type);
    }
    if (preset && !compatible(bundle, *preset, assignment.material_type)) return nullptr;
    return preset;
}
}
