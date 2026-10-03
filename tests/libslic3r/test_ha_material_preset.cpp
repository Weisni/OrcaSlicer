#include <catch2/catch_test_macros.hpp>
#include "libslic3r/PresetBundle.hpp"
using namespace Slic3r;

static void add_tray(PresetBundle &bundle, const std::string &preset_name, const std::string &material_type = "PLA")
{
    DynamicPrintConfig tray;
    tray.set_key_value("filament_id", new ConfigOptionStrings{"HA-roll-uuid"});
    tray.set_key_value("ha_material_preset", new ConfigOptionStrings{preset_name});
    tray.set_key_value("ha_spool_uuid", new ConfigOptionStrings{"roll-uuid"});
    tray.set_key_value("filament_colour", new ConfigOptionStrings{"#367AF5"});
    tray.set_key_value("filament_colour_type", new ConfigOptionStrings{"1"});
    tray.set_key_value("filament_multi_colour", new ConfigOptionStrings{"#367AF5"});
    tray.set_key_value("filament_type", new ConfigOptionStrings{material_type});
    tray.set_key_value("ams_id", new ConfigOptionStrings{"demo"});
    tray.set_key_value("slot_id", new ConfigOptionStrings{"A1"});
    bundle.filament_ams_list.emplace(0, std::move(tray));
}

TEST_CASE("HA rolls use a compatible same-type standard when their special profile is absent", "[HaMaterialPreset]")
{
    for (const auto &requested : {std::string(), std::string("Missing vendor PETG")}) {
        PresetBundle bundle;
        DynamicPrintConfig config(bundle.filaments.default_preset().config);
        config.set_key_value("filament_type", new ConfigOptionStrings{"PETG"});
        auto &generic = bundle.filaments.load_preset(std::string(), "Generic PETG @P2S HF", config, false);
        generic.is_system = true;
        generic.is_compatible = true;
        add_tray(bundle, requested, "PETG");
        std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
        std::map<int, AMSMapInfo> maps;
        MergeFilamentInfo merge;
        REQUIRE(bundle.sync_ams_list(unknowns, false, maps, false, merge, false) == 1);
        CHECK(bundle.filament_presets[0] == "Generic PETG @P2S HF");
        CHECK(unknowns.empty());
        CHECK(bundle.filament_ams_list.at(0).opt_string("ha_spool_uuid", 0u) == "roll-uuid");
        CHECK(bundle.project_config.opt_string("filament_colour", 0u) == "#367AF5");
    }
}

TEST_CASE("HA standards never substitute a different material type", "[HaMaterialPreset]")
{
    PresetBundle bundle;
    DynamicPrintConfig config(bundle.filaments.default_preset().config);
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    auto &generic = bundle.filaments.load_preset(std::string(), "Generic PLA @P2S", config, false);
    generic.is_system = true;
    generic.is_compatible = true;
    const auto previous = bundle.filament_presets;
    add_tray(bundle, "", "PETG");
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo> maps;
    MergeFilamentInfo merge;
    CHECK(bundle.sync_ams_list(unknowns, false, maps, false, merge, false) == 0);
    CHECK(bundle.filament_presets == previous);
    CHECK_FALSE(unknowns.empty());
}

TEST_CASE("HA synchronization selects the exact named material preset", "[HaMaterialPreset]")
{
    PresetBundle bundle;
    DynamicPrintConfig config(bundle.filaments.default_preset().config);
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.91});
    auto &generic = bundle.filaments.load_preset(std::string(), "Generic PLA @P2S", config, false);
    generic.is_system = true;
    generic.is_compatible = true;
    auto &preset = bundle.filaments.load_preset(std::string(), "Actual PLA @P2S", config, false);
    preset.is_compatible = true;
    add_tray(bundle, preset.name);
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo> maps;
    MergeFilamentInfo merge;
    REQUIRE(bundle.sync_ams_list(unknowns, false, maps, false, merge, false) == 1);
    CHECK(bundle.filament_presets[0] == "Actual PLA @P2S");
    CHECK(unknowns.empty());
    CHECK(bundle.project_config.opt_string("filament_colour", 0u) == "#367AF5");
}

TEST_CASE("Missing HA material presets preserve project selections when no matching standard exists", "[HaMaterialPreset]")
{
    PresetBundle bundle;
    const auto previous = bundle.filament_presets;
    add_tray(bundle, "Not installed PLA");
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo> maps;
    MergeFilamentInfo merge;
    CHECK(bundle.sync_ams_list(unknowns, false, maps, false, merge, false) == 0);
    CHECK(bundle.filament_presets == previous);
    CHECK_FALSE(unknowns.empty());
}

TEST_CASE("An incompatible special preset cannot be hidden by standard selection", "[HaMaterialPreset]")
{
    PresetBundle bundle;
    DynamicPrintConfig config(bundle.filaments.default_preset().config);
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    auto &generic = bundle.filaments.load_preset(std::string(), "Generic PLA @P2S", config, false);
    generic.is_system = true;
    generic.is_compatible = true;
    auto &special = bundle.filaments.load_preset(std::string(), "Special PLA for another printer", config, false);
    special.is_compatible = false;
    add_tray(bundle, special.name);
    const auto previous = bundle.filament_presets;
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo> maps;
    MergeFilamentInfo merge;
    CHECK(bundle.sync_ams_list(unknowns, false, maps, false, merge, false) == 0);
    CHECK(bundle.filament_presets == previous);
    CHECK_FALSE(unknowns.empty());
}
