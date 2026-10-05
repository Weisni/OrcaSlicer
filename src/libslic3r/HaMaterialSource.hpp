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
#include "HaMaterialContext.hpp"

namespace Slic3r::HaMaterialSource {

// ASA+ is a product designation in HA, while the slicer uses the ASA family.
// Keep this deliberate exception narrow; other modified materials stay distinct.
inline std::string material_family(const std::string &type) { return type == "ASA+" ? "ASA" : type; }
inline bool same_material_family(const std::string &a, const std::string &b)
{
    return !a.empty() && !b.empty() && material_family(a) == material_family(b);
}

inline bool supports_profile_variants(const nlohmann::json &snapshot)
{
    const auto capabilities = snapshot.find("capabilities");
    return capabilities != snapshot.end() && capabilities->is_object() &&
        capabilities->value("material_profile_variants_v1", false);
}

inline nlohmann::json profile_context(const DynamicPrintConfig &printer, const DynamicPrintConfig &project)
{
    const auto *diameters = printer.option<ConfigOptionFloats>("nozzle_diameter");
    // The native flow selector stores the active flow in project_config.
    // A printer preset's value is only the initial default for older projects.
    const auto *flows = project.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (!flows) flows = printer.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (!diameters || diameters->values.size() != 1 || !flows || flows->values.size() != 1 ||
        (flows->values[0] != 0 && flows->values[0] != 1))
        throw std::runtime_error("Choose one configured standard or high-flow nozzle before selecting HA profiles");
    return HaMaterialContext::make(printer.opt_string("printer_model"), diameters->values[0],
        flows->values[0] == 1 ? "high_flow" : "standard");
}

inline void validate_print_profile(const PresetWithVendorProfile &filament, const PresetWithVendorProfile &printer,
    const std::string &sliced_material_type, const std::string &prepared_context, const nlohmann::json &active_context)
{
    if (prepared_context.empty() || prepared_context != HaMaterialContext::key(active_context))
        throw std::runtime_error("The printer or nozzle configuration changed; slice again and reopen the print dialog");
    if (!same_material_family(filament.preset.config.opt_string("filament_type", 0u), sliced_material_type) ||
        !is_compatible_with_printer(filament, printer))
        throw std::runtime_error("A selected filament profile is incompatible with this printer/nozzle; choose a compatible same-type profile and slice again");
}

struct Assignment {
    std::string slot, spool_uuid, manufacturer, product, material_type;
    std::string color, material_preset;
    int revision;
    nlohmann::json material_profile = nullptr;
    nlohmann::json material_profile_variants = nlohmann::json::object();
    std::string profile_context_key;
};

inline nlohmann::json variant_profile(const Assignment &assignment)
{
    if (assignment.profile_context_key.empty()) return assignment.material_profile;
    const auto found = assignment.material_profile_variants.find(assignment.profile_context_key);
    return found == assignment.material_profile_variants.end() ? nlohmann::json(nullptr) : *found;
}

inline Assignment with_context(Assignment assignment, const nlohmann::json &context)
{
    assignment.profile_context_key = HaMaterialContext::key(context);
    const auto profile = variant_profile(assignment);
    if (!profile.is_null()) {
        assignment.material_profile = profile;
        assignment.material_preset = profile.at("name").get<std::string>();
    }
    return assignment;
}

// Resolve locally without overwriting HA's special-profile association.
// Generic fallback never derives a material type from a printer substitute.
inline const Preset *resolve_preset(const PresetCollection &filaments,
                                   const std::string &requested, const std::string &material_type)
{
    if (material_type.empty()) return nullptr;
    if (!requested.empty()) {
        if (const auto *exact = filaments.find_preset(requested))
            return exact->is_compatible && same_material_family(exact->config.opt_string("filament_type", 0u), material_type) ? exact : nullptr;
    }
    const auto prefix = "Generic " + material_family(material_type);
    for (const auto &preset : filaments) {
        if (preset.is_system && preset.is_compatible &&
            same_material_family(preset.config.opt_string("filament_type", 0u), material_type) &&
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
            !same_material_family(assignment.material_profile.at("material_type"), assignment.material_type))
            throw std::runtime_error("HA material profile does not match its physical roll");
    }
    assignment.material_profile_variants = spool.value("material_profile_variants", nlohmann::json::object());
    if (!assignment.material_profile_variants.is_object() || assignment.material_profile_variants.size() > 64)
        throw std::runtime_error("Invalid HA nozzle profile variants");
    for (const auto &profile : assignment.material_profile_variants.items()) {
        const auto first = profile.key().find('|'), last = profile.key().rfind('|');
        if (first == std::string::npos || first == last ||
            HaMaterialContext::key({{"printer_model", profile.key().substr(0, first)},
                {"nozzle_diameter", profile.key().substr(first + 1, last - first - 1)},
                {"flow_type", profile.key().substr(last + 1)}}) != profile.key())
            throw std::runtime_error("Invalid HA nozzle profile variant key");
        HaMaterialProfile::digest(profile.value());
        if (!same_material_family(profile.value().at("material_type"), assignment.material_type))
            throw std::runtime_error("HA nozzle profile belongs to another material type");
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
