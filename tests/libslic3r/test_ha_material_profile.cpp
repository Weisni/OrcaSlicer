#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaMaterialProfile.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/HaProjectMaterialDifferences.hpp"
#include "libslic3r/HaMaterialLocalOverride.hpp"
#include "libslic3r/HaMaterialEditSlot.hpp"
#include <boost/filesystem.hpp>

using namespace Slic3r;

static Preset exact_material()
{
    PresetBundle bundle;
    Preset preset = bundle.filaments.default_preset();
    preset.name = "Exact material @P2S";
    preset.config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    preset.config.set_key_value("inherits", new ConfigOptionString("Generic PLA parent"));
    preset.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Bambu Lab P2S 0.4 nozzle"});
    preset.config.set_key_value("filament_start_gcode", new ConfigOptionStrings{"; exact material\nM900 K0.02"});
    preset.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.98});
    preset.filament_id = "GFA00";
    return preset;
}

TEST_CASE("A full HA material retains settings without requiring its parent preset", "[HaMaterialProfile]")
{
    const auto preset = exact_material();
    const auto payload = HaMaterialProfile::capture(preset);
    REQUIRE(payload.contains("settings"));
    const auto restored = HaMaterialProfile::decode_config(payload);
    CHECK(payload.at("dependencies").at("inherits") == "Generic PLA parent");
    CHECK(restored.opt_string("inherits").empty());
    for (auto it = payload.at("settings").begin(); it != payload.at("settings").end(); ++it)
        CHECK(restored.opt_serialize(it.key()) == preset.config.opt_serialize(it.key()));
}

TEST_CASE("An unchanged HA-managed preset does not create a false upload difference", "[HaMaterialProfile]")
{
    const auto original = HaMaterialProfile::capture(exact_material());
    REQUIRE(original.contains("settings"));
    Preset downloaded(Preset::TYPE_FILAMENT, HaMaterialProfile::managed_name(original));
    downloaded.config = HaMaterialProfile::decode_config(original);
    const auto summary = HaMaterialProfile::profile_summary(original);
    CHECK(HaMaterialProfile::capture(downloaded, summary) == original);
    downloaded.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
    const auto changed = HaMaterialProfile::capture(downloaded, summary);
    CHECK(changed.at("name") == original.at("name"));
    CHECK(changed.at("sha256") != original.at("sha256"));
}

TEST_CASE("One HA profile replacement preserves effective edits on unchanged shared materials", "[HaMaterialProfile][Regression]")
{
    PresetBundle bundle;
    const auto local = exact_material();
    bundle.filaments.load_preset({}, local.name, local.config, false);
    auto remote = local;
    remote.name = "HA committed PLA";
    remote.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.97});
    remote.config.set_key_value("nozzle_temperature", new ConfigOptionInts{215});
    bundle.filaments.load_preset({}, remote.name, remote.config, false);
    bundle.filaments.select_preset_by_name(local.name, true);
    bundle.filaments.get_edited_preset().config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
    bundle.filaments.get_edited_preset().config.set_key_value("nozzle_temperature", new ConfigOptionInts{230});
    bundle.set_num_filaments(3, std::vector<std::string>{"#112233", "#445566", "#778899"});
    bundle.filament_presets = {local.name, local.name, local.name};
    const auto original = bundle.filament_presets;
    const auto settings = bundle.filaments.get_edited_preset().config;
    bundle.set_filament_preset(0, remote.name);
    const auto replacement = HaMaterialLocalOverride::replacement_index(original, bundle.filament_presets, local.name);
    REQUIRE_FALSE(replacement.has_value());
    CHECK(bundle.filaments.get_edited_preset().config == settings);
    const auto full = bundle.full_config();
    CHECK(full.opt_serialize("filament_flow_ratio") == "0.97,1.03,1.03");
    CHECK(full.opt_serialize("nozzle_temperature") == "215,230,230");
    REQUIRE(HaMaterialProfile::stored_preset(bundle.filaments, local.name) != nullptr);
    CHECK(HaMaterialProfile::stored_preset(bundle.filaments, local.name)->config.opt_serialize("filament_flow_ratio") == "0.98");
    CHECK(HaMaterialProfile::stored_preset(bundle.filaments, remote.name)->config.opt_serialize("filament_flow_ratio") == "0.97");
}

TEST_CASE("One managed profile recaptures distinct HA publication labels without preset duplication", "[HaMaterialProfile]")
{
    const auto first = HaMaterialProfile::capture(exact_material());
    auto second = first;
    second["name"] = "Another roll label";
    second["dependencies"]["inherits"] = "Another equivalent parent";
    second = HaMaterialProfile::seal(second);
    REQUIRE(HaMaterialProfile::managed_name(first) == HaMaterialProfile::managed_name(second));
    Preset installed(Preset::TYPE_FILAMENT,HaMaterialProfile::managed_name(first));
    installed.config = HaMaterialProfile::decode_config(first);
    installed.filament_id = first.at("dependencies").at("filament_id");
    CHECK(HaMaterialProfile::capture(installed,HaMaterialProfile::profile_summary(first)) == first);
    CHECK(HaMaterialProfile::capture(installed,HaMaterialProfile::profile_summary(second)) == second);
}

TEST_CASE("HA profile decoding rejects process settings and incomplete material data", "[HaMaterialProfile]")
{
    auto payload = HaMaterialProfile::capture(exact_material());
    REQUIRE(payload.contains("settings"));
    SECTION("A process setting is not a material override") { payload["settings"]["layer_height"] = "0.28"; }
    SECTION("Missing settings do not silently pick local defaults") { payload["settings"].erase("filament_flow_ratio"); }
    payload = HaMaterialProfile::seal(payload);
    CHECK_THROWS(HaMaterialProfile::decode_config(payload));
}

TEST_CASE("Stored HA profile validation and bootstrap do not consume temporary edits", "[HaMaterialProfile]")
{
    const auto original = HaMaterialProfile::capture(exact_material());
    const auto name = HaMaterialProfile::managed_name(original);
    PresetBundle bundle;
    auto &installed = bundle.filaments.load_preset({}, name, HaMaterialProfile::decode_config(original), false);
    installed.filament_id = original.at("dependencies").at("filament_id");
    bundle.filaments.select_preset_by_name(name,true);
    bundle.filaments.get_edited_preset().config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
    const auto* stored = HaMaterialProfile::stored_preset(bundle.filaments,name);
    REQUIRE(stored != nullptr);
    CHECK(HaMaterialProfile::capture(*stored,original) == original);
    CHECK(HaMaterialProfile::capture(*bundle.filaments.find_preset(name),original) != original);
    CHECK(bundle.filaments.get_edited_preset().config.opt_serialize("filament_flow_ratio") == "1.03");
    CHECK(HaMaterialProfile::stored_preset(bundle.filaments,"Missing profile") == nullptr);
    bundle.filaments.find_preset(name,false,true)->config.set_key_value("filament_flow_ratio",new ConfigOptionFloats{1.07});
    CHECK(HaMaterialProfile::content_identity(HaMaterialProfile::capture(
        *HaMaterialProfile::stored_preset(bundle.filaments,name),original)) != HaMaterialProfile::content_identity(original));
}

TEST_CASE("Uploading a managed project's edits does not rename its profile on the next comparison", "[HaMaterialProfile]")
{
    const auto original = HaMaterialProfile::capture(exact_material());
    Preset downloaded(Preset::TYPE_FILAMENT, HaMaterialProfile::managed_name(original));
    downloaded.config = HaMaterialProfile::decode_config(original);
    downloaded.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
    const auto uploaded = HaMaterialProfile::capture(downloaded, HaMaterialProfile::profile_summary(original));
    CHECK(HaMaterialProfile::capture(downloaded, HaMaterialProfile::profile_summary(uploaded)) == uploaded);
}

TEST_CASE("A downloaded material retains its identity and settings after user preset reload", "[HaMaterialProfile]")
{
    namespace fs = boost::filesystem;
    // Use an isolated directory because the real loader derives names from
    // filenames and scans the directory, unlike a single-config JSON decoder.
    struct Files {
        fs::path root = fs::temp_directory_path() / fs::unique_path("quack-ha-profile-%%%%-%%%%");
        fs::path directory = root / PRESET_FILAMENT_NAME;
        fs::path file;
        ~Files() {
            boost::system::error_code ignored;
            if (!file.empty()) { fs::remove(file, ignored); auto info = file; info.replace_extension(".info"); fs::remove(info, ignored); }
            fs::remove(directory, ignored); fs::remove(root, ignored);
        }
    } files;
    auto original = exact_material();
    original.config.set_key_value("compatible_printers", new ConfigOptionStrings{});
    original.config.set_key_value("compatible_printers_condition", new ConfigOptionString("printer_model==\"Bambu Lab P2S\""));
    const auto payload = HaMaterialProfile::capture(original);
    const auto name = HaMaterialProfile::managed_name(payload);
    auto config = HaMaterialProfile::decode_config(payload);
    config.set_key_value(BBL_JSON_KEY_FILAMENT_ID, new ConfigOptionString(original.filament_id));
    fs::create_directories(files.directory);
    files.file = files.directory / (name + ".json");
    config.save_to_json(files.file.string(), name, "User", "2.5.19");

    PresetBundle reloaded;
    PresetsConfigSubstitutions substitutions;
    reloaded.filaments.load_presets(files.root.string(), PRESET_FILAMENT_NAME, substitutions,
        ForwardCompatibilitySubstitutionRule::Disable);
    const auto *preset = reloaded.filaments.find_preset(name);
    REQUIRE(preset != nullptr);
    CHECK(preset->name == name);
    CHECK(preset->filament_id == original.filament_id);
    CHECK(preset->config.opt<ConfigOptionStrings>("compatible_printers")->values.empty());
    CHECK(HaMaterialProfile::capture(*preset, HaMaterialProfile::profile_summary(payload)) == payload);
}

static const std::string profile_roll = "641c70ee-6b92-44dc-b1c1-ddc1c86ce001";
static const std::string profile_source = "http://ha/api/materials";

TEST_CASE("Editing one HA roll keeps another roll sharing its committed profile unchanged", "[HaMaterialProfile][Regression]")
{
    PresetBundle bundle;
    auto material = exact_material();
    material.config.set_key_value("nozzle_temperature", new ConfigOptionInts{220});
    const auto profile = HaMaterialProfile::capture(material);
    const auto name = HaMaterialProfile::managed_name(profile);
    bundle.filaments.load_preset({}, name, HaMaterialProfile::decode_config(profile), false).filament_id = material.filament_id;
    bundle.set_num_filaments(2, std::vector<std::string>{"#112233", "#445566"});
    bundle.filament_presets = {name, name};
    HaMaterialBinding::write(bundle.project_config, {
        {profile_roll, profile_source, "printer"},
        {"641c70ee-6b92-44dc-b1c1-ddc1c86ce002", profile_source, "printer"}});
    const auto local_name = HaMaterialLocalOverride::isolate_edit_slot(bundle, 1);
    bundle.filaments.select_preset_by_name(local_name, true);
    bundle.filaments.get_edited_preset().config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
    bundle.filaments.get_edited_preset().config.set_key_value("nozzle_temperature", new ConfigOptionInts{230});
    const auto full = bundle.full_config();
    CHECK(full.opt_serialize("filament_flow_ratio") == "0.98,1.03");
    CHECK(full.opt_serialize("nozzle_temperature") == "220,230");
    CHECK(bundle.filament_presets[0] == name);
    CHECK(HaMaterialProfile::stored_preset(bundle.filaments, name)->config.opt_serialize("filament_flow_ratio") == "0.98");
}

TEST_CASE("HA slot edits survive switching among twenty slots with identical committed settings", "[HaMaterialProfile][Regression]")
{
    PresetBundle bundle;
    auto material = exact_material();
    material.config.set_key_value("nozzle_temperature", new ConfigOptionInts{220});
    const auto profile = HaMaterialProfile::capture(material);
    const auto name = HaMaterialProfile::managed_name(profile);
    bundle.filaments.load_preset({}, name, HaMaterialProfile::decode_config(profile), false).filament_id = material.filament_id;
    bundle.set_num_filaments(20, std::vector<std::string>(20, "#112233"));
    bundle.filament_presets.assign(20, name);
    HaMaterialBinding::write(bundle.project_config,
        std::vector<HaMaterialBinding::Binding>(20, {profile_roll, profile_source, "printer"}));
    for (size_t index = 0; index < 20; ++index) {
        const auto local_name = HaMaterialLocalOverride::isolate_edit_slot(bundle, index);
        bundle.filaments.select_preset_by_name(local_name, true);
        CHECK(HaMaterialProfile::capture(bundle.filaments.get_edited_preset(), profile) == profile);
        bundle.filaments.get_edited_preset().config.set_key_value("nozzle_temperature", new ConfigOptionInts{230 + int(index)});
    }
    const auto full = bundle.full_config();
    CHECK(full.opt_serialize("nozzle_temperature") == "230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249");
    CHECK(std::set<std::string>(bundle.filament_presets.begin(), bundle.filament_presets.end()).size() == 20);
    const auto reopened_name = HaMaterialLocalOverride::isolate_edit_slot(bundle, 0);
    bundle.filaments.select_preset_by_name(reopened_name, true);
    CHECK(bundle.filaments.get_edited_preset().config.opt_serialize("nozzle_temperature") == "230");
    bundle.filaments.get_edited_preset().config.set_key_value("nozzle_temperature", new ConfigOptionInts{251});
    CHECK(HaMaterialLocalOverride::isolate_edit_slot(bundle, 0) == reopened_name);
    CHECK(bundle.filaments.get_edited_preset().config.opt_serialize("nozzle_temperature") == "251");
    CHECK(HaMaterialProfile::stored_preset(bundle.filaments, name)->config.opt_serialize("nozzle_temperature") == "220");
}

TEST_CASE("HA slot isolation keeps ordinary preset assignments unchanged", "[HaMaterialProfile]")
{
    PresetBundle bundle;
    const auto material = exact_material();
    bundle.filaments.load_preset({}, material.name, material.config, false);
    bundle.filament_presets = {material.name};
    CHECK(HaMaterialLocalOverride::isolate_edit_slot(bundle, 0) == material.name);
    CHECK(bundle.filament_presets[0] == material.name);
}

TEST_CASE("Retained HA slot edits stay pending against their committed baseline after reopening the editor", "[HaMaterialProfile][Regression]")
{
    PresetBundle bundle;
    const auto profile = HaMaterialProfile::capture(exact_material());
    const auto name = HaMaterialProfile::managed_name(profile);
    bundle.filaments.load_preset({}, name, HaMaterialProfile::decode_config(profile), false).filament_id = "GFA00";
    bundle.set_num_filaments(2, std::vector<std::string>(2, "#112233"));
    bundle.filament_presets = {name, name};
    HaMaterialBinding::write(bundle.project_config,
        std::vector<HaMaterialBinding::Binding>(2, {profile_roll, profile_source, "printer"}));
    const auto first = HaMaterialLocalOverride::isolate_edit_slot(bundle, 0);
    bundle.filaments.select_preset_by_name(first, true);
    CHECK_FALSE(HaMaterialLocalOverride::has_local_edits(bundle));
    bundle.filaments.get_edited_preset().config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
    const auto second = HaMaterialLocalOverride::isolate_edit_slot(bundle, 1);
    bundle.filaments.select_preset_by_name(second, true);
    HaMaterialLocalOverride::isolate_edit_slot(bundle, 0);
    bundle.filaments.select_preset_by_name(first, true);
    CHECK_FALSE(bundle.filaments.current_is_dirty());
    CHECK(HaMaterialLocalOverride::has_local_edits(bundle));
    const auto* baseline = HaMaterialLocalOverride::edit_baseline(bundle);
    REQUIRE(baseline != nullptr);
    CHECK(baseline->config.opt_serialize("filament_flow_ratio") == "0.98");
    auto incoming = profile;
    incoming["settings"]["filament_flow_ratio"] = "1.00";
    const auto settings = HaMaterialLocalOverride::merge_settings(
        HaMaterialProfile::capture(*baseline, profile).at("settings"),
        HaMaterialProfile::capture(bundle.filaments.get_edited_preset(), profile).at("settings"),
        incoming.at("settings"));
    CHECK(settings.at("filament_flow_ratio") == "1.03");
}

TEST_CASE("HA project slot settings reopen through embedded presets without the downloaded cache", "[HaMaterialProfile][Regression]")
{
    PresetBundle bundle;
    auto material = exact_material();
    material.config.set_key_value("nozzle_temperature", new ConfigOptionInts{220});
    const auto profile = HaMaterialProfile::capture(material);
    const auto name = HaMaterialProfile::managed_name(profile);
    bundle.filaments.load_preset({}, name, HaMaterialProfile::decode_config(profile), false).filament_id = material.filament_id;
    bundle.set_num_filaments(3, std::vector<std::string>(3, "#112233"));
    bundle.filament_presets = {name, name, material.name};
    bundle.filaments.load_preset({}, material.name, material.config, false);
    HaMaterialBinding::write(bundle.project_config, {
        {profile_roll, profile_source, "printer"},
        {"641c70ee-6b92-44dc-b1c1-ddc1c86ce002", profile_source, "printer"}, {}});
    const auto first = HaMaterialLocalOverride::isolate_edit_slot(bundle, 0);
    bundle.filaments.select_preset_by_name(first, true);
    bundle.filaments.get_edited_preset().config.set_key_value("nozzle_temperature", new ConfigOptionInts{230});
    const auto second = HaMaterialLocalOverride::isolate_edit_slot(bundle, 1);
    bundle.filaments.select_preset_by_name(second, true);
    bundle.filaments.get_edited_preset().config.set_key_value("nozzle_temperature", new ConfigOptionInts{240});
    const auto ordinary = HaMaterialLocalOverride::isolate_edit_slot(bundle, 2);
    bundle.filaments.select_preset_by_name(ordinary, true);

    auto embedded = bundle.get_current_project_embedded_presets();
    REQUIRE(embedded.size() == 2);
    PresetBundle reopened;
    REQUIRE(HaMaterialProfile::stored_preset(reopened.filaments, name) == nullptr);
    CHECK_NOTHROW(reopened.load_project_embedded_presets(embedded, ForwardCompatibilitySubstitutionRule::Disable));
    for (auto* preset : embedded) delete preset;
    const auto* restored_first = HaMaterialProfile::stored_preset(reopened.filaments, first);
    const auto* restored_second = HaMaterialProfile::stored_preset(reopened.filaments, second);
    REQUIRE(restored_first != nullptr);
    REQUIRE(restored_second != nullptr);
    CHECK(restored_first->config.opt_serialize("nozzle_temperature") == "230");
    CHECK(restored_second->config.opt_serialize("nozzle_temperature") == "240");
    auto expected = profile;
    expected["settings"]["nozzle_temperature"] = "230";
    CHECK(HaMaterialProfile::capture(*restored_first, profile) == HaMaterialProfile::seal(expected));
}

static HaProjectMaterialSync::Row profile_row(const nlohmann::json& profile)
{
    return {{"", profile_roll, "Vendor", "Stock roll", "PLA", "#FFFFFF", profile.at("name"), 2,
             HaMaterialProfile::profile_summary(profile)}, profile.at("name"), "", 600000};
}

TEST_CASE("Same-name material setting differences are selected and sent with their HA digest baseline", "[HaMaterialProfileSync]")
{
    auto preset = exact_material();
    const auto before = HaMaterialProfile::capture(preset);
    const std::vector<HaProjectMaterialSync::Row> rows{profile_row(before)};
    preset.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
    const auto after = HaMaterialProfile::capture(preset);
    const HaProjectMaterialSync::Project project{{preset.name}, {"#FFFFFF"}, {after}};
    const std::vector<HaMaterialBinding::Binding> bindings{{profile_roll, profile_source, "printer"}};
    const auto differences = HaProjectMaterialDifferences::compare(project, rows, bindings, profile_source, "printer");
    REQUIRE(differences.size() == 1);
    CHECK(differences[0].field == HaProjectMaterialDifferences::Field::Profile);
    CHECK(differences[0].before == differences[0].after);
    CHECK(HaProjectMaterialDifferences::selected_rolls(differences, {false}, rows).empty());
    const auto selected = HaProjectMaterialDifferences::selected_rolls(differences, {true}, rows);
    REQUIRE(selected.size() == 1);
    const nlohmann::json remote = {{"schema_version", 8}, {"tables", {
        {"spools", nlohmann::json::array({{{"id", profile_roll}, {"status", "active"},
          {"nominal_capacity_mg", 1000000}, {"filament_preset_id", preset.name}, {"color_hex", "#FFFFFF"}}})},
        {"stock_events", nlohmann::json::array({{{"spool_id", profile_roll}, {"delta_mg", 600000}}})}}}};
    const auto changes = HaProjectRollPublish::publish_roll_changes(project, remote, selected);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].at("material_profile") == after);
    CHECK(changes[0].at("expected_profile_sha256") == before.at("sha256"));
    CHECK_FALSE(changes[0].contains("remaining_mg"));
    auto payload = nlohmann::json{{"revision", 2}, {"request_key", "explicit-profile-test"},
        {"confirmed", true}, {"changes", changes}, {"concurrency", "fields"}, {"response", "ack"}};
    CHECK_NOTHROW(HaProjectRollPublish::validate_payload(payload));
    payload["changes"][0]["material_profile"]["settings"]["filament_flow_ratio"] = "0.1";
    CHECK_THROWS(HaProjectRollPublish::validate_payload(payload));
    CHECK_THROWS(HaProjectMaterialDifferences::selected_rolls(differences, {true}, {profile_row(after)}));

    auto color_project = project;
    color_project.colors[0] = "#FF0000";
    auto color_selection = selected;
    color_selection[0].color_only = true;
    color_selection[0].profile_only = false;
    const auto color_changes = HaProjectRollPublish::publish_roll_changes(color_project, remote, color_selection);
    REQUIRE(color_changes.size() == 1);
    CHECK_FALSE(color_changes[0].contains("material_profile"));
    CHECK_FALSE(color_changes[0].at("fields").contains("filament_preset_id"));
}

TEST_CASE("An unmodified downloaded HA profile has no upload difference", "[HaMaterialProfileSync]")
{
    const auto profile = HaMaterialProfile::capture(exact_material());
    const HaProjectMaterialSync::Project project{{HaMaterialProfile::managed_name(profile)}, {"#FFFFFF"}, {profile}};
    const auto result = HaProjectMaterialDifferences::compare(project, {profile_row(profile)},
        {{profile_roll, profile_source, "printer"}}, profile_source, "printer");
    CHECK(result.empty());
}

TEST_CASE("An unchanged HA base profile is not a change when its nozzle variant is absent", "[HaMaterialProfileSync]")
{
    const auto profile = HaMaterialProfile::capture(exact_material());
    auto row = profile_row(profile);
    const nlohmann::json context = {{"printer_model", "Bambu Lab P2S"}, {"nozzle_diameter", "0.4"}, {"flow_type", "high_flow"}};
    row.assignment = HaMaterialSource::with_context(row.assignment, context);
    REQUIRE(HaMaterialSource::variant_profile(row.assignment).is_null());
    const HaProjectMaterialSync::Project project{{HaMaterialProfile::managed_name(profile)}, {"#FFFFFF"}, {profile}};
    CHECK(HaProjectMaterialDifferences::compare(project, {row},
        {{profile_roll, profile_source, "printer"}}, profile_source, "printer").empty());

    auto changed = profile;
    changed["settings"]["filament_flow_ratio"] = "1.03";
    changed = HaMaterialProfile::seal(changed);
    const auto differences = HaProjectMaterialDifferences::compare(
        {{HaMaterialProfile::managed_name(profile)}, {"#FFFFFF"}, {changed}}, {row},
        {{profile_roll, profile_source, "printer"}}, profile_source, "printer");
    REQUIRE(differences.size() == 1);
    CHECK(differences[0].before_profile_sha256.empty());
    CHECK(differences[0].after_profile_sha256 == HaMaterialProfile::digest(changed));
    CHECK_NOTHROW(HaProjectMaterialDifferences::selected_rolls(differences, {true}, {row}));
    auto selections = HaProjectMaterialDifferences::selected_rolls(differences, {true}, {row});
    REQUIRE(selections.size() == 1);
    CHECK(selections[0].expected_profile_sha256.empty());
    selections[0].profile_context = context;
    const nlohmann::json remote = {{"schema_version", 8}, {"tables", {
        {"spools", nlohmann::json::array({{{"id", profile_roll}, {"status", "active"},
          {"nominal_capacity_mg", 1000000}, {"filament_preset_id", profile.at("name")}, {"color_hex", "#FFFFFF"}}})},
        {"stock_events", nlohmann::json::array({{{"spool_id", profile_roll}, {"delta_mg", 600000}}})}}}};
    const auto changes = HaProjectRollPublish::publish_roll_changes(
        {{HaMaterialProfile::managed_name(profile)}, {"#FFFFFF"}, {changed}}, remote, selections);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].at("expected_profile_sha256").is_null());
    CHECK(changes[0].at("profile_context") == context);
    CHECK(changes[0].at("material_profile") == changed);
}

TEST_CASE("A name-only HA association offers initial storage without claiming different settings", "[HaMaterialProfileSync]")
{
    const auto profile = HaMaterialProfile::capture(exact_material());
    auto row = profile_row(profile);
    row.assignment.material_profile = nullptr;
    const auto differences = HaProjectMaterialDifferences::compare(
        {{profile.at("name")}, {"#FFFFFF"}, {profile}}, {row},
        {{profile_roll, profile_source, "printer"}}, profile_source, "printer");
    REQUIRE(differences.size() == 1);
    CHECK(differences[0].before == differences[0].after);
    CHECK(differences[0].before_profile_sha256.empty());
    CHECK_FALSE(differences[0].after_profile_sha256.empty());
}

TEST_CASE("A roll used with conflicting same-name material settings cannot be uploaded", "[HaMaterialProfileSync]")
{
    auto preset = exact_material();
    const auto first = HaMaterialProfile::capture(preset);
    preset.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
    const auto second = HaMaterialProfile::capture(preset);
    const HaProjectMaterialSync::Project project{{preset.name, preset.name}, {"#FFFFFF", "#FFFFFF"}, {first, second}};
    const auto result = HaProjectMaterialDifferences::compare(project, {profile_row(first)},
        {{profile_roll, profile_source, "printer"}, {profile_roll, profile_source, "printer"}}, profile_source, "printer");
    REQUIRE(result.size() == 1);
    CHECK_FALSE(result[0].problem.empty());
    CHECK_THROWS(HaProjectMaterialDifferences::selected_rolls(result, {true}, {profile_row(first)}));
}

TEST_CASE("HA material assignments retain their profile content identity", "[HaMaterialProfileSync]")
{
    const auto profile = HaMaterialProfile::capture(exact_material());
    const nlohmann::json spool = {{"uuid", profile_roll}, {"manufacturer", "Vendor"}, {"product", "Roll"},
        {"material_type", "PLA"}, {"color", "#FFFFFF"}, {"material_preset", profile.at("name")},
        {"material_profile", HaMaterialProfile::profile_summary(profile)}};
    const auto assignment = HaMaterialSource::assignment_from_spool(spool, "A1", 2);
    CHECK(assignment.material_profile == spool.at("material_profile"));
}

TEST_CASE("Reopened HA material aliases preserve publication metadata and local override detection", "[HaMaterialProfile][Regression]")
{
    const auto original = HaMaterialProfile::capture(exact_material());
    const auto canonical = HaMaterialProfile::managed_name(original);
    for (const auto& suffix : {"(Sink+strainer+with+plug.3mf)", "-3(Sink+strainer+with+plug.3mf)",
             " [project filament 3](Project (copy).3mf)"}) {
        Preset reopened(Preset::TYPE_FILAMENT, canonical + suffix);
        reopened.config = HaMaterialProfile::decode_config(original);
        reopened.filament_id = original.at("dependencies").at("filament_id");
        reopened.is_project_embedded = true;
        const HaMaterialBinding::Binding binding{profile_roll, profile_source, "printer",
            "Bambu Lab P2S|0.4|high_flow", canonical, HaMaterialProfile::digest(original)};
        CAPTURE(reopened.name);
        CHECK(HaMaterialProfile::capture(reopened, original) == original);
        CHECK_FALSE(HaMaterialBinding::preserves_override(binding, &reopened));
        CHECK_FALSE(HaMaterialLocalOverride::has_local_edits(reopened));

        reopened.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.03});
        const auto changed = HaMaterialProfile::capture(reopened, original);
        CHECK(changed.at("name") == original.at("name"));
        CHECK(changed.at("dependencies") == original.at("dependencies"));
        CHECK(changed.at("settings").at("filament_flow_ratio") == "1.03");
        CHECK(HaMaterialBinding::preserves_override(binding, &reopened));
        CHECK(HaMaterialLocalOverride::has_local_edits(reopened));
    }
}
