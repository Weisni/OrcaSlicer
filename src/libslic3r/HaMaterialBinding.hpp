#pragma once

#include "HaMaterialSource.hpp"
#include "HaMaterialProfile.hpp"
#include "PrintConfig.hpp"
#include <cstdint>
#include <cctype>

namespace Slic3r::HaMaterialBinding {

inline constexpr const char *config_key = "ha_material_bindings";

struct Binding {
    std::string spool_uuid, source, printer_id;
    std::string profile_context, profile_name, profile_sha256;
};

inline bool preserves_override(const Binding &binding, const Preset *effective)
{
    return binding.profile_context.empty() || !effective || effective->name != binding.profile_name ||
        HaMaterialProfile::digest(HaMaterialProfile::capture(*effective)) != binding.profile_sha256;
}
struct Usage {
    size_t project_index;
    std::string material_type;
    int64_t estimated_mg = 0;
};
struct LiveSlot {
    std::string slot, ams_id, slot_id;
    int tray_id = -1;
    bool present = false;
    std::string material_type, color;
};
struct Resolved {
    size_t project_index;
    std::string spool_uuid, slot, ams_id, slot_id;
    int tray_id;
    int64_t estimated_mg;
    int revision;
    std::string color;
};

inline void validate(const Binding &binding)
{
    if (binding.spool_uuid.empty() && binding.source.empty() && binding.printer_id.empty() &&
        binding.profile_context.empty() && binding.profile_name.empty() && binding.profile_sha256.empty()) return;
    const auto &id = binding.spool_uuid;
    if (id.size() != 36 || binding.source.empty() || binding.source.size() > 2048 ||
        binding.printer_id.empty() || binding.printer_id.size() > 128)
        throw std::runtime_error("Invalid HA material binding");
    for (size_t i = 0; i < id.size(); ++i) {
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        if (separator ? id[i] != '-' : std::string("0123456789abcdef").find(id[i]) == std::string::npos)
            throw std::runtime_error("Invalid HA roll UUID");
    }
    if (!binding.profile_context.empty() && (binding.profile_context.size() > 192 ||
        binding.profile_name.empty() || binding.profile_name.size() > 256 ||
        !HaMaterialProfile::is_sha256(binding.profile_sha256)))
        throw std::runtime_error("Invalid HA project profile baseline");
    if (binding.profile_context.empty() && (!binding.profile_name.empty() || !binding.profile_sha256.empty()))
        throw std::runtime_error("Incomplete HA project profile baseline");
}

inline std::vector<Binding> read(const DynamicPrintConfig &config, size_t count)
{
    if (count > 1024) throw std::runtime_error("Too many project material bindings");
    std::vector<Binding> bindings(count);
    const auto *option = config.option<ConfigOptionStrings>(config_key);
    if (!option) return bindings;
    for (size_t i = 0; i < std::min(count, option->values.size()); ++i) {
        if (option->values[i].empty()) continue;
        if (option->values[i].size() > 4096) throw std::runtime_error("Invalid HA material binding size");
        const auto value = nlohmann::json::parse(option->values[i]);
        if (!value.is_object() || (value.size() != 3 && value.size() != 6)) throw std::runtime_error("Invalid HA material binding fields");
        bindings[i] = {value.at("spool_uuid"), value.at("source"), value.at("printer_id")};
        if (value.size() == 6) {
            bindings[i].profile_context = value.at("profile_context");
            bindings[i].profile_name = value.at("profile_name");
            bindings[i].profile_sha256 = value.at("profile_sha256");
        }
        validate(bindings[i]);
    }
    return bindings;
}

inline void write(DynamicPrintConfig &config, const std::vector<Binding> &bindings)
{
    std::vector<std::string> values;
    for (const auto &binding : bindings) {
        validate(binding);
        nlohmann::json value = {{"spool_uuid", binding.spool_uuid}, {"source", binding.source}, {"printer_id", binding.printer_id}};
        if (!binding.profile_context.empty()) {
            value["profile_context"] = binding.profile_context;
            value["profile_name"] = binding.profile_name;
            value["profile_sha256"] = binding.profile_sha256;
        }
        values.push_back(binding.spool_uuid.empty() ? std::string() : value.dump());
    }
    config.set_key_value(config_key, new ConfigOptionStrings(std::move(values)));
}
inline void set(DynamicPrintConfig &config, size_t index, const Binding &binding, size_t count)
{
    if (index >= count) throw std::runtime_error("Invalid project material index");
    auto bindings = read(config, count);
    bindings[index] = binding;
    write(config, bindings);
}
inline void resize(DynamicPrintConfig &config, size_t count) { write(config, read(config, count)); }
inline void erase(DynamicPrintConfig &config, size_t index, size_t old_count)
{
    if (index >= old_count) throw std::runtime_error("Invalid project material removal");
    auto bindings = read(config, old_count);
    bindings.erase(bindings.begin() + index);
    write(config, bindings);
}
inline void remap(DynamicPrintConfig &config, const std::vector<size_t> &new_to_old, size_t old_count)
{
    const auto original = read(config, old_count);
    std::vector<Binding> bindings;
    for (const auto index : new_to_old) {
        if (index >= original.size()) throw std::runtime_error("Invalid project material remapping");
        bindings.push_back(original[index]);
    }
    write(config, bindings);
}

inline std::string rgb(std::string value)
{
    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    if (value.size() == 8) value.resize(6);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return value;
}

inline std::map<std::string, int64_t> reserved_stock(const nlohmann::json &snapshot,
    const std::string &job_uuid, const std::string &target_device)
{
    std::map<std::string, int64_t> result;
    if (job_uuid.empty()) return result;
    const auto fail = [] { throw std::runtime_error("HA no longer authorizes this reserved print; reconcile its status before retrying"); };
    if (snapshot.contains("native_bundle")) {
        const auto &tables = snapshot.at("native_bundle").at("tables");
        const auto &jobs = tables.at("print_jobs");
        const auto job = std::find_if(jobs.begin(), jobs.end(), [&](const auto &item) { return item.at("id") == job_uuid; });
        if (job == jobs.end() || job->at("state") != "reserved" || job->at("printer_id") != target_device) fail();
        for (const auto &allocation : tables.at("allocations"))
            if (allocation.at("job_id") == job_uuid)
                result[allocation.at("spool_id").template get<std::string>()] += allocation.at("estimated_weight_mg").template get<int64_t>();
    } else if (snapshot.contains("jobs")) {
        const auto &jobs = snapshot.at("jobs");
        const auto job = std::find_if(jobs.begin(), jobs.end(), [&](const auto &item) { return item.at("uuid") == job_uuid; });
        if (job == jobs.end() || job->at("state") != "reserved" || !job->at("settlement").is_null()) fail();
        for (const auto &allocation : job->at("allocations"))
            result[allocation.at("spool_uuid").template get<std::string>()] += allocation.at("weight_mg").template get<int64_t>();
    } else fail();
    return result;
}

inline std::vector<Resolved> preflight(const std::vector<Binding> &bindings, const std::vector<Usage> &usages,
    const nlohmann::json &snapshot, const std::string &source, const std::string &paired_device,
    const std::string &target_device, const std::vector<LiveSlot> &live,
    const std::string &reserved_job_uuid = {})
{
    if (paired_device.empty() || paired_device != target_device)
        throw std::runtime_error("Select the physical printer paired with this HA material source");
    const auto printer = snapshot.at("printer_id").get<std::string>();
    const auto slots = HaMaterialSource::parse_snapshot(snapshot, printer);
    const auto reserved = reserved_stock(snapshot, reserved_job_uuid, target_device);
    if (usages.empty()) throw std::runtime_error("No sliced material usage is available for HA preflight");
    std::set<size_t> seen;
    std::map<std::string, int64_t> totals;
    std::vector<Resolved> resolved;
    for (const auto &usage : usages) {
        if (usage.project_index >= bindings.size() || !seen.insert(usage.project_index).second ||
            usage.estimated_mg < 0 || usage.estimated_mg > 10000000000LL)
            throw std::runtime_error("Invalid sliced material usage");
        const auto &binding = bindings[usage.project_index];
        validate(binding);
        if (binding.spool_uuid.empty() || binding.source != source || binding.printer_id != printer)
            throw std::runtime_error("Select an HA roll for every material used by this plate");
        const auto slot = std::find_if(slots.begin(), slots.end(), [&](const auto &item) { return item.spool_uuid == binding.spool_uuid; });
        if (slot == slots.end()) throw std::runtime_error("A selected HA roll is not assigned to a printer slot");
        const auto &spools = snapshot.at("spools");
        const auto spool = std::find_if(spools.begin(), spools.end(), [&](const auto &item) { return item.at("uuid") == binding.spool_uuid; });
        if (spool == spools.end() || spool->value("status", "active") != "active" || spool->at("remaining_mg").template get<int64_t>() <= 0)
            throw std::runtime_error("A selected HA roll is archived or empty");
        const auto device = std::find_if(live.begin(), live.end(), [&](const auto &item) { return item.slot == slot->slot; });
        if (device == live.end() || !device->present || device->tray_id < 0 || device->ams_id.empty() || device->slot_id.empty())
            throw std::runtime_error("The assigned HA slot is not available on the selected printer");
        if (!HaMaterialSource::same_material_family(usage.material_type, slot->material_type) ||
            !HaMaterialSource::same_material_family(device->material_type, slot->material_type))
            throw std::runtime_error("Sliced material, physical slot and HA roll types do not match");
        // The printer may report an approximation of the real color. Identity
        // comes from the fresh HA UUID-to-slot assignment, not that RGB value.
        // Keep the authoritative HA color for display without changing telemetry.
        totals[binding.spool_uuid] += usage.estimated_mg;
        int64_t available = spool->value("available_mg", spool->at("remaining_mg").template get<int64_t>());
        const auto credit = reserved.find(binding.spool_uuid);
        if (credit != reserved.end()) available += credit->second;
        if (totals[binding.spool_uuid] > available)
            throw std::runtime_error("The HA roll has insufficient available stock for this plate");
        resolved.push_back({usage.project_index,binding.spool_uuid,slot->slot,device->ams_id,device->slot_id,
            device->tray_id,usage.estimated_mg,slot->revision,slot->color});
    }
    return resolved;
}

} // namespace Slic3r::HaMaterialBinding
