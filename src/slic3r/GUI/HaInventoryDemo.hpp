#pragma once
#include <fstream>
#include <chrono>
#include <sstream>
#include <wx/utils.h>
#include <boost/algorithm/string/predicate.hpp>
#include "nlohmann/json.hpp"
#include "GUI_App.hpp"
#include "HaInventoryCredentials.hpp"
#include "HaNetworkOperation.hpp"
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
// Worker callers capture credentials on the GUI thread before scheduling work.
inline nlohmann::json ha_inventory_request_raw_with_token(const std::string &endpoint, const std::string &token,
                                              const nlohmann::json *payload = nullptr, size_t maximum_bytes = 1024 * 1024) {
    const auto body = payload ? payload->dump() : std::string();
    const bool posting = payload != nullptr;
    return HaNetworkOperation::run([endpoint, token, body, posting, maximum_bytes](const std::function<bool()>& canceled) {
    std::string response, transport_error; unsigned status=0;
    auto http=posting ? Http::post(endpoint) : Http::get(endpoint);
    if (!token.empty()) http.header("Authorization", "Bearer " + token);
    if(posting) http.header("Content-Type","application/json").set_post_body(body);
    // Windows DNS/mDNS can take about eleven seconds on multiple interfaces.
    // Reuse successful resolutions and allow the cold lookup on the worker;
    // the bounded request remains cancellable through the loading dialog.
    http.share_dns_cache().tls_verify(true).follow_redirects(false).timeout_connect(15).timeout_max(20).size_limit(maximum_bytes)
        .on_progress([&](Http::Progress, bool& cancel) { cancel = canceled(); })
        .on_complete([&](std::string body,unsigned code){response=std::move(body);status=code;})
        .on_error([&](std::string body,std::string error,unsigned code){response=std::move(body);transport_error=std::move(error);status=code;}).perform_sync();
    if(status!=200) {
        std::string detail = status == 0 ? "Could not connect to Home Assistant" :
            "Home Assistant returned HTTP " + std::to_string(status);
        if (!transport_error.empty()) detail += ": " + transport_error;
        try { auto body=nlohmann::json::parse(response); detail += ": " + body.at("error").get<std::string>(); } catch(...){}
        if (posting) detail += ". If the write was sent, retry its saved request to reconcile the outcome.";
        throw HaInventoryRequestError(status,detail);
    }
    return nlohmann::json::parse(response);
    });
}

inline nlohmann::json ha_inventory_request_raw(const std::string &endpoint, const nlohmann::json *payload = nullptr,
                                              size_t maximum_bytes = 1024 * 1024) {
    return ha_inventory_request_raw_with_token(endpoint, ha_inventory_token(endpoint), payload, maximum_bytes);
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
