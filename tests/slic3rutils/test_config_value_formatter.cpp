#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ConfigValueFormatter.hpp"
#include "slic3r/GUI/OptionsGroup.hpp"
#include <boost/any.hpp>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("An enum vector index past the end of the vector reads as undefined", "[ConfigValueFormatter][Regression]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, ConfigOptionEnumsGenericNullable::nil_value()));

    CHECK(get_string_value(key + "#1", config) == "Undefined");
}

TEST_CASE("An enum vector index inside the vector reads as its label", "[ConfigValueFormatter]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, def->enum_keys_map->at("Top Only")));

    CHECK(get_string_value(key + "#0", config).utf8_string() == "Top Only");
}

TEST_CASE("An enum value outside the label list reads as undefined", "[ConfigValueFormatter]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, int(def->enum_labels.size())));

    CHECK(get_string_value(key + "#0", config) == "Undefined");
}

TEST_CASE("Nullable nozzle fields broadcast their raw value without changing the material", "[ConfigValueFormatter][Regression]")
{
    const unsigned char value = GENERATE(static_cast<unsigned char>(0), static_cast<unsigned char>(1),
        ConfigOptionBoolsNullable::nil_value());
    DynamicPrintConfig config;
    auto *option = new ConfigOptionBoolsNullable();
    option->values = {value};
    config.set_key_value("filament_retract_when_changing_layer", option);
    const auto before = config.opt_serialize("filament_retract_when_changing_layer");
    ConfigOptionsGroup group(nullptr);

    CHECK(boost::any_cast<unsigned char>(group.get_config_value(config, "filament_retract_when_changing_layer", 1)) == value);
    CHECK(boost::any_cast<unsigned char>(group.get_config_value2(config, "filament_retract_when_changing_layer", 1)) == value);
    CHECK(config.opt_serialize("filament_retract_when_changing_layer") == before);
}
