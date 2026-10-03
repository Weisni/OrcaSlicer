#pragma once

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"
#include "Preset.hpp"
#include "HaMaterialProfilePayload.hpp"

namespace Slic3r::HaMaterialSource {

struct Assignment {
    std::string slot, spool_uuid, manufacturer, product, material_type;
    std::string color, material_preset;
    int revision;
    nlohmann::json material_profile = nullptr;
};

// Resolve locally without overwriting HA's special-profile association.
// Generic fallback never derives a material type from a printer substitute.
inline const Preset *resolve_preset(const PresetCollection &filaments,
                                   const std::string &requested, const std::string &material_type)
{
    if (material_type.empty()) return nullptr;
    if (!requested.empty()) {
        if (const auto *exact = filaments.find_preset(requested))
            return exact->is_compatible && exact->config.opt_string("filament_type", 0u) == material_type ? exact : nullptr;
    }
    const auto prefix = "Generic " + material_type;
    for (const auto &preset : filaments) {
        if (preset.is_system && preset.is_compatible &&
            preset.config.opt_string("filament_type", 0u) == material_type &&
            preset.name.compare(0, prefix.size(), prefix) == 0 &&
            (preset.name.size() == prefix.size() || preset.name[prefix.size()] == ' ' || preset.name[prefix.size()] == '@'))
            return &preset;
    }
    return nullptr;
}

inline Assignment assignment_from_spool(const nlohmann::json &spool,
                                        const std::string &slot, int revision)
{
    Assignment assignment {slot, spool.at("uuid"), spool.at("manufacturer"), spool.at("product"),
        spool.at("material_type"), spool.at("color"), spool.at("material_preset"), revision};
    if (assignment.spool_uuid.empty())
        throw std::runtime_error("Duplicate or missing spool UUID");
    if (revision < 0)
        throw std::runtime_error("HA slot has no valid revision");
    if (assignment.material_type.empty())
        throw std::runtime_error("HA slot has no material type");
    assignment.material_profile = spool.value("material_profile", nlohmann::json());
    if (!assignment.material_profile.is_null()) {
        HaMaterialProfile::digest(assignment.material_profile);
        if (assignment.material_profile.at("name") != assignment.material_preset ||
            assignment.material_profile.at("material_type") != assignment.material_type)
            throw std::runtime_error("HA material profile does not match its physical roll");
    }
    if (assignment.color.size() != 7 || assignment.color.front() != '#' ||
        !std::all_of(assignment.color.begin() + 1, assignment.color.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }))
        throw std::runtime_error("HA material color must be #RRGGBB");
    return assignment;
}

// The HTTP boundary has already bounded and decoded this snapshot. Read only
// material fields here; unrelated native inventory/history must not be copied.
inline std::vector<Assignment> parse_snapshot(const nlohmann::json &root, const std::string &printer_id)
{
    if (root.at("schema_version") != 1 || root.at("demo_mode") != true || root.at("printer_id") != printer_id)
        throw std::runtime_error("HA demo schema, mode or printer identity does not match");
    const auto age = std::time(nullptr) - root.at("captured_unix").get<std::time_t>();
    if (age > 300 || age < -30)
        throw std::runtime_error("HA material snapshot is stale");
    std::map<std::string, const nlohmann::json *> spools;
    for (const auto &spool : root.at("spools")) {
        const auto id = spool.at("uuid").get<std::string>();
        if (id.empty() || !spools.emplace(id, &spool).second)
            throw std::runtime_error("Duplicate or missing spool UUID");
    }
    const std::vector<std::string> order {"A1", "A2", "A3", "A4", "HT1", "EXT"};
    std::set<std::string> seen_slots, seen_spools;
    std::vector<Assignment> result;
    for (const auto &slot : root.at("slots")) {
        const auto name = slot.at("id").get<std::string>();
        if (std::find(order.begin(), order.end(), name) == order.end() || !seen_slots.insert(name).second)
            throw std::runtime_error("Unknown or duplicate HA slot");
        const int revision = slot.at("revision").get<int>();
        if (revision < 0)
            throw std::runtime_error("HA slot has no valid revision");
        if (slot.at("spool_uuid").is_null()) {
            result.push_back({name, "", "", "", "", "", "", revision});
            continue;
        }
        const auto id = slot.at("spool_uuid").get<std::string>();
        if (!spools.count(id) || !seen_spools.insert(id).second)
            throw std::runtime_error("Missing spool identity or spool assigned twice");
        result.push_back(assignment_from_spool(*spools.at(id), name, revision));
    }
    std::sort(result.begin(), result.end(), [&order](const auto &a, const auto &b) {
        return std::find(order.begin(), order.end(), a.slot) < std::find(order.begin(), order.end(), b.slot);
    });
    return result;
}

inline std::vector<Assignment> parse(const std::string &body, const std::string &printer_id)
{
    if (body.size() > 1024 * 1024)
        throw std::runtime_error("HA demo snapshot exceeds 1 MiB");
    return parse_snapshot(nlohmann::json::parse(body), printer_id);
}

} // namespace Slic3r::HaMaterialSource
