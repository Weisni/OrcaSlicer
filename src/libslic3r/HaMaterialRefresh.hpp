#pragma once
#include <cstdint>
#include <string>
#include "nlohmann/json.hpp"

namespace Slic3r::HaMaterialRefresh {
class DisplayRevision {
public:
    // A stock-history/transaction revision is not necessarily a displayed material
    // change. Never copy the native inventory graph just to decide whether to paint.
    bool update(const std::string &source, const nlohmann::json &snapshot, const std::string &error)
    {
        using Json = nlohmann::json;
        Json view = {{"source", source}, {"error", error}, {"slots", Json::array()}, {"spools", Json::object()}};
        if (snapshot.is_object()) {
            for (const auto *key : {"printer_id", "provider_printer_id"})
                if (snapshot.contains(key)) view[key] = snapshot.at(key);
            if (snapshot.contains("slots")) for (const auto &slot : snapshot.at("slots")) {
                Json item = Json::object();
                for (const auto *key : {"id", "spool_uuid"})
                    if (slot.contains(key)) item[key] = slot.at(key);
                view["slots"].push_back(std::move(item));
            }
            if (snapshot.contains("spools")) for (const auto &spool : snapshot.at("spools")) {
                Json item = Json::object();
                for (const auto *key : {"uuid", "manufacturer", "product", "material_type", "color",
                                       "material_preset", "status", "remaining_mg"})
                    if (spool.contains(key)) item[key] = spool.at(key);
                view["spools"][spool.at("uuid").get<std::string>()] = std::move(item);
            }
        }
        if (view == m_view) return false;
        m_view = std::move(view);
        ++m_generation;
        return true;
    }
    std::uint64_t generation() const { return m_generation; }
private:
    nlohmann::json m_view;
    std::uint64_t m_generation = 0;
};
inline bool should_rebuild(std::uint64_t current, std::uint64_t rendered, bool picker_open)
{
    return current != rendered && !picker_open;
}
}
