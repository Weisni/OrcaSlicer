#pragma once
#include <map>
#include <mutex>
#include <iomanip>
#include <sstream>
#include <wx/secretstore.h>
#include <wx/utils.h>
#include <boost/uuid/detail/sha1.hpp>
#include <boost/nowide/fstream.hpp>
#include "nlohmann/json.hpp"
#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/HaCredentialPolicy.hpp"

namespace Slic3r::GUI {
namespace HaInventoryCredentials {
inline std::mutex mutex;
inline std::map<std::string, std::string> session;
inline HaCredentialPolicy::LegacyEnvironmentOrigin environment_origin;

inline void freeze_environment_origin(const std::string &initial_endpoint)
{
    std::lock_guard<std::mutex> lock(mutex);
    environment_origin.bind_initial(initial_endpoint);
}

inline bool can_remember()
{
#if wxUSE_SECRETSTORE
    return wxSecretStore::GetDefault().IsOk();
#else
    return false;
#endif
}

// AppConfig::save() reports filesystem failures through dirty state or its log.
// Verify this connection's actual persisted fields before accepting the change.
inline bool connection_persisted(AppConfig &config)
{
    if (config.dirty()) return false;
    try {
        boost::nowide::ifstream input(config.config_path());
        std::ostringstream serialized;
        serialized << input.rdbuf();
        std::map<std::string, std::string> expected;
        for (const auto *key : {"ha_material_demo_endpoint", "ha_material_demo_enabled",
                               "ha_material_provider_enabled", "ha_material_provider_device_id"})
            expected[key] = config.get(key);
        return HaCredentialPolicy::persisted_connection_matches(serialized.str(), expected);
    } catch (...) { return false; }
}

inline bool valid_token(const std::string &token)
{
    if (token.empty() || token.size() > 8192) return false;
    for (unsigned char c : token) if (c <= 32 || c >= 127) return false;
    return true;
}

inline wxString service(const std::string &origin)
{
    // The hash names a credential slot; it does not encrypt the token.
    const auto identity = data_dir() + "\n" + origin;
    boost::uuids::detail::sha1 hash;
    hash.process_bytes(identity.data(), identity.size());
    unsigned int digest[5]; hash.get_digest(digest);
    std::ostringstream name; name << "QuackSlicer.HomeAssistant." << std::hex << std::setfill('0');
    for (auto part : digest) name << std::setw(8) << part;
    return wxString::FromUTF8(name.str().c_str());
}

inline void stage(const std::string &endpoint, const std::string &token)
{
    const auto origin = HaCredentialPolicy::origin(endpoint);
    if (origin.empty() || (!token.empty() && !valid_token(token))) throw std::runtime_error("Invalid HA address or token");
    std::lock_guard<std::mutex> lock(mutex);
    session[origin] = token;
}

inline void remember(const std::string &endpoint, const std::string &token)
{
    const auto origin = HaCredentialPolicy::origin(endpoint);
    if (origin.empty() || !valid_token(token)) throw std::runtime_error("Enter a valid HA access token first");
#if wxUSE_SECRETSTORE
    auto store = wxSecretStore::GetDefault();
    if (!store.IsOk() || !store.Save(service(origin), "home-assistant", wxSecretValue(wxString::FromUTF8(token.c_str()))))
        throw std::runtime_error("The operating system could not save the token. Use this session only, or unlock the system credential store and reconnect.");
#else
    throw std::runtime_error("Secure credential storage is unavailable in this build. Use this session only.");
#endif
}
}

inline std::string ha_inventory_token(const std::string &endpoint)
{
    const auto origin = HaCredentialPolicy::origin(endpoint);
    if (origin.empty()) return {};
    std::lock_guard<std::mutex> lock(HaInventoryCredentials::mutex);
    HaInventoryCredentials::environment_origin.bind_initial(wxGetApp().app_config->get("ha_material_demo_endpoint"));
    const auto found = HaInventoryCredentials::session.find(origin);
    if (found != HaInventoryCredentials::session.end()) return found->second;
    std::string token;
#if wxUSE_SECRETSTORE
    auto store = wxSecretStore::GetDefault();
    wxString username;
    wxSecretValue secret;
    if (store.IsOk() && store.Load(HaInventoryCredentials::service(origin), username, secret) && secret.IsOk())
        token.assign(static_cast<const char *>(secret.GetData()), secret.GetSize());
#endif
    if (!HaInventoryCredentials::valid_token(token)) {
        token.clear();
        // Compatibility with existing private launchers, restricted to their configured server.
        wxString environment;
        if (HaInventoryCredentials::environment_origin.allows(endpoint) && wxGetEnv("QUACK_HA_DEMO_TOKEN", &environment))
            token = std::string(environment.ToUTF8().data());
    }
    if (!HaInventoryCredentials::valid_token(token)) token.clear();
    HaInventoryCredentials::session[origin] = token;
    return token;
}
}
