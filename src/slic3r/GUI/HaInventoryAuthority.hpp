#pragma once

#include "HaInventoryDemo.hpp"
#include <wx/filefn.h>
#include <mutex>
#include <map>

namespace Slic3r::GUI::HaInventoryAuthority {
using Json = nlohmann::json;

inline bool enabled() { return wxGetApp().app_config->get("ha_material_provider_enabled") == "1"; }
inline std::string endpoint() { return wxGetApp().app_config->get("ha_material_demo_endpoint"); }

struct State {
    std::mutex mutex;
    std::recursive_mutex refresh_mutex;
    bool refreshing = false;
    std::string endpoint, local_baseline;
    Json snapshot;
};
inline State &state() { static State value; return value; }
inline std::string journal(const std::string &kind) { return data_dir() + "/ha-authority-" + kind + "-pending.json"; }
inline std::string key() { return "quack-authority:" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()); }

inline Json read_journal(const std::string &path)
{
    Json value;
    std::ifstream file(path);
    file >> value;
    if (!value.is_object() || !value.contains("payload") || !value.contains("endpoint") || !value.contains("action"))
        throw std::runtime_error("Invalid pending HA operation; preserve it and reconcile the inventory");
    return value;
}

inline void save_journal(const std::string &path, const std::string &url, const std::string &action, const Json &payload)
{
    const auto temporary = path + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << Json{{"endpoint", url}, {"action", action}, {"payload", payload}}.dump();
    file.close();
    if (!file || !wxRenameFile(from_u8(temporary), from_u8(path), false))
        throw std::runtime_error("Cannot persist the HA operation; no changes were sent");
}

inline void save_job_queue(const std::string &url, const Json &requests)
{
    const auto path = journal("job"), temporary = path + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << Json{{"endpoint", url}, {"action", "provider_job"}, {"requests", requests}}.dump();
    file.close();
    if (!file || !wxRenameFile(from_u8(temporary), from_u8(path), true))
        throw std::runtime_error("Cannot retain the printer outcome for HA reconciliation");
}

inline std::string action_url(const std::string &url, const std::string &action)
{
    const std::string suffix = "/materials";
    if (!boost::algorithm::ends_with(url, suffix) || (action != "provider_apply" && action != "provider_delta" && action != "provider_job"))
        throw std::runtime_error("Invalid HA provider endpoint or operation");
    return url.substr(0, url.size() - suffix.size()) + "/action/" + action;
}

// Caller holds State::mutex. Repeating these commands never resends a print:
// they only reconcile the same durable inventory/job request on the server.
inline Json send(const std::string &url, const std::string &action, const Json &payload)
{
    auto request = payload;
    if (action == "provider_job") request["response"] = "provider";
    const auto response = ha_inventory_request(action_url(url, action), &request);
    if (action == "provider_delta") validate_ha_inventory_receipt(response, request);
    else validate_ha_material_snapshot(response);
    return response;
}

inline void preserve_original_inventory(const std::string &url, const std::string &bundle)
{
    // The stable hash is a filename key, not an authentication mechanism.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : url) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream name; name << std::hex << hash;
    const auto path = data_dir() + "/ha-authority-original-" + name.str() + ".json";
    if (wxFileExists(from_u8(path))) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() > std::streamoff(HaInventoryTransport::max_bundle_bytes + 4096))
            throw std::runtime_error("The existing HA inventory recovery file exceeds the supported size");
        file.seekg(0);
        Json saved; file >> saved;
        if (saved.at("endpoint") != url || !saved.contains("native_bundle"))
            throw std::runtime_error("Preserve and reconcile the existing HA inventory recovery file before reconnecting");
        return;
    }
    const auto temporary = path + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << Json{{"endpoint", url}, {"native_bundle", Json::parse(bundle)}}.dump();
    file.close();
    if (!file || !wxRenameFile(from_u8(temporary), from_u8(path), false))
        throw std::runtime_error("Cannot retain the original inventory; the authoritative import was not applied");
}

inline Json recover_pending_locked(const std::string &url)
{
    Json last_job_result;
    for (const auto &kind : {std::string("inventory"), std::string("job")}) {
        const auto path = journal(kind);
        if (!wxFileExists(from_u8(path))) continue;
        Json pending; { std::ifstream file(path); file >> pending; }
        if (pending.at("endpoint") != url)
            throw std::runtime_error("Reconnect the HA source owning the pending inventory operation");
        const auto requests = pending.contains("requests") ? pending.at("requests") : Json::array({pending.at("payload")});
        for (const auto &request : requests)
            try {
                const auto result = send(url, pending.at("action").get<std::string>(), request);
                if (result.contains("provider_job")) last_job_result = result.at("provider_job");
            }
            catch (const HaInventoryRequestError &error) {
                if (error.status != 400 && error.status != 409) throw;
                // A definitive rejection did not change HA; import its current state.
            }
        // Keep the receipt until its current authoritative snapshot is imported.
    }
    return last_job_result;
}

inline void refresh_current();

inline void attach()
{
    auto &service = wxGetApp().filament_inventory();
    auto &store = service.store();
    if (!enabled()) {
        store.set_authority_handler({});
        service.set_authority_refresher({});
        return;
    }
    store.set_authority_handler([](const std::string &before, const std::string &after) {
        auto &shared = state();
        std::lock_guard<std::mutex> lock(shared.mutex);
        const auto url = endpoint();
        if (shared.refreshing || !shared.snapshot.is_object() || shared.endpoint != url || shared.local_baseline != before)
            throw std::runtime_error("Refresh the authoritative HA inventory before changing it");
        const auto path = journal("inventory");
        if (wxFileExists(from_u8(path)) || wxFileExists(from_u8(journal("job"))))
            throw std::runtime_error("A previous HA result is pending; refresh to reconcile it before another change");
        const auto changes = HaInventoryTransport::changes(shared.snapshot.at("native_bundle"), Json::parse(before), Json::parse(after));
        if (changes.empty()) { shared.local_baseline = after; return; }
        if (!shared.snapshot.value("capabilities", Json::object()).value("provider_delta", false))
            throw std::runtime_error("Update the HA integration to support transactional row changes");
        const Json payload = {{"request_key", key()}, {"revision", shared.snapshot.at("revision")}, {"changes", changes}};
        save_journal(path, url, "provider_delta", payload);
        try {
            send(url, "provider_delta", payload);
            shared.local_baseline = after;
        } catch (const HaInventoryRequestError &error) {
            if (error.status == 400 || error.status == 409) wxRemoveFile(from_u8(path));
            throw;
        }
    }); // Keep the receipt until a complete authoritative refresh is imported.
    service.set_authority_refresher([] { refresh_current(); });
}

inline Json refresh(const Json &supplied)
{
    std::lock_guard<std::recursive_mutex> serialize(state().refresh_mutex);
    attach();
    if (!enabled()) return supplied;
    validate_ha_material_snapshot(supplied);
    auto &store = wxGetApp().filament_inventory().store();
    const auto before = store.export_ha_demo_bundle();
    const auto url = endpoint();
    Json snapshot = supplied;
    std::map<std::string, Json> recovered;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().refreshing = true;
    }
    try {
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        // A GUI GET may have started before a native transaction received its
        // acknowledgement. Never replace that committed cache with older data.
        if (state().endpoint == url && state().snapshot.is_object() &&
            snapshot.at("revision") < state().snapshot.at("revision")) {
            snapshot = ha_inventory_material_snapshot(url, false);
            validate_ha_material_snapshot(snapshot);
            if (snapshot.at("revision") < state().snapshot.at("revision"))
                throw std::runtime_error("HA returned an older revision than the acknowledged inventory");
        }
        const bool pending = wxFileExists(from_u8(journal("inventory"))) || wxFileExists(from_u8(journal("job")));
        if (pending) {
            for (const auto &kind : {std::string("inventory"), std::string("job")}) {
                const auto path = journal(kind);
                if (wxFileExists(from_u8(path))) { std::ifstream file(path); file >> recovered[path]; }
            }
            const auto job_result = recover_pending_locked(url);
            snapshot = ha_inventory_material_snapshot(url);
            validate_ha_inventory_snapshot(snapshot);
            if (!job_result.is_null()) snapshot["provider_job"] = job_result;
        } else if (!snapshot.contains("native_bundle") || snapshot.at("native_bundle").is_null()) {
            if (state().endpoint == url && state().snapshot.is_object() &&
                state().snapshot.at("revision") == snapshot.at("revision") && state().local_baseline == before)
                snapshot["native_bundle"] = state().snapshot.at("native_bundle");
            else snapshot = ha_inventory_material_snapshot(url, true, &snapshot);
        }
    }
    // Never hold the network/state mutex while acquiring the native Store lock:
    // native pre-commit callbacks acquire these locks in the opposite order.
    Json acknowledgement = {{"explicit_sync", true}, {"endpoint", url}, {"revision", snapshot.at("revision")},
        {"_expected_local", Json::parse(before)}};
    if (endpoint() != url || !enabled()) throw std::runtime_error("The HA authority changed during refresh; inventory preserved");
    validate_ha_inventory_snapshot(snapshot);
    if (snapshot.at("native_bundle") != acknowledgement.at("_expected_local")) {
        preserve_original_inventory(url, before);
        store.import_authoritative_ha_bundle(snapshot.at("native_bundle").dump(), acknowledgement.dump());
    } else {
        acknowledgement.erase("_expected_local");
        store.save_ha_demo_sync_state(acknowledgement.dump());
    }
    const auto imported = store.export_ha_demo_bundle();
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().endpoint = url;
        state().snapshot = snapshot;
        state().local_baseline = imported;
        for (const auto &[path, value] : recovered) {
            Json current; { std::ifstream file(path); file >> current; }
            if (current != value || !wxRemoveFile(from_u8(path)))
                throw std::runtime_error("HA inventory is refreshed but its durable receipt could not be cleared");
        }
        state().refreshing = false;
    }
    } catch (...) { std::lock_guard<std::mutex> lock(state().mutex); state().refreshing = false; throw; }
    return snapshot;
}

inline void refresh_current()
{
    std::lock_guard<std::recursive_mutex> serialize(state().refresh_mutex);
    attach();
    if (!enabled()) return;
    Json current;
    { std::lock_guard<std::mutex> lock(state().mutex); current = ha_inventory_material_snapshot(endpoint(), false); }
    refresh(current);
}

inline Json provider_job(const std::string &command, const std::string &job_uuid, const Json &data)
{
    std::lock_guard<std::recursive_mutex> serialize(state().refresh_mutex);
    if (!enabled()) throw std::runtime_error("Select Home Assistant as the inventory authority first");
    attach();
    Json response;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        const auto url = endpoint(), path = journal("job");
        const Json payload = {{"request_key", key()}, {"command", command}, {"job_uuid", job_uuid}, {"data", data}};
        Json requests = Json::array();
        if (wxFileExists(from_u8(path))) {
            Json pending; { std::ifstream file(path); file >> pending; }
            if (pending.at("endpoint") != url) throw std::runtime_error("Reconnect the source owning pending printer outcomes");
            requests = pending.contains("requests") ? pending.at("requests") : Json::array({pending.at("payload")});
        }
        // Retain the actual dispatch outcome BEFORE any HTTP call can fail.
        requests.push_back(payload);
        save_job_queue(url, requests);
        for (size_t index = 0; index < requests.size(); ++index)
            try { response = send(url, "provider_job", requests.at(index)); }
            catch (const HaInventoryRequestError &error) {
                if (error.status == 400 || error.status == 409) {
                    requests.erase(requests.begin() + index);
                    if (requests.empty()) wxRemoveFile(from_u8(path)); else save_job_queue(url, requests);
                }
                throw;
            }
    }
    return refresh(response);
}

} // namespace Slic3r::GUI::HaInventoryAuthority
