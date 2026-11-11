#include <catch2/catch_test_macros.hpp>
#include <map>
#include "libslic3r/HaCredentialPolicy.hpp"
#include "libslic3r/CloudCredentialPolicy.hpp"

using namespace Slic3r::HaCredentialPolicy;

TEST_CASE("HA credentials survive equivalent data directory spellings", "[HaCredentialPolicy][Regression]")
{
#ifdef _WIN32
    CHECK(credential_directory_identity("C:/Users/Example/Test-Profile") == "c:/users/example/test-profile");
    CHECK(credential_directory_identity("c:\\users\\EXAMPLE\\temporary\\..\\test-profile\\") == "c:/users/example/test-profile");
    CHECK(credential_directory_identity("C:/Users/Example/./Test-Profile/") == "c:/users/example/test-profile");
    CHECK(credential_directory_identity("C:/Users/Example/Production") != "c:/users/example/test-profile");
#else
    CHECK(credential_directory_identity("/Users/Example/temporary/../Test-Profile/") == "/Users/Example/Test-Profile");
    CHECK(credential_directory_identity("/Users/Example/./Test-Profile") == "/Users/Example/Test-Profile");
    CHECK(credential_directory_identity("/Users/example/Test-Profile") != "/Users/Example/Test-Profile");
    CHECK(credential_directory_identity("/Users/Example/Test\\Profile") != "/Users/Example/Test/Profile");
#endif
}

TEST_CASE("HA credentials migrate a valid same-directory legacy slot without deleting it", "[HaCredentialPolicy][Regression]")
{
#ifdef _WIN32
    const std::string requested = "C:\\Users\\Example\\Test-Profile\\";
    const std::string legacy = "C:/Users/Example/Test-Profile";
    const std::string canonical = "c:/users/example/test-profile";
#else
    const std::string requested = "/Users/Example/./Test-Profile/";
    const std::string legacy = "/Users/Example/Test-Profile/";
    const std::string canonical = "/Users/Example/Test-Profile";
#endif
    std::map<std::string, std::string> store{{legacy, "synthetic-token"}};
    auto load = [&](const std::string &directory) { const auto found = store.find(directory); return found == store.end() ? std::string() : found->second; };
    auto save = [&](const std::string &directory, const std::string &token) { store[directory] = token; return true; };
    CHECK(load_saved_token(requested, load, save) == "synthetic-token");
    CHECK(store.at(legacy) == "synthetic-token");
    REQUIRE(store.count(canonical) == 1);
    CHECK(store.at(canonical) == "synthetic-token");
}

TEST_CASE("HA credential fallback never searches a different profile or migrates invalid data", "[HaCredentialPolicy][Regression]")
{
#ifdef _WIN32
    const std::string requested = "C:\\Users\\Example\\Test-Profile\\";
    const std::string legacy = "C:/Users/Example/Test-Profile";
    const std::string other = "C:/Users/Example/Production";
#else
    const std::string requested = "/Users/Example/./Test-Profile/";
    const std::string legacy = "/Users/Example/Test-Profile/";
    const std::string other = "/Users/Example/Production";
#endif
    std::map<std::string, std::string> store{{legacy, "invalid token"}, {other, "synthetic-production-token"}};
    const auto original = store;
    auto load = [&](const std::string &directory) { const auto found = store.find(directory); return found == store.end() ? std::string() : found->second; };
    auto save = [&](const std::string &directory, const std::string &token) { store[directory] = token; return true; };
    CHECK(load_saved_token(requested, load, save).empty());
    CHECK(store == original);
}

TEST_CASE("HA credentials remain usable when a legacy-slot migration cannot be saved", "[HaCredentialPolicy][Regression]")
{
#ifdef _WIN32
    const std::string requested = "C:\\Users\\Example\\Test-Profile\\";
    const std::string legacy = "C:/Users/Example/Test-Profile";
#else
    const std::string requested = "/Users/Example/./Test-Profile/";
    const std::string legacy = "/Users/Example/Test-Profile/";
#endif
    std::map<std::string, std::string> store{{legacy, "synthetic-token"}};
    auto load = [&](const std::string &directory) { const auto found = store.find(directory); return found == store.end() ? std::string() : found->second; };
    auto save = [](const std::string &, const std::string &) { return false; };
    CHECK(load_saved_token(requested, load, save) == "synthetic-token");
    CHECK(store.at(legacy) == "synthetic-token");
    CHECK(store.size() == 1);
}

TEST_CASE("HA credentials prefer the canonical slot and leave legacy tokens unchanged", "[HaCredentialPolicy][Regression]")
{
#ifdef _WIN32
    const std::string requested = "C:/Users/Example/Test-Profile";
    const std::string canonical = "c:/users/example/test-profile";
#else
    const std::string requested = "/Users/Example/./Test-Profile/";
    const std::string canonical = "/Users/Example/Test-Profile";
#endif
    std::map<std::string, std::string> store{{requested, "legacy-token"}, {canonical, "current-token"}};
    const auto original = store;
    auto load = [&](const std::string &directory) { const auto found = store.find(directory); return found == store.end() ? std::string() : found->second; };
    auto save = [&](const std::string &directory, const std::string &token) { store[directory] = token; return true; };
    CHECK(load_saved_token(requested, load, save) == "current-token");
    CHECK(store == original);
    CHECK(credential_directory_aliases(requested).size() <= 13);
}

TEST_CASE("HA credentials do not access storage without a data directory", "[HaCredentialPolicy][Regression]")
{
    int accesses = 0;
    auto load = [&](const std::string &) { ++accesses; return std::string("synthetic-token"); };
    auto save = [&](const std::string &, const std::string &) { ++accesses; return true; };
    CHECK(load_saved_token("", load, save).empty());
    CHECK(accesses == 0);
}

TEST_CASE("HA credentials follow a normalized server origin only", "[ha][credentials]")
{
    REQUIRE(origin("https://HA.Example:443/api/quack_material_demo/materials") == "https://ha.example");
    REQUIRE(same_origin("https://ha.example/api/quack_material_demo/materials",
                        "https://HA.example:443/api/quack_material_demo/native_snapshot?table=spools"));
    REQUIRE_FALSE(same_origin("https://ha.example/api/quack_material_demo/materials",
                              "https://ha.example.attacker.test/api/quack_material_demo/materials"));
    REQUIRE_FALSE(same_origin("https://ha.example/api/quack_material_demo/materials",
                              "https://ha.example:8443/api/quack_material_demo/materials"));
}

TEST_CASE("Invalid or non-HA endpoints cannot receive an HA token", "[ha][credentials]")
{
    for (const auto *url : {"https://user:secret@ha.example/api/quack_material_demo/materials",
                            "https://ha.example/api/other", "https://ha.example/api/quack_material_demo/materials#fragment",
                            "https://ha.example:70000/api/quack_material_demo/materials", "http://internet.example/api/quack_material_demo/materials",
                            "https://ha.example\\attacker/api/quack_material_demo/materials", "https://ha.example/%2fapi/quack_material_demo/materials"})
        REQUIRE(origin(url).empty());
    REQUIRE_FALSE(same_origin("bad", "bad"));
    REQUIRE(origin("http://homeassistant.local:8123/api/quack_material_demo/materials") == "http://homeassistant.local:8123");
    REQUIRE(origin("http://127.0.0.1:8765/api/quack_material_demo/materials") == "http://127.0.0.1:8765");
}

TEST_CASE("A legacy launcher credential cannot migrate when connection settings change", "[ha][credentials]")
{
    LegacyEnvironmentOrigin binding;
    const std::string original = "https://ha.example/api/quack_material_demo/materials";
    const std::string changed = "https://other.example/api/quack_material_demo/materials";
    binding.bind_initial(original);
    REQUIRE(binding.allows(original));
    binding.bind_initial(changed); // A disabled connection can still change its URL.
    CHECK_FALSE(binding.allows(changed));
    CHECK(binding.allows(original));
}

TEST_CASE("An initially unconfigured launcher token is not paired implicitly later", "[ha][credentials]")
{
    LegacyEnvironmentOrigin binding;
    binding.bind_initial("");
    binding.bind_initial("https://ha.example/api/quack_material_demo/materials");
    CHECK_FALSE(binding.allows("https://ha.example/api/quack_material_demo/materials"));
}

TEST_CASE("HA connection persistence checks the actual saved settings including Windows checksum files", "[ha][credentials]")
{
    const std::map<std::string, std::string> expected{
        {"ha_material_demo_endpoint", "https://ha.example/api/quack_material_demo/materials"},
        {"ha_material_provider_enabled", "1"}, {"ha_material_demo_enabled", "0"},
        {"ha_material_provider_device_id", "printer-1"}};
    const auto saved = nlohmann::json{{"app", expected}}.dump(1, '\t');
    CHECK(persisted_connection_matches(saved, expected));
    CHECK(persisted_connection_matches(saved + "\n# MD5 checksum " + std::string(32, '0') + "\n", expected));
    auto changed = expected;
    changed["ha_material_demo_endpoint"] = "https://other.example/api/quack_material_demo/materials";
    CHECK_FALSE(persisted_connection_matches(saved, changed));
    CHECK_FALSE(persisted_connection_matches(saved.substr(0, saved.size() - 1), expected));
    CHECK_FALSE(persisted_connection_matches("{}", expected));
    CHECK(persisted_connection_matches(R"({"app":{"enabled":true}})", {{"enabled", "true"}, {"missing", ""}}));
}

TEST_CASE("Quack cloud credential stores cannot read or delete Orca sessions", "[CloudCredentialPolicy][Regression]")
{
    using Slic3r::CloudCredentialPolicy::service_name;
    CHECK(std::string(service_name(false)) == "QuackSlicer/Auth");
    CHECK(std::string(service_name(true)) == "QuackSlicerBetaTest/Auth");
    CHECK(std::string(service_name(false)) != service_name(true));
    CHECK(std::string(service_name(false)) != "OrcaSlicer/Auth");
    CHECK(std::string(service_name(true, "QuackSlicerNativeTest/Auth")) != service_name(true));
    CHECK(std::string(service_name(false, "QuackSlicerNativeTest/Auth")) == service_name(false));
}
