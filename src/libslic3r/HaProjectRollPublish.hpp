#pragma once

#include "HaProjectMaterialSync.hpp"

namespace Slic3r::HaProjectRollPublish {
using Json = nlohmann::json;

struct RollPublish {
    size_t project_index = 0;
    std::string uuid;
    std::int64_t remaining_mg = -1; // A quantity must be explicitly supplied.
    bool create = false;
    std::string name, manufacturer, material_type;
    std::int64_t nominal_mg = 1000000;
    bool color_only = false, profile_only = false;
    std::string expected_profile_sha256;
};

inline void require_keys(const Json &value, const std::set<std::string> &allowed)
{
    if (!value.is_object()) throw std::runtime_error("Invalid project roll transfer");
    for (auto item = value.begin(); item != value.end(); ++item)
        if (!allowed.count(item.key())) throw std::runtime_error("Project roll transfer contains an unrelated field");
}

inline bool canonical_uuid(const std::string &value)
{
    if (value.size() != 36) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        if (separator ? value[i] != '-' : std::string("0123456789abcdef").find(value[i]) == std::string::npos)
            return false;
    }
    return true;
}

inline void require_text(const Json &value, size_t maximum, bool allow_empty = false)
{
    if (!value.is_string()) throw std::runtime_error("Project roll text is invalid");
    const auto text = value.get<std::string>();
    if (text.size() > maximum || (!allow_empty && text.find_first_not_of(" \t\r\n") == std::string::npos))
        throw std::runtime_error("Provide a valid roll name, material and installed profile");
}

inline std::int64_t checked_weight(const Json &value)
{
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > 10000000000ULL))
        throw std::runtime_error("Stock must be a nonnegative number of whole milligrams");
    const auto result = value.get<std::int64_t>();
    if (result < 0 || result > 10000000000LL)
        throw std::runtime_error("Stock must be a nonnegative number of whole milligrams");
    return result;
}

inline void require_color(const Json &value)
{
    require_text(value, 7);
    const auto color = value.get<std::string>();
    if (color.size() != 7 || color[0] != '#' || color.find_first_not_of("0123456789aAbBcCdDeEfF", 1) != std::string::npos)
        throw std::runtime_error("Use a #RRGGBB filament color");
}

// Validate persisted requests as strictly as newly prepared ones. A journal
// cannot broaden a reviewed project transfer into unrelated inventory edits.
inline void validate_changes(const Json &changes)
{
    if (!changes.is_array() || changes.empty() || changes.size() > 100)
        throw std::runtime_error("Select between one and 100 physical rolls");
    std::set<std::string> seen;
    for (const auto &change : changes) {
        require_keys(change, {"spool_uuid", "fields", "expected", "create", "remaining_mg", "expected_remaining_mg", "quality", "material_profile", "expected_profile_sha256"});
        require_text(change.at("spool_uuid"), 36);
        const auto uuid = change.at("spool_uuid").get<std::string>();
        if (!canonical_uuid(uuid) || !seen.insert(uuid).second)
            throw std::runtime_error("Select each canonical physical roll UUID only once");
        if (change.contains("create") && !change.at("create").is_boolean())
            throw std::runtime_error("Invalid roll creation flag");
        const bool create = change.value("create", false);
        const auto &fields = change.at("fields"), &expected = change.at("expected");
        const std::set<std::string> editable = create ? std::set<std::string>{"name", "manufacturer", "material_type", "nominal_capacity_mg", "filament_preset_id", "color_hex", "status"}
                                                     : std::set<std::string>{"filament_preset_id", "color_hex"};
        require_keys(fields, editable);
        require_keys(expected, create ? std::set<std::string>{} : editable);
        if (!create && expected.size() != fields.size())
            throw std::runtime_error("Every selected field needs its HA baseline");
        for (auto field = fields.begin(); field != fields.end(); ++field) {
            if (field.key() == "color_hex") require_color(field.value());
            else if (field.key() == "nominal_capacity_mg") {
                if (!checked_weight(field.value())) throw std::runtime_error("Nominal capacity must be positive");
            } else require_text(field.value(), field.key() == "material_type" ? 40 : 256, field.key() == "manufacturer");
            if (!create) require_text(expected.at(field.key()), 256, true);
        }
        if (change.contains("material_profile")) {
            const auto &profile = change.at("material_profile");
            HaMaterialProfile::validate(profile);
            if (!fields.contains("filament_preset_id") || fields.at("filament_preset_id") != profile.at("name"))
                throw std::runtime_error("Material settings require the matching selected profile association");
            const auto &baseline = change.at("expected_profile_sha256");
            if (!baseline.is_null() && !HaMaterialProfile::is_sha256(baseline))
                throw std::runtime_error("Material settings need a valid HA digest baseline");
        } else if (change.contains("expected_profile_sha256"))
            throw std::runtime_error("A material digest baseline requires complete settings");
        const bool stock = change.contains("remaining_mg");
        if (!stock && (fields.empty() || change.contains("quality") || change.contains("expected_remaining_mg")))
            throw std::runtime_error("Stock provenance requires an explicit stock transfer");
        if (stock) {
            checked_weight(change.at("remaining_mg"));
            if (change.at("quality") != "estimated")
                throw std::runtime_error("Quack stock transfers must retain estimated provenance");
            if (create) {
                if (change.contains("expected_remaining_mg")) throw std::runtime_error("A new roll has no previous stock baseline");
            } else checked_weight(change.at("expected_remaining_mg"));
        }
        if (create) {
            if (fields.size() != editable.size() || !stock)
                throw std::runtime_error("New rolls require complete metadata and explicit stock");
            const auto amount = checked_weight(change.at("remaining_mg"));
            if (amount > checked_weight(fields.at("nominal_capacity_mg")) || fields.at("status") != (amount ? "active" : "empty"))
                throw std::runtime_error("New roll stock contradicts its capacity or status");
        }
    }
}

inline void validate_payload(const Json &payload)
{
    require_keys(payload, {"revision", "request_key", "confirmed", "changes", "concurrency", "response"});
    if (payload.contains("concurrency") && payload.at("concurrency") != "fields")
        throw std::runtime_error("Invalid project transfer concurrency mode");
    if (payload.contains("response") && payload.at("response") != "ack")
        throw std::runtime_error("Invalid project transfer response mode");
    checked_weight(payload.at("revision"));
    require_text(payload.at("request_key"), 128);
    if (!payload.at("confirmed").is_boolean() || payload.at("confirmed") != true)
        throw std::runtime_error("An explicit confirmed project transfer is required");
    validate_changes(payload.at("changes"));
    if (payload.dump().size() > 2000000) throw std::runtime_error("Project transfer exceeds the HA size limit");
}

// Only the selected physical rolls are changed. Current HA stock is the
// baseline; project profiles never imply a roll's quantity or identity.
inline Json publish_roll_changes(const HaProjectMaterialSync::Project &project,
                                 const Json &remote, const std::vector<RollPublish> &selected)
{
    if (project.presets.empty() || project.presets.size() != project.colors.size())
        throw std::runtime_error("Project filament colors and presets do not match");
    if (!project.profiles.empty() && project.profiles.size() != project.presets.size())
        throw std::runtime_error("Project material profiles do not match its filaments");
    if (selected.size() > 100) throw std::runtime_error("Select at most 100 physical rolls");
    Json changes = Json::array();
    std::set<std::string> seen;
    for (const auto &item : selected) {
        if (!canonical_uuid(item.uuid) || !seen.insert(item.uuid).second)
            throw std::runtime_error("Select each canonical physical roll UUID only once");
        if (item.project_index >= project.presets.size() || (item.color_only && item.profile_only))
            throw std::runtime_error("Select an existing project filament and a valid field scope");
        const auto amount = checked_weight(item.remaining_mg);
        const auto *target = HaInventorySelection::spool(remote, item.uuid);
        if (item.create ? target != nullptr : target == nullptr)
            throw std::runtime_error("The HA roll identity changed; refresh the comparison");
        Json change = {{"spool_uuid", item.uuid}, {"fields", Json::object()}, {"expected", Json::object()}};
        auto &fields = change["fields"];
        if (item.create) {
            fields = {{"name", item.name}, {"manufacturer", item.manufacturer}, {"material_type", item.material_type},
                {"nominal_capacity_mg", item.nominal_mg}, {"filament_preset_id", project.presets.at(item.project_index)},
                {"color_hex", project.colors.at(item.project_index)}, {"status", amount ? "active" : "empty"}};
            change["create"] = true;
        } else {
            const auto add = [&](const std::string &field, const std::string &value) {
                if (target->at(field) != value) {
                    fields[field] = value;
                    change["expected"][field] = target->at(field);
                }
            };
            if (!item.color_only) add("filament_preset_id", project.presets.at(item.project_index));
            if (!item.profile_only) add("color_hex", project.colors.at(item.project_index));
        }
        if (!item.color_only && !project.profiles.empty()) {
            const auto &profile = project.profiles.at(item.project_index);
            HaMaterialProfile::validate(profile);
            fields["filament_preset_id"] = profile.at("name");
            if (!item.create) change["expected"]["filament_preset_id"] = target->at("filament_preset_id");
            change["material_profile"] = profile;
            change["expected_profile_sha256"] = item.expected_profile_sha256.empty()
                ? Json(nullptr) : Json(item.expected_profile_sha256);
        }
        const auto baseline = item.create ? 0 : checked_weight(HaInventorySelection::balance(remote, item.uuid));
        if (item.create || amount != baseline) {
            if (!item.create) {
                if (amount > checked_weight(target->at("nominal_capacity_mg")))
                    throw std::runtime_error("Remaining stock exceeds the HA roll's nominal capacity");
                if (target->at("status") == "archived" || HaInventorySelection::open_allocation(remote, item.uuid))
                    throw std::runtime_error("Restore the roll and resolve open jobs before replacing its stock");
                change["expected_remaining_mg"] = baseline;
            }
            change["remaining_mg"] = amount;
            change["quality"] = "estimated";
        }
        if (!fields.empty() || change.contains("remaining_mg")) changes.push_back(std::move(change));
    }
    if (!changes.empty()) validate_changes(changes);
    return changes;
}
} // namespace Slic3r::HaProjectRollPublish
