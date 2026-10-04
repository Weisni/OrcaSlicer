#pragma once
#include <string>
#include <algorithm>
#include <cctype>
#include <map>
#include "nlohmann/json.hpp"

namespace Slic3r::HaCredentialPolicy {
// Credentials belong to one server, and only to this integration's endpoints.
inline std::string origin(const std::string &url)
{
    if (url.size() > 2048 || url.find_first_of("\\#@") != std::string::npos) return {};
    for (unsigned char c : url) if (c <= 32 || c >= 127) return {};
    const auto separator = url.find("://");
    if (separator == std::string::npos) return {};
    auto scheme = url.substr(0, separator);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (scheme != "https" && scheme != "http") return {};
    const auto slash = url.find('/', separator + 3);
    if (slash == std::string::npos) return {};
    auto authority = url.substr(separator + 3, slash - separator - 3);
    std::transform(authority.begin(), authority.end(), authority.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const auto query = url.find('?', slash);
    const auto path = url.substr(slash, query == std::string::npos ? query : query - slash);
    const std::string prefix = "/api/quack_material_demo/";
    if (path.compare(0, prefix.size(), prefix) != 0) return {};
    const auto operation = path.substr(prefix.size());
    const bool read = operation == "materials" || operation == "native_snapshot" || operation == "profile" ||
                      operation == "inventory" || operation == "recovery";
    const bool action = operation.compare(0, 7, "action/") == 0 && operation.size() > 7 &&
                        operation.find_first_not_of("abcdefghijklmnopqrstuvwxyz_", 7) == std::string::npos;
    if (!read && !action) return {};
    std::string host, port_text;
    if (!authority.empty() && authority[0] == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos) return {};
        host = authority.substr(0, end + 1);
        if (host != "[::1]") return {}; // Other hosts use their DNS name.
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') return {};
            port_text = authority.substr(end + 2);
            if (port_text.empty()) return {};
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) {
            port_text = authority.substr(colon + 1);
            if (port_text.empty()) return {};
        }
        if (host.empty() || host.front() == '.' || host.back() == '.' ||
            host.find("..") != std::string::npos || host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos) return {};
    }
    unsigned port = scheme == "https" ? 443 : 80;
    if (!port_text.empty()) {
        if (port_text.size() > 5 || port_text.find_first_not_of("0123456789") != std::string::npos) return {};
        port = 0;
        for (char c : port_text) port = port * 10 + unsigned(c - '0');
        if (port == 0 || port > 65535) return {};
    }
    const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    if (scheme == "http" && !loopback && !(host == "homeassistant.local" && port == 8123)) return {};
    const bool default_port = (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    return scheme + "://" + host + (default_port ? "" : ":" + std::to_string(port));
}
inline bool same_origin(const std::string &left, const std::string &right)
{
    const auto value = origin(left);
    return !value.empty() && value == origin(right);
}

class LegacyEnvironmentOrigin {
public:
    void bind_initial(const std::string &endpoint) {
        if (m_bound) return;
        m_origin = origin(endpoint);
        m_bound = true;
    }
    bool allows(const std::string &endpoint) const { return !m_origin.empty() && m_origin == origin(endpoint); }
private:
    std::string m_origin;
    bool m_bound = false;
};

inline bool persisted_connection_matches(const std::string &serialized,
                                        const std::map<std::string, std::string> &expected)
{
    try {
        auto content = serialized;
        const std::string marker = "\n# MD5 checksum ";
        const auto footer = content.rfind(marker);
        if (footer != std::string::npos) {
            const auto checksum = content.substr(footer + marker.size());
            if (checksum.size() < 32 || checksum.substr(0, 32).find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                return false;
            const auto ending = checksum.substr(32);
            if (!ending.empty() && ending != "\n" && ending != "\r\n") return false;
            content.resize(footer);
        }
        const auto saved = nlohmann::json::parse(content);
        const auto &app = saved.at("app");
        if (!app.is_object()) return false;
        for (const auto &item : expected) {
            if (!app.contains(item.first)) { if (!item.second.empty()) return false; continue; }
            const auto &value = app.at(item.first);
            if (value.is_boolean()) {
                if (item.second != (value.get<bool>() ? "true" : "false")) return false;
            } else if (value != item.second) return false;
        }
        return true;
    } catch (...) { return false; }
}
}
