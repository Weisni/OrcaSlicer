#pragma once
#include "HaMaterialSource.hpp"
#include <tuple>

namespace Slic3r::HaMaterialCatalog {
struct Roll {
    HaMaterialSource::Assignment assignment;
    int64_t remaining_mg = 0;
};
inline std::vector<Roll> available(const nlohmann::json &snapshot)
{
    const auto slots = HaMaterialSource::parse_snapshot(snapshot, snapshot.at("printer_id"));
    std::vector<Roll> result;
    for (const auto &spool : snapshot.at("spools")) {
        const auto status = spool.at("status").get<std::string>();
        const auto remaining = spool.at("remaining_mg").get<int64_t>();
        if (status == "archived" || status == "empty" || remaining <= 0) continue;
        if (status != "active") throw std::runtime_error("Unknown HA roll status");
        const auto uuid = spool.at("uuid").get<std::string>();
        const auto found = std::find_if(slots.begin(), slots.end(), [&](const auto &a) { return a.spool_uuid == uuid; });
        if (found != slots.end()) {
            result.push_back({*found, remaining});
        } else {
            // Reuse mounted-roll validation without serializing the ledger or
            // constructing a synthetic snapshot for each inventory entry.
            result.push_back({HaMaterialSource::assignment_from_spool(
                spool, "", snapshot.at("revision").get<int>()), remaining});
        }
    }
    const std::vector<std::string> order {"A1", "A2", "A3", "A4", "HT1", "EXT", ""};
    std::sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
        const auto &x = a.assignment, &y = b.assignment;
        const auto px = std::find(order.begin(), order.end(), x.slot), py = std::find(order.begin(), order.end(), y.slot);
        if (px != py) return px < py;
        return std::tie(x.manufacturer, x.product, x.spool_uuid) < std::tie(y.manufacturer, y.product, y.spool_uuid);
    });
    return result;
}
}
