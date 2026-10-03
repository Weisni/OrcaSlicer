#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaMaterialProfilePayload.hpp"

using namespace Slic3r::HaMaterialProfile;

static Json profile_content()
{
    return {{"schema_version", 1}, {"name", u8"Exact Weiß PLA"}, {"material_type", "PLA"},
        {"settings", {{"filament_type", "PLA"}, {"filament_flow_ratio", "0.98"},
            {"filament_start_gcode", "; keep material code\nM900 K0.02"},
            {"compatible_printers", "Bambu Lab P2S 0.4 nozzle"}}},
        {"dependencies", {{"inherits", "Generic PLA"}, {"filament_id", "GFA00"}, {"vendor", "BBL"}}}};
}

TEST_CASE("Material payload fingerprints change for same-name setting changes", "[HaMaterialProfilePayload]")
{
    const auto first = seal(profile_content());
    auto changed = profile_content();
    changed["settings"]["filament_flow_ratio"] = "1.03";
    const auto second = seal(changed);
    CHECK(first.at("name") == second.at("name"));
    CHECK(digest(first) != digest(second));
    CHECK(managed_name(first) != managed_name(second));
}

TEST_CASE("Material payloads preserve Unicode G-code and dependency metadata", "[HaMaterialProfilePayload]")
{
    const auto value = seal(profile_content());
    // Independently calculated by Python hashlib over the HA canonical payload.
    CHECK(digest(value) == "f5f3d0d36e2b60ea00029476f376a806e3be303779284d77ccb1a58cd1432a56");
    CHECK_NOTHROW(validate(value));
    auto summary = profile_summary(value);
    CHECK_FALSE(summary.contains("settings"));
    CHECK(summary.at("dependencies") == value.at("dependencies"));
    CHECK(digest(summary) == digest(value));
    CHECK(value.at("settings").at("filament_start_gcode") == "; keep material code\nM900 K0.02");
}

TEST_CASE("Tampered material settings cannot keep a previous digest", "[HaMaterialProfilePayload]")
{
    auto value = seal(profile_content());
    value["settings"]["filament_flow_ratio"] = "1.03";
    CHECK_THROWS(validate(value));
}

TEST_CASE("Material payloads reject unrelated malformed and oversized data", "[HaMaterialProfilePayload]")
{
    auto value = profile_content();
    SECTION("Unknown envelope field") { value["unexpected"] = true; }
    SECTION("Nonstring setting") { value["settings"]["filament_flow_ratio"] = 1.1; }
    SECTION("Oversized material code") { value["settings"]["filament_start_gcode"] = std::string(65537, 'x'); }
    SECTION("Contradictory material") { value["material_type"] = "PETG"; }
    SECTION("Missing inheritance metadata") { value["dependencies"].erase("inherits"); }
    SECTION("Inheritance hidden among settings") { value["settings"]["inherits"] = "Other"; }
    CHECK_THROWS(seal(value));
}

TEST_CASE("Managed material names survive filename-based user preset loading", "[HaMaterialProfilePayload]")
{
    auto value = profile_content();
    value["name"] = u8"Maker: weiß / PLA <soft> | test?*\\profile\n@P2S" + std::string(160, 'x');
    const auto profile = seal(value);
    const auto name = managed_name(profile);
    CHECK(name.find_first_of("<>:\"/\\|?*@\n\r\t") == std::string::npos);
    CHECK(name.size() <= 122);
    CHECK_NOTHROW(Json(name).dump());
    CHECK(name.substr(name.size() - 18) == "[" + digest(profile).substr(0, 16) + "]");
    CHECK(profile.at("name") == value.at("name"));
}
