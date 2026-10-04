#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaCredentialPolicy.hpp"

using namespace Slic3r::HaCredentialPolicy;

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
