#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaMaterialLocalOverride.hpp"

TEST_CASE("Temporary edits follow the changed material after its editor is closed", "[HaMaterialLocalOverride]")
{
    const auto index = Slic3r::HaMaterialLocalOverride::replacement_index(
        {"HA PLA", "HA PETG"}, {"HA PLA", "HA PETG new", "HA ASA"}, "HA PETG");
    REQUIRE(index.has_value());
    CHECK(*index == 1);
    CHECK_FALSE(Slic3r::HaMaterialLocalOverride::replacement_index(
        {"HA PLA", "HA PETG"}, {"HA PLA new", "HA PETG"}, "HA PETG").has_value());
}

TEST_CASE("Shared temporary material settings require a consistent new HA base", "[HaMaterialLocalOverride]")
{
    using Slic3r::HaMaterialLocalOverride::replacement_index;
    CHECK_THROWS(replacement_index({"HA PLA", "HA PLA"}, {"HA PLA new", "HA PLA other"}, "HA PLA"));
    CHECK(replacement_index({"HA PLA", "HA PLA"}, {"HA PLA new", "HA PLA new"}, "HA PLA").has_value());
    CHECK_FALSE(replacement_index({"HA PLA"}, {"HA PLA new"}, "Unassigned profile").has_value());
    CHECK_THROWS(replacement_index({"HA PLA", "HA PETG"}, {"HA PETG", "HA PETG"}, "HA PLA"));
    CHECK_THROWS(replacement_index({"HA PLA"}, {"HA PLA new", "HA PLA new"}, "HA PLA"));
}

TEST_CASE("Loading one shared material leaves temporary settings on the remaining original profile", "[HaMaterialLocalOverride]")
{
    CHECK_FALSE(Slic3r::HaMaterialLocalOverride::replacement_index(
        {"Local PLA", "Local PLA", "Local PLA"}, {"HA PLA", "Local PLA", "Local PLA"}, "Local PLA").has_value());
    CHECK_FALSE(Slic3r::HaMaterialLocalOverride::replacement_index(
        {"Local PLA", "Local PLA", "Local PLA"}, {"HA PLA", "HA PETG", "Local PLA"}, "Local PLA").has_value());
    CHECK_FALSE(Slic3r::HaMaterialLocalOverride::replacement_index(
        {"Local PLA"}, {"HA PLA", "Local PLA"}, "Local PLA").has_value());
}

using Slic3r::HaMaterialLocalOverride::merge_settings;

TEST_CASE("One HA project instance retains edits when another slot already uses the new committed base", "[HaMaterialLocalOverride][Regression]")
{
    const std::string edited = "HA material [" + std::string(64, 'a') + "] [project filament 1]";
    const std::string updated = "HA material [" + std::string(64, 'b') + "]";
    std::optional<size_t> index;
    REQUIRE_NOTHROW(index = Slic3r::HaMaterialLocalOverride::replacement_index(
        {edited, updated}, {updated, updated}, edited));
    REQUIRE(index.has_value());
    CHECK(*index == 0);
}
using Json = nlohmann::json;

TEST_CASE("Temporary material edits survive an updated HA base", "[HaMaterialLocalOverride]")
{
    const Json base = {{"filament_flow_ratio", "0.98"}, {"temperature", "210"}, {"filament_start_gcode", "; original"}};
    auto edited = base;
    edited["filament_flow_ratio"] = "1.04";
    const Json updated = {{"filament_flow_ratio", "0.97"}, {"temperature", "215"}, {"filament_start_gcode", "; updated"}};
    const auto result = merge_settings(base, edited, updated);
    CHECK(result.at("filament_flow_ratio") == "1.04");
    CHECK(result.at("temperature") == "215");
    CHECK(result.at("filament_start_gcode") == "; updated");
    CHECK(base.at("filament_flow_ratio") == "0.98");
}

TEST_CASE("Unedited material settings follow the updated HA profile", "[HaMaterialLocalOverride]")
{
    const Json base = {{"temperature", "210"}};
    const Json updated = {{"temperature", "215"}};
    CHECK(merge_settings(base, base, updated) == updated);
}

TEST_CASE("Temporary edits do not reintroduce removed material settings", "[HaMaterialLocalOverride]")
{
    const Json base = {{"old_option", "1"}, {"temperature", "210"}};
    const Json edited = {{"old_option", "2"}, {"temperature", "220"}};
    const Json updated = {{"temperature", "215"}};
    CHECK(merge_settings(base, edited, updated) == Json{{"temperature", "220"}});
}
