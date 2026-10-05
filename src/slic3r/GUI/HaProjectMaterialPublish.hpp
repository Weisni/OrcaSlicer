#pragma once

#include "HaInventoryExplicit.hpp"
#include "libslic3r/HaProjectRollPublish.hpp"
#include <iomanip>
#include <map>
#include <sstream>

namespace Slic3r::GUI {

// A dedicated journal retains exactly the confirmed roll metadata/stock
// transfer across retries, including new UUIDs, without saving credentials.
inline bool publish_ha_project_materials(wxWindow *parent, const std::string &endpoint,
                                         const nlohmann::json &snapshot, const nlohmann::json &changes,
                                         const std::function<void()> &validate_project,
                                         const std::vector<std::string> &selected_uuids = {},
                                         bool reviewed_in_dialog = false)
{
    using Json = nlohmann::json;
    const std::string suffix = "/materials";
    if (!boost::algorithm::ends_with(endpoint, suffix))
        throw std::runtime_error("Configure the HA /materials endpoint first");
    const auto journal = data_dir() + "/ha-project-material-pending.json";
    Json payload;
    std::vector<std::string> cache_rolls = selected_uuids;
    bool saved_cache_scope = false;
    const bool retry = wxFileExists(from_u8(journal));
    const bool cache_only = !retry && changes.empty();
    if (retry) {
        Json saved;
        std::ifstream in(journal);
        in >> saved;
        if (saved.at("endpoint") != endpoint)
            throw std::runtime_error("The pending material save belongs to another HA endpoint");
        payload = saved.at("payload");
        // Retry the originally reviewed cache scope, never a newer selection.
        saved_cache_scope = saved.contains("cache_rolls");
        cache_rolls = saved_cache_scope ? saved.at("cache_rolls").get<std::vector<std::string>>() : std::vector<std::string>{};
    } else {
        payload = {{"revision", snapshot.at("revision")}, {"confirmed", true}, {"concurrency", "fields"}, {"response", "ack"}, {"changes", changes},
            {"request_key", "quack-project-material:" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count())}};
    }
    if (!cache_only) HaProjectRollPublish::validate_payload(payload);
    if (cache_rolls.empty() && !saved_cache_scope)
        for (const auto &change : payload.at("changes")) cache_rolls.push_back(change.at("spool_uuid").get<std::string>());
    if (cache_rolls.empty() || cache_rolls.size() > 100)
        throw std::runtime_error("Select between one and 100 physical rolls to synchronize");
    std::set<std::string> cache_ids;
    for (const auto &uuid : cache_rolls)
        if (!HaProjectRollPublish::canonical_uuid(uuid) || !cache_ids.insert(uuid).second)
            throw std::runtime_error("Select each canonical physical roll UUID only once");
    for (const auto &change : payload.at("changes"))
        if (!cache_ids.count(change.at("spool_uuid").get<std::string>()))
            throw std::runtime_error("The saved cache scope must include every transferred roll");
    auto &store = wxGetApp().filament_inventory().store();
    // The same local baseline is reviewed and later checked atomically on import.
    const auto local_before = Json::parse(store.export_ha_demo_bundle());
    const auto grams = [](const Json &value) {
        std::ostringstream text;
        text << std::fixed << std::setprecision(3) << value.get<std::int64_t>() / 1000.0 << " g";
        return text.str();
    };
    std::string preview = cache_only ? "HA already matches the selected uploads. Synchronize its current stock into Quack?\n\n" :
                          retry ? "Retry the previously confirmed roll transfer?\n\n" : "Save these physical rolls to HA?\n\n";
    for (const auto &change : payload.at("changes")) {
        const bool create = change.value("create", false);
        preview += std::string(create ? "Create roll " : "Update roll ") + change.at("spool_uuid").get<std::string>() + "\n";
        for (auto field = change.at("fields").begin(); field != change.at("fields").end(); ++field) {
            const std::map<std::string, std::string> names = {{"name", "Name"}, {"manufacturer", "Manufacturer"},
                {"material_type", "Material"}, {"filament_preset_id", "Profile"}, {"color_hex", "Color"},
                {"nominal_capacity_mg", "Nominal capacity"}, {"status", "Status"}};
            const auto after = field.key() == "nominal_capacity_mg" ? grams(field.value()) : field.value().get<std::string>();
            preview += names.at(field.key()) + ": " +
                (create ? "" : change.at("expected").at(field.key()).get<std::string>() + " -> ") + after + "\n";
        }
        if (change.contains("material_profile")) {
            preview += "Profile settings: " + change.at("material_profile").at("name").get<std::string>() + "\n";
            if (change.contains("profile_context"))
                preview += "Printer/nozzle: " + HaMaterialContext::key(change.at("profile_context")) + "\n";
        }
        if (change.contains("remaining_mg"))
            preview += "Stock: " + (create ? std::string("new -> ") : grams(change.at("expected_remaining_mg")) + " -> ") +
                grams(change.at("remaining_mg")) + " (estimated)\n";
        preview += "\n";
    }
    preview += "Refresh local stock and stock status from Home Assistant for:\n";
    for (const auto &uuid : cache_rolls) preview += uuid + "\n";
    preview += "Current HA stock is read after confirmation. Other local fields keep their values unless listed above.\n"
               "Rolls missing locally are added with their HA metadata.\n";
    if (cache_only) preview += "This only updates the selected local inventory records; Home Assistant is unchanged.";
    else preview += "Only the listed changes are sent. Existing roll UUIDs and print history are preserved.\n"
                    "Setting stock to zero marks that roll empty and clears its HA slot. No printer command is sent.";
    // The compact dialog already shows and confirms the selected differences.
    // Recovery still reviews the frozen request, which may differ from today's selection.
    if ((!reviewed_in_dialog || retry || cache_only) &&
        wxMessageBox(from_u8(preview), _L("Save project materials to HA"),
                     wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, parent) != wxYES) return false;
    validate_project();
    if (!retry && !cache_only) {
        const auto temporary = journal + ".tmp";
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << Json{{"endpoint", endpoint}, {"payload", payload}, {"cache_rolls", cache_rolls}}.dump();
        out.close();
        if (!out || !wxRenameFile(from_u8(temporary), from_u8(journal), false))
            throw std::runtime_error("Cannot persist the material save; no data sent");
    }
    const auto action = endpoint.substr(0, endpoint.size() - std::string("/materials").size()) + "/action/native_apply";
    bool accepted = false;
    try {
        if (!cache_only) {
            auto request = payload;
            request["response"] = "ack"; // Transport-only hint also works for existing durable requests.
            const auto response = ha_inventory_request(action, &request);
            accepted = true;
            validate_ha_inventory_receipt(response, request);
        }
        // A replay receipt may precede later HA consumption. Fetch the current
        // authority before updating only the explicitly transferred cache rows.
        const auto fresh = ha_inventory_request(endpoint);
        validate_ha_inventory_snapshot(fresh);
        if (HaInventoryAuthority::enabled()) {
            HaInventoryAuthority::refresh(fresh);
            if (!cache_only && !wxRemoveFile(from_u8(journal)))
                throw std::runtime_error("HA accepted the material save; its journal remains for a safe explicit retry");
            return true;
        }
        std::vector<HaInventorySelection::Selection> selected;
        for (const auto &uuid : cache_rolls) {
            HaInventorySelection::Selection selection{uuid, {"status"}, true};
            if (!HaInventorySelection::spool(local_before, selection.uuid)) {
                selection.fields = {HaInventorySelection::fields.begin(), HaInventorySelection::fields.end()};
            } else {
                for (const auto &change : payload.at("changes")) if (change.at("spool_uuid") == uuid)
                    for (auto field = change.at("fields").begin(); field != change.at("fields").end(); ++field)
                        selection.fields.insert(field.key());
            }
            selected.push_back(std::move(selection));
        }
        const auto cache_key = payload.at("request_key").get<std::string>() + ":cache:" + fresh.at("revision").dump();
        const auto timestamp = std::string(wxDateTime::UNow().ToUTC().FormatISOCombined('T').ToUTF8().data()) + "Z";
        const auto merged = HaInventorySelection::download_bundle(local_before, fresh.at("native_bundle"), selected, cache_key, timestamp);
        auto state = HaInventorySelection::acknowledge(Json::parse(store.ha_demo_sync_state()), merged, selected);
        state["endpoint"] = endpoint;
        state["revision"] = fresh.at("revision");
        state["_expected_local"] = local_before;
        store.import_ha_demo_bundle(merged.dump(), state.dump());
        if (!cache_only && !wxRemoveFile(from_u8(journal)))
            throw std::runtime_error("HA accepted the material save; its journal remains for a safe explicit retry");
    } catch (const HaInventoryRequestError &error) {
        if (!cache_only && !accepted && (error.status == 400 || error.status == 409)) wxRemoveFile(from_u8(journal));
        if (accepted) throw std::runtime_error(std::string("HA accepted the transfer; local completion is pending. Retry the saved request. ") + error.what());
        throw;
    } catch (const std::exception &error) {
        if (accepted) throw std::runtime_error(std::string("HA accepted the transfer; local completion is pending. Retry the saved request. ") + error.what());
        throw;
    }
    return true;
}

} // namespace Slic3r::GUI
