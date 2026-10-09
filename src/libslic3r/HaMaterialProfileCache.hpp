#pragma once
#include "HaMaterialProfilePayload.hpp"
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <mutex>
namespace Slic3r::HaMaterialProfile {
struct PublishedContent {
    Json summary;
    std::string spool, context;
};
inline std::vector<PublishedContent> published_contents(const Json& snapshot)
{
    std::map<std::string, PublishedContent> contents;
    const auto add = [&](const Json& summary, const std::string& spool = {}, const std::string& context = {}) {
        if (summary.is_null()) return;
        const auto hash = digest(summary);
        const auto found = contents.find(hash);
        if (found == contents.end()) contents.emplace(hash, PublishedContent{summary,spool,context});
        else {
            const auto normalize = [](const Json& profile) { return profile.contains("settings") ? profile_summary(profile) : profile; };
            if (normalize(found->second.summary) != normalize(summary))
                throw std::runtime_error("Conflicting HA profile summaries use the same content digest");
            if (!spool.empty() && found->second.spool.empty()) found->second = {summary,spool,context};
        }
    };
    if (snapshot.contains("profiles")) {
        if (!snapshot.at("profiles").is_array()) throw std::runtime_error("Invalid HA profile catalog");
        for (const auto& summary : snapshot.at("profiles")) add(summary);
    }
    if (snapshot.contains("spools")) for (const auto& spool : snapshot.at("spools")) {
        const auto id = spool.at("uuid").get<std::string>();
        if (spool.contains("material_profile")) add(spool.at("material_profile"),id);
        if (spool.contains("material_profile_variants"))
            for (const auto& variant : spool.at("material_profile_variants").items()) add(variant.value(),id,variant.key());
    }
    if (contents.size() > 250000) throw std::runtime_error("The HA profile catalog exceeds the supported limit");
    std::vector<PublishedContent> result;
    for (auto& content : contents) result.push_back(std::move(content.second));
    return result;
}
class PersistentCache {
public:
    PersistentCache(std::filesystem::path root, std::string schema, std::set<std::string> keys = {})
        : m_root(std::move(root)), m_schema(content_sha256(schema)), m_keys(std::move(keys))
    {
#ifdef _WIN32
        // Full digest namespaces can exceed MAX_PATH below redirected user
        // directories. Preserve Unicode and full hashes with the native prefix.
        auto absolute = std::filesystem::absolute(m_root).wstring();
        if (absolute.compare(0, 4, L"\\\\?\\") != 0)
            m_root = absolute.compare(0, 2, L"\\\\") == 0 ?
                L"\\\\?\\UNC\\" + absolute.substr(2) : L"\\\\?\\" + absolute;
#endif
    }
    Json find(const std::string& endpoint, const Json& summary)
    {
        const auto path = entry_path(endpoint, summary);
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto cached = m_payloads.find(path);
        if (cached != m_payloads.end()) return cached->second;
        try {
            if (!std::filesystem::exists(path) || std::filesystem::file_size(path) > 262144) return nullptr;
            std::ifstream file(path, std::ios::binary);
            const auto payload = Json::parse(file);
            verify(payload, summary);
            remember(path,payload);
            return payload;
        } catch (const std::exception&) { return nullptr; }
    }
    Json get(const std::string& endpoint, const Json& summary, const std::function<Json()>& fetch)
    {
        if (auto payload = find(endpoint, summary); !payload.is_null()) return payload;
        // Network work must never hold the cache or GUI mutex.
        const auto payload = fetch();
        verify(payload, summary);
        const auto path = entry_path(endpoint, summary);
        std::lock_guard<std::mutex> lock(m_mutex);
        std::filesystem::create_directories(path.parent_path());
        auto pending = path;
        pending += ".pending";
        {
            std::ofstream file(pending, std::ios::binary | std::ios::trunc);
            file << payload.dump();
            file.flush();
            if (!file) throw std::runtime_error("Cannot persist the HA material content cache");
        }
        // Entries are immutable; an existing valid entry can be reused. Only a
        // corrupt entry is removed before repairing it (Windows rename semantics).
        if (std::filesystem::exists(path)) {
            bool valid = false;
            try { std::ifstream file(path); const auto prior = Json::parse(file); verify(prior, summary); valid = true; }
            catch (const std::exception&) {}
            if (valid) std::filesystem::remove(pending);
            else { std::filesystem::remove(path); std::filesystem::rename(pending, path); }
        } else std::filesystem::rename(pending, path);
        remember(path,payload);
        return payload;
    }
private:
    void remember(const std::filesystem::path& path,const Json& payload)
    {
        // Keep all immutable files, but bound resident serialized profile data
        // to at most 64 * 256 KiB. Evicted entries are validated again on disk.
        if (!m_payloads.count(path) && m_payloads.size() >= 64) m_payloads.erase(m_payloads.begin());
        m_payloads[path] = payload;
    }
    void verify(const Json& payload, const Json& summary) const
    {
        validate(payload);
        const auto expected = summary.contains("settings") ? profile_summary(summary) : summary;
        if (profile_summary(payload) != expected)
            throw std::runtime_error("The downloaded HA material content does not match its requested digest");
        if (!m_keys.empty()) {
            std::set<std::string> keys;
            for (const auto& setting : payload.at("settings").items()) keys.insert(setting.key());
            if (keys != m_keys) throw std::runtime_error("The HA material profile uses a different settings schema; resynchronize it with this Quack version");
        }
    }
    std::filesystem::path entry_path(const std::string& endpoint, const Json& summary) const
    {
        return m_root / content_sha256(endpoint) / m_schema / (digest(summary) + ".json");
    }
    std::filesystem::path m_root;
    std::string m_schema;
    std::set<std::string> m_keys;
    std::mutex m_mutex;
    std::map<std::filesystem::path, Json> m_payloads;
};
inline bool restorable_contents(PersistentCache& cache,const std::string& endpoint,const Json& snapshot)
{
    try {
        for (const auto& profile : published_contents(snapshot))
            if (cache.find(endpoint,profile.summary).is_null()) return false;
        return true;
    } catch (const std::exception&) { return false; }
}
}
