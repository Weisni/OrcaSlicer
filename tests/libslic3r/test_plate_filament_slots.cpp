#include <catch2/catch_test_macros.hpp>
#include <map>
#include <stdexcept>
#include <vector>
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/PlateFilamentSlots.hpp"

using namespace Slic3r;

TEST_CASE("Plate filament permutations preserve head assignments and logical material references", "[PlateFilamentSlots][Regression]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_map", new ConfigOptionInts({2, 1, 3}));
    config.set_key_value("filament_nozzle_map", new ConfigOptionInts({4, 2, 5}));
    config.set_key_value("filament_volume_map", new ConfigOptionInts({0, 1, 2}));
    config.set_key_value("imex_head_filament_map", new ConfigOptionString("0:3,2:1"));
    config.set_key_value("first_layer_print_sequence", new ConfigOptionInts({3, 1, 2}));
    config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(2));
    config.set_key_value("other_layers_print_sequence", new ConfigOptionInts({4, 12, 3, 1, 2, 13, 18, 2, 3, 1}));
    config.set_key_value("support_filament", new ConfigOptionInt(3));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(0));
    remap_plate_filament_slots(config, {{2, 3}, {3, 2}}, 4);
    CHECK(config.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{2, 1, 1, 3});
    CHECK(config.option<ConfigOptionInts>("filament_nozzle_map")->values == std::vector<int>{4, 2, 0, 5});
    CHECK(config.option<ConfigOptionInts>("filament_volume_map")->values == std::vector<int>{0, 1, 0, 2});
    CHECK(config.opt_string("imex_head_filament_map") == "0:4,2:1");
    CHECK(config.option<ConfigOptionInts>("first_layer_print_sequence")->values == std::vector<int>{4, 1, 2, 3});
    CHECK(config.option<ConfigOptionInts>("other_layers_print_sequence")->values == std::vector<int>{4, 12, 4, 1, 2, 3, 13, 18, 2, 4, 1, 3});
    CHECK(config.opt_int("other_layers_print_sequence_nums") == 2);
    CHECK(config.opt_int("support_filament") == 4);
    CHECK(config.opt_int("support_interface_filament") == 0);
}

TEST_CASE("Plate permutations retain inherited and automatic settings", "[PlateFilamentSlots][Regression]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_map", new ConfigOptionInts());
    config.set_key_value("first_layer_print_sequence", new ConfigOptionInts({0}));
    const auto original = config;
    remap_plate_filament_slots(config, {{2, 3}, {3, 2}}, 4);
    CHECK(config == original);
}

TEST_CASE("Invalid plate sequence metadata rejects a permutation atomically", "[PlateFilamentSlots][Regression]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_map", new ConfigOptionInts({2, 1, 3}));
    config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(2));
    config.set_key_value("other_layers_print_sequence", new ConfigOptionInts({1, 8, 3}));
    const auto original = config;
    CHECK_THROWS(remap_plate_filament_slots(config, {{2, 3}, {3, 2}}, 4));
    CHECK(config == original);
}
