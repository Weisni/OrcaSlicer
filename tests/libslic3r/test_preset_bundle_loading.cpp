#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

struct TempPresetDir {
    fs::path path;

    TempPresetDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("orcaslicer-preset-%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }

    ~TempPresetDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_print_preset(const DynamicPrintConfig &default_config, const fs::path &file, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>("print_settings_id", true)->value = name;
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

} // namespace

TEST_CASE("Preset identity is canonicalized from load path", "[Preset][Identity]")
{
    TempPresetDir              temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_PRINT_NAME / "User.json", "User");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_LOCAL_DIR / "bundle-1" / PRESET_PRINT_NAME / "LocalBundle.json", "LocalBundle");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1" / PRESET_PRINT_NAME / "Subscribed.json", "Subscribed");

    bundle.prints.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_LOCAL_DIR / "bundle-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *root_user = bundle.prints.find_preset("User");
    REQUIRE(root_user != nullptr);
    CHECK(root_user->name == "User");
    CHECK_FALSE(root_user->is_from_bundle());

    const Preset *local_bundle = bundle.prints.find_preset("_local/bundle-1/LocalBundle");
    REQUIRE(local_bundle != nullptr);
    CHECK(local_bundle->name == "_local/bundle-1/LocalBundle");
    CHECK(local_bundle->is_from_bundle());

    const Preset *subscribed = bundle.prints.find_preset("_subscribed/remote-1/Subscribed");
    REQUIRE(subscribed != nullptr);
    CHECK(subscribed->name == "_subscribed/remote-1/Subscribed");
    CHECK(subscribed->is_from_bundle());
}

TEST_CASE("Legacy bundle import without bundle metadata stays in the user preset directory", "[Preset][Identity]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    PresetsConfigSubstitutions substitutions;
    std::vector<std::string>   result;
    int                        overwrite = 0;
    std::string                file      = (temp_dir.path / "legacy-bundle" / "Imported.json").string();
    const fs::path             user_root = temp_dir.path / "user";

    write_print_preset(bundle.prints.default_preset().config, file, "Imported");
    fs::create_directories(user_root);
    bundle.prints.update_user_presets_directory(user_root.string(), PRESET_PRINT_NAME);

    REQUIRE(bundle.import_json_presets(
        substitutions,
        file,
        [](std::string const &) { return 1; },
        ForwardCompatibilitySubstitutionRule::Disable,
        overwrite,
        result));

    const Preset *imported = bundle.prints.find_preset("Imported");
    REQUIRE(imported != nullptr);
    CHECK(imported->name == "Imported");
    CHECK(imported->bundle_id.empty());
    CHECK_FALSE(imported->is_from_bundle());
    // Detached user presets (no inherits) are saved in the "base" subfolder of the user preset root.
    CHECK(fs::equivalent(fs::path(imported->file).parent_path().parent_path(), user_root / PRESET_PRINT_NAME));
}

TEST_CASE("Current vendor type tolerates missing printer model", "[Preset][Bundle]")
{
    PresetBundle bundle;

    VendorProfile orca_vendor("ORCA");
    VendorProfile::PrinterModel model;
    model.name = "Orca Test";
    orca_vendor.models.emplace_back(model);
    bundle.vendors.emplace("ORCA", std::move(orca_vendor));

    bundle.printers.get_edited_preset().config.erase("printer_model");

    CHECK(bundle.get_current_vendor_type() == VendorType::Unknown);
}

TEST_CASE("Printer extruder count tolerates missing nozzle diameter", "[Preset][Bundle]")
{
    PresetBundle bundle;
    DynamicPrintConfig& config = bundle.printers.get_edited_preset().config;

    config.erase("nozzle_diameter");
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats());
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
    CHECK(bundle.get_printer_extruder_count() == 2);
}

TEST_CASE("Project printers are activated only when configured locally", "[Preset][ProjectPrinter]")
{
    PresetBundle bundle;

    DynamicPrintConfig configured_config(bundle.printers.default_preset().config);
    configured_config.set_key_value("printer_model", new ConfigOptionString("Configured Model"));
    configured_config.set_key_value("printer_variant", new ConfigOptionString("0.4"));
    Preset &configured = bundle.printers.load_preset({}, "Configured Printer", configured_config, false);
    configured.is_visible = true;

    DynamicPrintConfig project_config(configured_config);
    project_config.set_key_value("printer_settings_id", new ConfigOptionString("Project Printer"));
    project_config.set_key_value("filament_colour", new ConfigOptionStrings({"#FFFFFF"}));

    CHECK(bundle.has_configured_printer_for_project(project_config));

    configured.is_visible = false;
    CHECK_FALSE(bundle.has_configured_printer_for_project(project_config));

    configured.is_visible = true;
    configured.is_external = true;
    CHECK_FALSE(bundle.has_configured_printer_for_project(project_config));

    configured.is_external = false;
    project_config.set_key_value("printer_variant", new ConfigOptionString("0.6"));
    CHECK_FALSE(bundle.has_configured_printer_for_project(project_config));

    project_config.set_key_value("printer_settings_id", new ConfigOptionString("Configured Printer"));
    CHECK(bundle.has_configured_printer_for_project(project_config));
}

TEST_CASE("Switching printers preserves project filament colors", "[Preset][ProjectColors]")
{
    PresetBundle bundle;
    AppConfig    app_config;

    const std::string printer_name = bundle.printers.get_selected_preset_name();
    REQUIRE_FALSE(printer_name.empty());

    app_config.set_printer_setting(printer_name, PRESET_PRINT_NAME, bundle.prints.get_selected_preset_name());
    app_config.set_printer_setting(printer_name, PRESET_FILAMENT_NAME, bundle.filaments.get_selected_preset_name());
    app_config.set_printer_setting(printer_name, "filament_colors", "#AAAAAA");
    app_config.set_printer_setting(printer_name, "filament_multi_colors", "#BBBBBB");
    app_config.set_printer_setting(printer_name, "filament_color_types", "9");

    const std::vector<std::string> colors       = {"#112233", "#445566", "#778899"};
    const std::vector<std::string> multi_colors = {"#111111", "#444444", "#777777"};
    const std::vector<std::string> color_types  = {"1", "2", "3"};
    const std::vector<int>         maps         = {3, 2, 1};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values       = colors;
    bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = multi_colors;
    bundle.project_config.option<ConfigOptionStrings>("filament_colour_type")->values  = color_types;
    bundle.project_config.option<ConfigOptionInts>("filament_map")->values              = maps;

    bundle.update_selections(app_config, true);

    CHECK(bundle.filament_presets.size() >= colors.size());
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values == colors);
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values == multi_colors);
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_colour_type")->values == color_types);
    CHECK(bundle.project_config.option<ConfigOptionInts>("filament_map")->values == maps);
}

