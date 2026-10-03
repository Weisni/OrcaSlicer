#pragma once
#include <fstream>
#include <chrono>
#include <sstream>
#include <wx/utils.h>
#include <boost/algorithm/string/predicate.hpp>
#include "nlohmann/json.hpp"
#include "GUI_App.hpp"
#include "HaInventoryCredentials.hpp"
#include "FilamentInventoryService.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/HaInventoryTransport.hpp"

namespace Slic3r::GUI {
class HaInventoryRequestError : public std::runtime_error {
public:
    unsigned status;
    HaInventoryRequestError(unsigned code,const std::string &message):std::runtime_error(message),status(code){}
};
inline bool ha_inventory_demo_enabled() { return wxGetApp().app_config->get("ha_material_demo_enabled")=="1" || wxGetApp().app_config->get("ha_material_provider_enabled")=="1"; }
inline nlohmann::json ha_inventory_request_raw(const std::string &endpoint, const nlohmann::json *payload = nullptr,
                                              size_t maximum_bytes = 1024 * 1024) {
    std::string response; unsigned status=0;
    auto http=payload ? Http::post(endpoint) : Http::get(endpoint);
    const auto token = ha_inventory_token(endpoint);
    if (!token.empty()) http.header("Authorization", "Bearer " + token);
    if(payload) http.header("Content-Type","application/json").set_post_body(payload->dump());
    http.tls_verify(true).follow_redirects(false).timeout_connect(2).timeout_max(8).size_limit(maximum_bytes)
        .on_complete([&](std::string body,unsigned code){response=std::move(body);status=code;})
        .on_error([&](std::string body,std::string,unsigned code){response=std::move(body);status=code;}).perform_sync();
    if(status!=200) {
        std::string detail="Could not confirm the HA response. Pending changes remain available for reconciliation.";
        try { auto body=nlohmann::json::parse(response); detail=body.value("error",detail); } catch(...){}
        throw HaInventoryRequestError(status,detail);
    }
    return nlohmann::json::parse(response);
}

inline void validate_ha_material_snapshot(const nlohmann::json &snapshot) {
    if(snapshot.at("demo_mode")!=true || snapshot.at("printer_id")!="duck-poop-demo" || snapshot.at("schema_version")!=1 ||
        !snapshot.at("revision").is_number_integer() || snapshot.at("revision")<0 ||
        !snapshot.at("spools").is_array() || !snapshot.at("slots").is_array())
        throw std::runtime_error("Wrong or uninitialized HA material source; inventory preserved");
}

inline void validate_ha_inventory_snapshot(const nlohmann::json &snapshot) {
    validate_ha_material_snapshot(snapshot);
    if(!snapshot.contains("native_bundle") || snapshot.at("native_bundle").is_null() || snapshot.at("native_bundle").at("schema_version")!=8)
        throw std::runtime_error("The HA inventory snapshot is incomplete; inventory preserved");
}

inline void validate_ha_inventory_receipt(const nlohmann::json &receipt, const nlohmann::json &request) {
    if (receipt.at("accepted") != true || receipt.at("request_key") != request.at("request_key") ||
        !receipt.at("accepted_revision").is_number_integer() || receipt.at("accepted_revision") < 0 ||
        !receipt.at("revision").is_number_integer() || receipt.at("revision") < receipt.at("accepted_revision"))
        throw std::runtime_error("HA returned an invalid inventory receipt; the operation remains pending");
}

inline nlohmann::json ha_inventory_material_snapshot(const std::string &endpoint, bool include_inventory = true,
                                                    const nlohmann::json *initial = nullptr) {
    using Json = nlohmann::json;
    const std::string suffix = "/materials";
    if (!boost::algorithm::ends_with(endpoint, suffix)) throw std::runtime_error("Configure the HA materials endpoint first");
    bool first = true;
    const auto metadata = [&]() {
        Json snapshot;
        if (first && initial) snapshot = *initial;
        else snapshot = ha_inventory_request_raw(endpoint + "?view=provider", nullptr, 4 * 1024 * 1024);
        first = false;
        validate_ha_material_snapshot(snapshot);
        return snapshot;
    };
    auto snapshot = metadata();
    if (!include_inventory) return snapshot;
    if (snapshot.contains("native_bundle") && !snapshot.at("native_bundle").is_null()) return snapshot;
    if (!snapshot.value("capabilities", Json::object()).value("native_snapshot_pages", false))
        throw std::runtime_error("Update the HA integration to support paged inventory snapshots");
    bool use_initial = true;
    return HaInventoryTransport::read_snapshot([&]() {
        if (use_initial) { use_initial = false; return snapshot; }
        return metadata();
    }, [&](const std::string &table, size_t cursor, std::int64_t revision) {
        const auto url = endpoint.substr(0, endpoint.size() - suffix.size()) + "/native_snapshot?table=" + table +
            "&revision=" + std::to_string(revision) + "&cursor=" + std::to_string(cursor) + "&limit=200";
        try { return ha_inventory_request_raw(url, nullptr, 256 * 1024); }
        catch (const HaInventoryRequestError &error) {
            if (error.status == 409) throw HaInventoryTransport::RevisionChanged();
            throw;
        }
    });
}

inline nlohmann::json ha_inventory_request(const std::string &endpoint, const nlohmann::json *payload = nullptr) {
    if (!payload && boost::algorithm::ends_with(endpoint, "/materials")) return ha_inventory_material_snapshot(endpoint);
    // Only legacy durable provider_apply receipts still return a whole graph.
    const size_t limit = boost::algorithm::ends_with(endpoint, "/action/provider_apply") ? HaInventoryTransport::max_bundle_bytes :
        boost::algorithm::ends_with(endpoint, "/action/provider_job") ? 4 * 1024 * 1024 : 1024 * 1024;
    return ha_inventory_request_raw(endpoint, payload, limit);
}
}
