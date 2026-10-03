#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"

namespace Slic3r::HaInventoryTransport {
using Json = nlohmann::json;
using PageReader = std::function<Json(const std::string &, size_t, std::int64_t)>;
class RevisionChanged : public std::runtime_error {
public:
    RevisionChanged() : std::runtime_error("HA inventory changed during the paged read; refresh again") {}
};
inline constexpr size_t max_bundle_bytes = 64 * 1024 * 1024;
inline constexpr size_t max_bundle_rows = 250000;
inline const std::vector<std::string> tables = {"inventory_settings", "spools", "spool_identifiers", "customers",
    "customer_orders", "print_jobs", "allocations", "job_identifiers", "stock_events", "print_job_manual_overrides"};

inline std::string row_key(const std::string &table, const Json &row)
{
    if (!row.is_object() || row.empty()) throw std::runtime_error("Invalid HA inventory row");
    const std::vector<std::string> keys = table == "spool_identifiers" ? std::vector<std::string>{"kind", "value"} :
        table == "job_identifiers" ? std::vector<std::string>{"provider", "kind", "value"} :
        table == "print_job_manual_overrides" ? std::vector<std::string>{"job_id"} : std::vector<std::string>{"id"};
    Json identity = Json::array();
    for (const auto &key : keys) {
        const auto &value = row.at(key);
        if (table == "inventory_settings" && key == "id") {
            if (!value.is_number_integer() || value != 1) throw std::runtime_error("Invalid HA inventory settings identity");
        } else if (!value.is_string() || value.get_ref<const std::string &>().empty())
            throw std::runtime_error("HA inventory row has no stable identity");
        identity.push_back(value);
    }
    return identity.dump();
}

inline size_t bounded_count(const Json &value, size_t maximum)
{
    if (!value.is_number_integer() || value < 0 || value > maximum)
        throw std::runtime_error("HA inventory exceeds its supported resource budget");
    return value.get<size_t>();
}

// A partially read graph never escapes this function. Every page belongs to
// the same revision and contiguous cursor range, including empty tables.
inline Json read_bundle(std::int64_t revision, const PageReader &read)
{
    if (revision < 0) throw std::runtime_error("Invalid HA inventory revision");
    Json bundle = {{"schema_version", 8}, {"tables", Json::object()}};
    size_t bytes = 0, count = 0;
    for (const auto &table : tables) {
        Json rows = Json::array();
        size_t cursor = 0, total = 0;
        std::set<std::string> identities;
        do {
            auto page = read(table, cursor, revision);
            if (page.at("revision") != revision) throw RevisionChanged();
            if (page.at("schema_version") != 8 || page.at("table") != table)
                throw std::runtime_error("HA inventory page belongs to a different schema or table");
            const auto &items = page.at("rows");
            if (!items.is_array() || items.size() > 200 || page.dump().size() > 256 * 1024)
                throw std::runtime_error("Invalid or oversized HA inventory page");
            const auto page_total = bounded_count(page.at("total"), max_bundle_rows);
            if (cursor == 0) total = page_total;
            if (page_total != total || cursor + items.size() > total)
                throw std::runtime_error("HA inventory pagination is inconsistent");
            for (const auto &row : items) {
                if (!identities.insert(row_key(table, row)).second)
                    throw std::runtime_error("HA inventory contains duplicate row identities");
                bytes += row.dump().size();
                if (bytes > max_bundle_bytes || ++count > max_bundle_rows)
                    throw std::runtime_error("HA inventory exceeds its supported resource budget");
                rows.push_back(row);
            }
            cursor += items.size();
            const auto &next = page.at("next_cursor");
            if (next.is_null()) {
                if (cursor != total) throw std::runtime_error("HA inventory page is incomplete");
                break;
            }
            if (items.empty() || bounded_count(next, total) != cursor || cursor == total)
                throw std::runtime_error("HA inventory cursor did not advance consistently");
        } while (true);
        bundle["tables"][table] = std::move(rows);
    }
    if (bundle.dump().size() > max_bundle_bytes) throw std::runtime_error("HA inventory exceeds its supported resource budget");
    return bundle;
}

inline Json read_snapshot(const std::function<Json()> &metadata, const PageReader &read)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto snapshot = metadata();
        if (!snapshot.at("revision").is_number_integer()) throw std::runtime_error("Invalid HA inventory revision");
        try {
            snapshot["native_bundle"] = read_bundle(snapshot.at("revision").get<std::int64_t>(), read);
            return snapshot;
        } catch (const RevisionChanged &) {
            if (attempt == 2) throw;
        }
    }
    throw RevisionChanged();
}

inline std::map<std::string, const Json *> index(const std::string &table, const Json &rows)
{
    if (!rows.is_array() || rows.size() > max_bundle_rows) throw std::runtime_error("Invalid HA inventory table");
    std::map<std::string, const Json *> result;
    for (const auto &row : rows)
        if (!result.emplace(row_key(table, row), &row).second) throw std::runtime_error("Duplicate HA inventory row identity");
    return result;
}

inline Json without_timestamp(Json row)
{
    row.erase("updated_at");
    return row;
}

// Detect edits against the local pre-transaction image, but retain HA's full
// expected row and any server fields this local transaction did not touch.
inline Json changes(const Json &remote, const Json &before, const Json &after)
{
    for (const auto *bundle : {&remote, &before, &after})
        if (bundle->at("schema_version") != 8 || bundle->at("tables").size() != tables.size())
            throw std::runtime_error("HA inventory delta schema mismatch");
    Json result = Json::array();
    for (const auto &table : tables) {
        const auto authoritative = index(table, remote.at("tables").at(table));
        const auto old_rows = index(table, before.at("tables").at(table));
        const auto new_rows = index(table, after.at("tables").at(table));
        std::set<std::string> keys;
        for (const auto &[key, row] : old_rows) keys.insert(key);
        for (const auto &[key, row] : new_rows) keys.insert(key);
        for (const auto &key : keys) {
            const auto old = old_rows.find(key), current = new_rows.find(key), server = authoritative.find(key);
            const bool existed = old != old_rows.end(), exists = current != new_rows.end();
            if (existed && exists && without_timestamp(*old->second) == without_timestamp(*current->second)) continue;
            if (existed ? server == authoritative.end() : server != authoritative.end())
                throw std::runtime_error("The HA row identity changed; refresh before editing");
            Json next = nullptr;
            if (exists) {
                if (!existed) next = *current->second;
                else {
                    next = *server->second;
                    for (auto field = current->second->begin(); field != current->second->end(); ++field)
                        if (!old->second->contains(field.key()) || old->second->at(field.key()) != field.value()) next[field.key()] = field.value();
                    for (auto field = old->second->begin(); field != old->second->end(); ++field)
                        if (!current->second->contains(field.key())) next.erase(field.key());
                }
            }
            result.push_back({{"table", table}, {"before", existed ? *server->second : Json(nullptr)}, {"after", std::move(next)}});
            if (result.size() > 1000) throw std::runtime_error("Split this inventory operation into fewer than 1001 changed rows");
        }
    }
    // Server receipt canonicalization escapes non-ASCII text and adds spaces.
    // Pretty ASCII JSON is a conservative upper bound for that representation;
    // retain headroom for request identity/revision outside this changes array.
    if (result.dump(1, ' ', true).size() > 990000)
        throw std::runtime_error("Split this inventory operation into smaller row changes");
    return result;
}
} // namespace Slic3r::HaInventoryTransport
