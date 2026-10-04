#pragma once
#include "nlohmann/json.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>

namespace Slic3r::HaMaterialProfile {
using Json = nlohmann::json;
inline void require_text(const Json& value, size_t maximum, bool allow_empty = false)
{
    if (!value.is_string()) throw std::runtime_error("Invalid material profile text");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > maximum || text.find('\0') != std::string::npos ||
        (!allow_empty && text.find_first_not_of(" \t\r\n") == std::string::npos))
        throw std::runtime_error("Invalid material profile text");
}

inline bool is_sha256(const Json& value)
{
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

inline std::string content_sha256(const Json& content)
{
    // nlohmann's default object ordering + compact UTF-8 dump matches the HA
    // canonical JSON representation. Settings are serialized strings, not floats.
    const auto bytes = content.dump();
    unsigned char hash[SHA256_DIGEST_LENGTH];
    if (!SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), hash))
        throw std::runtime_error("Cannot fingerprint material profile");
    const char* hex = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : hash) {
        result += hex[byte >> 4]; result += hex[byte & 15];
    }
    return result;
}

inline void validate_summary(const Json& value, bool full = false)
{
    const std::set<std::string> keys {"schema_version", "name", "material_type", "dependencies", "sha256"};
    if (!value.is_object() || value.size() != keys.size() + (full ? 1 : 0))
        throw std::runtime_error("Unsupported material profile envelope");
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!keys.count(it.key()) && !(full && it.key() == "settings"))
            throw std::runtime_error("Unsupported material profile field");
    if (!value.at("schema_version").is_number_integer() || value.at("schema_version") != 1)
        throw std::runtime_error("Unsupported material profile schema");
    require_text(value.at("name"), 256);
    require_text(value.at("material_type"), 40);
    if (!is_sha256(value.at("sha256"))) throw std::runtime_error("Invalid material profile SHA-256");
    const auto& deps = value.at("dependencies");
    if (!deps.is_object() || deps.size() != 3)
        throw std::runtime_error("Invalid material profile dependency metadata");
    for (const auto* key : {"inherits", "filament_id", "vendor"}) require_text(deps.at(key), 256, true);
}

inline void validate(const Json& value)
{
    validate_summary(value, true);
    const auto& settings = value.at("settings");
    if (!settings.is_object() || settings.empty() || settings.size() > 2000)
        throw std::runtime_error("Invalid material settings");
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const auto& key = it.key();
        if (key.empty() || key.size() > 128 ||
            std::string("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ").find(key[0]) == std::string::npos ||
            key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos ||
            key == "inherits" || key == "filament_settings_id")
            throw std::runtime_error("Invalid material setting key");
        require_text(it.value(), 65536, true);
    }
    const auto& type = value.at("material_type");
    if (!settings.contains("filament_type") ||
        (settings.at("filament_type") != type && settings.at("filament_type") != type.dump()))
        throw std::runtime_error("Profile material type contradicts its settings");
    if (value.dump().size() > 262144) throw std::runtime_error("Material profile exceeds 256 KiB");
    auto content = value;
    content.erase("sha256");
    if (content_sha256(content) != value.at("sha256"))
        throw std::runtime_error("Material profile content does not match its SHA-256");
}

inline Json seal(Json value)
{
    value.erase("sha256");
    value["sha256"] = content_sha256(value);
    validate(value);
    return value;
}

inline Json profile_summary(const Json& value)
{
    if (value.is_null()) return nullptr;
    validate(value);
    auto result = value;
    result.erase("settings");
    return result;
}

inline std::string digest(const Json& value)
{
    if (value.is_null()) return {};
    value.contains("settings") ? validate(value) : validate_summary(value);
    return value.at("sha256");
}

inline std::string managed_name(const Json& value)
{
    const auto sha = digest(value);
    std::string label = value.at("name");
    // User presets recover their name from the filename at startup. '@' also
    // triggers legacy printer-name inference, so keep it out of this cache name.
    for (char& c : label)
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) == 127 ||
            std::string("<>:\"/\\|?*@").find(c) != std::string::npos) c = '_';
    if (label.size() > 100) {
        size_t end = 100;
        while (end && (static_cast<unsigned char>(label[end]) & 0xc0) == 0x80) --end;
        label.resize(end);
    }
    return "HA " + label + " [" + sha.substr(0, 16) + "]";
}
} // namespace Slic3r::HaMaterialProfile
