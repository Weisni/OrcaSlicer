#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaProjectMaterialSync.hpp"

using namespace Slic3r::HaProjectMaterialSync;

static std::vector<Row> ha_project_rows()
{
    return {{{"A1", "roll-a", "Vendor", "PLA", "PLA", "#FFFFFF", "Special PLA", 3}, "Special PLA"},
            {{"A2", "roll-b", "Vendor", "PETG", "PETG", "#123456", "", 4}, "Generic PETG"}};
}

TEST_CASE("Selected HA mappings preserve all unchecked project filaments and their indices", "[HaProjectMaterialSync]")
{
    const Project original{{"Existing 1", "Existing 2", "Existing 3"}, {"#000001", "#000002", "#000003"}};
    const auto rows = ha_project_rows();
    const auto result = stage(original, rows, rows, {{1, 2, false}});
    CHECK(result.presets == std::vector<std::string>{"Existing 1", "Existing 2", "Generic PETG"});
    CHECK(result.colors == std::vector<std::string>{"#000001", "#000002", "#123456"});
    CHECK(original.presets[2] == "Existing 3");
}

TEST_CASE("Color-only HA synchronization preserves every material preset", "[HaProjectMaterialSync]")
{
    const Project original{{"Custom dirty PLA", "Existing PETG"}, {"#000001", "#000002"}};
    auto rows = ha_project_rows();
    rows[0].resolved_preset.clear();
    const auto result = stage(original, rows, rows, {{0, 0, true}});
    CHECK(result.presets == original.presets);
    CHECK(result.colors == std::vector<std::string>{"#FFFFFF", "#000002"});
}

TEST_CASE("No selected HA rows leave the complete project selection unchanged", "[HaProjectMaterialSync]")
{
    const Project original{{"Project PLA"}, {"#000001"}};
    const auto rows = ha_project_rows();
    const auto result = stage(original, rows, {}, {});
    CHECK(result.presets == original.presets);
    CHECK(result.colors == original.colors);
}

TEST_CASE("Duplicate HA target mappings fail without a partial project update", "[HaProjectMaterialSync]")
{
    const Project original{{"Project PLA"}, {"#000001"}};
    const auto rows = ha_project_rows();
    CHECK_THROWS(stage(original, rows, rows, {{0, 0, false}, {1, 0, false}}));
    CHECK(original.presets[0] == "Project PLA");
    CHECK(original.colors[0] == "#000001");
}

TEST_CASE("Changed HA assignments and profile resolution reject the reviewed batch atomically", "[HaProjectMaterialSync]")
{
    const Project original{{"Project PLA", "Project PETG"}, {"#000001", "#000002"}};
    const auto rows = ha_project_rows();
    SECTION("Slot revision changes") {
        auto fresh = rows;
        ++fresh[1].assignment.revision;
        CHECK_THROWS(stage(original, rows, fresh, {{0, 0, false}, {1, 1, false}}));
    }
    SECTION("Roll identity changes") {
        auto fresh = rows;
        fresh[1].assignment.spool_uuid = "different-roll";
        CHECK_THROWS(stage(original, rows, fresh, {{0, 0, false}, {1, 1, false}}));
    }
    SECTION("Profile or color changes without a slot revision") {
        auto fresh = rows;
        fresh[1].assignment.color = "#ABCDEF";
        CHECK_THROWS(stage(original, rows, fresh, {{0, 0, false}, {1, 1, false}}));
        fresh = rows;
        fresh[1].resolved_preset = "Different Generic PETG";
        CHECK_THROWS(stage(original, rows, fresh, {{0, 0, false}, {1, 1, false}}));
    }
    SECTION("Installed profile content changes under the same preset name") {
        auto fresh = rows;
        fresh[1].resolved_config = "changed flow ratio";
        CHECK_THROWS(stage(original, rows, fresh, {{0, 0, false}, {1, 1, false}}));
    }
    CHECK(original.presets == std::vector<std::string>{"Project PLA", "Project PETG"});
    CHECK(original.colors == std::vector<std::string>{"#000001", "#000002"});
}

TEST_CASE("Unselected HA changes do not invalidate an independent selected mapping", "[HaProjectMaterialSync]")
{
    const Project original{{"Project PLA", "Project PETG"}, {"#000001", "#000002"}};
    const auto rows = ha_project_rows();
    auto fresh = rows;
    fresh.erase(fresh.begin() + 1);
    const auto result = stage(original, rows, fresh, {{0, 0, false}});
    CHECK(result.presets[0] == "Special PLA");
    CHECK(result.presets[1] == "Project PETG");
}

TEST_CASE("Missing presets and invalid project targets cannot change a project", "[HaProjectMaterialSync]")
{
    const Project original{{"Project PLA"}, {"#000001"}};
    auto rows = ha_project_rows();
    rows[0].resolved_preset.clear();
    CHECK_THROWS(stage(original, rows, rows, {{0, 0, false}}));
    CHECK_THROWS(stage(original, rows, rows, {{0, size_t(-1), true}}));
    CHECK_THROWS(stage(original, rows, rows, {{rows.size(), 0, true}}));
    CHECK(original.presets[0] == "Project PLA");
}

TEST_CASE("Profile-only project loading preserves its current color", "[HaProjectMaterialSync]")
{
    const Project project{{"Old PLA"}, {"#010203"}};
    const auto rows = ha_project_rows();
    const auto loaded = stage(project, rows, rows, {{0, 0, false, true}});
    CHECK(loaded.presets == std::vector<std::string>{"Special PLA"});
    CHECK(loaded.colors == std::vector<std::string>{"#010203"});
}

TEST_CASE("Project material publication transfers only the chosen role fields", "[HaProjectMaterialSync]")
{
    const Project project{{"Installed PLA", "Installed PETG"}, {"#ABCDEF", "#654321"}};
    const auto rows = ha_project_rows();
    const nlohmann::json remote = {{"tables", {{"spools", nlohmann::json::array({
        {{"id", "roll-a"}, {"filament_preset_id", "Special PLA"}, {"color_hex", "#FFFFFF"}, {"status", "active"}},
        {{"id", "roll-b"}, {"filament_preset_id", ""}, {"color_hex", "#123456"}, {"status", "active"}}
    })}}}};
    SECTION("Profile and color") {
        const auto changes = save_changes(project, rows, rows, {{0, 0, false}}, remote);
        REQUIRE(changes.size() == 1);
        CHECK(changes[0] == nlohmann::json({{"spool_uuid", "roll-a"},
            {"fields", {{"filament_preset_id", "Installed PLA"}, {"color_hex", "#ABCDEF"}}},
            {"expected", {{"filament_preset_id", "Special PLA"}, {"color_hex", "#FFFFFF"}}}}));
    }
    SECTION("Profile only") {
        const auto changes = save_changes(project, rows, rows, {{0, 0, false, true}}, remote);
        REQUIRE(changes.size() == 1);
        CHECK(changes[0].at("fields") == nlohmann::json({{"filament_preset_id", "Installed PLA"}}));
    }
    SECTION("Color only") {
        const auto changes = save_changes(project, rows, rows, {{1, 1, true}}, remote);
        REQUIRE(changes.size() == 1);
        CHECK(changes[0].at("spool_uuid") == "roll-b");
        CHECK(changes[0].at("fields") == nlohmann::json({{"color_hex", "#654321"}}));
    }
    SECTION("Slot changes and duplicate roll mappings reject the entire batch") {
        auto fresh = rows;
        fresh[0].assignment.spool_uuid = "different-roll";
        CHECK_THROWS(save_changes(project, rows, fresh, {{0, 0, false}}, remote));
        CHECK_THROWS(save_changes(project, rows, rows, {{0, 0, false}, {0, 1, true}}, remote));
    }
    CHECK(remote.at("tables").at("spools")[0].at("status") == "active");
}

TEST_CASE("Importing additional HA rolls appends project filaments without shifting existing indices", "[HaProjectMaterialSync]")
{
    const Project original{{"Existing PLA"}, {"#000001"}};
    const auto rows = ha_project_rows();
    const auto result = stage(original, rows, rows, {{0, 0, false}, {1, 1, false}});
    CHECK(result.presets == std::vector<std::string>{"Special PLA", "Generic PETG"});
    CHECK(result.colors == std::vector<std::string>{"#FFFFFF", "#123456"});
    CHECK(original.presets == std::vector<std::string>{"Existing PLA"});
}

TEST_CASE("Contiguous appended HA mappings may be selected in any order", "[HaProjectMaterialSync]")
{
    const Project original{{"Existing PLA"}, {"#000001"}};
    const auto rows = ha_project_rows();
    const auto result = stage(original, rows, rows, {{1, 2, false}, {0, 1, false}});
    CHECK(result.presets == std::vector<std::string>{"Existing PLA", "Special PLA", "Generic PETG"});
    CHECK(result.colors == std::vector<std::string>{"#000001", "#FFFFFF", "#123456"});
}

TEST_CASE("New project materials reject incomplete fields or gaps atomically", "[HaProjectMaterialSync]")
{
    const Project original{{"Existing PLA"}, {"#000001"}};
    const auto rows = ha_project_rows();
    CHECK_THROWS(stage(original, rows, rows, {{0, 1, true}}));
    CHECK_THROWS(stage(original, rows, rows, {{0, 1, false, true}}));
    CHECK_THROWS(stage(original, rows, rows, {{0, 2, false}}));
    CHECK_THROWS(stage(original, rows, rows, {{0, 0, false}, {1, 2, false}}));
    CHECK(original.presets == std::vector<std::string>{"Existing PLA"});
    CHECK(original.colors == std::vector<std::string>{"#000001"});
}

TEST_CASE("Empty HA slots cannot replace existing project materials", "[HaProjectMaterialSync]")
{
    const Project original{{"Existing PLA"}, {"#000001"}};
    const std::vector<Row> rows{{{"HT1", "", "", "", "", "", "", 2}, ""}};
    CHECK_THROWS(stage(original, rows, rows, {{0, 0, true}}));
    CHECK(original.colors == std::vector<std::string>{"#000001"});
}

TEST_CASE("Import all occupied slots skips empty slots and unassigned inventory rolls", "[HaProjectMaterialSync]")
{
    auto rows = ha_project_rows();
    rows.insert(rows.begin() + 1, {{"A2", "", "", "", "", "", "", 2}, ""});
    rows[2].assignment.slot = "HT1";
    rows.push_back({{"", "stored-roll", "Vendor", "PLA", "PLA", "#AABBCC", "", 0}, "Generic PLA"});
    const auto selected = all_occupied_selections(rows);
    REQUIRE(selected.size() == 2);
    CHECK(selected[0].source_index == 0);
    CHECK(selected[0].project_index == 0);
    CHECK_FALSE(selected[0].color_only);
    CHECK_FALSE(selected[0].profile_only);
    CHECK(selected[1].source_index == 2);
    CHECK(selected[1].project_index == 1);
    const Project original{{"Existing PLA", "Existing PETG", "Unused ABS"}, {"#000001", "#000002", "#000003"}};
    const auto result = stage(original, rows, rows, selected);
    CHECK(result.presets == std::vector<std::string>{"Special PLA", "Generic PETG", "Unused ABS"});
    CHECK(result.colors == std::vector<std::string>{"#FFFFFF", "#123456", "#000003"});
}

TEST_CASE("An entirely empty HA material system imports no project changes", "[HaProjectMaterialSync]")
{
    const std::vector<Row> rows{{{"HT1", "", "", "", "", "", "", 2}, ""}};
    const auto selected = all_occupied_selections(rows);
    CHECK(selected.empty());
    const Project original{{"Existing PLA"}, {"#000001"}};
    const auto result = stage(original, rows, rows, selected);
    CHECK(result.presets == original.presets);
    CHECK(result.colors == original.colors);
}

TEST_CASE("Appending HA materials preserves existing project color metadata and mappings", "[HaProjectMaterialSync]")
{
    Slic3r::PresetBundle bundle;
    bundle.filament_presets = {bundle.filaments.get_selected_preset_name(), bundle.filaments.get_selected_preset_name()};
    bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_colour")->values = {"#AABBCC", "#112233"};
    bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_multi_colour")->values = {"#AABBCC;#FFFFFF", "#112233"};
    bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_colour_type")->values = {"0", "1"};
    bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_map")->values = {2, 1};
    bundle.project_config.option<Slic3r::ConfigOptionFloats>("flush_volumes_matrix")->values = {0., 140., 140., 0.};
    bundle.project_config.option<Slic3r::ConfigOptionFloats>("flush_volumes_vector")->values = {140., 140., 140., 140.};
    bundle.project_config.option<Slic3r::ConfigOptionFloats>("flush_multiplier")->values = {1.};
    bundle.ams_multi_color_filment = {{"#AABBCC", "#FFFFFF"}, {"#112233"}};
    const auto original_presets = bundle.filament_presets;
    Project staged{original_presets, {"#AABBCC", "#112233", "#ABCDEF"}};
    staged.presets.push_back("New HA material");
    extend_project(bundle, staged);
    REQUIRE(bundle.filament_presets.size() == 3);
    CHECK(bundle.filament_presets[0] == original_presets[0]);
    CHECK(bundle.filament_presets[1] == original_presets[1]);
    CHECK(bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_colour")->values == staged.colors);
    CHECK(bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_multi_colour")->values ==
          std::vector<std::string>{"#AABBCC;#FFFFFF", "#112233", "#ABCDEF"});
    CHECK(bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_colour_type")->values ==
          std::vector<std::string>{"0", "1", "1"});
    CHECK(bundle.project_config.option<Slic3r::ConfigOptionInts>("filament_map")->values == std::vector<int>{2, 1, 1});
    CHECK(bundle.ams_multi_color_filment == std::vector<std::vector<std::string>>{{"#AABBCC", "#FFFFFF"}, {"#112233"}, {}});
}

TEST_CASE("Invalid HA project growth preserves the existing bundle", "[HaProjectMaterialSync]")
{
    Slic3r::PresetBundle bundle;
    bundle.filament_presets = {bundle.filaments.get_selected_preset_name()};
    bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_colour")->values = {"#AABBCC"};
    bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_multi_colour")->values = {"#AABBCC"};
    bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_colour_type")->values = {"1"};
    bundle.project_config.option<Slic3r::ConfigOptionFloats>("flush_volumes_matrix")->values = {0.};
    bundle.project_config.option<Slic3r::ConfigOptionFloats>("flush_multiplier")->values = {1.};
    const auto original = bundle.project_config;
    const auto original_presets = bundle.filament_presets;
    CHECK_THROWS(extend_project(bundle, {{"PLA", "PETG"}, {"#AABBCC"}}));
    CHECK_THROWS(extend_project(bundle, {std::vector<std::string>(17, "PLA"), std::vector<std::string>(17, "#AABBCC")}));
    CHECK(bundle.project_config == original);
    CHECK(bundle.filament_presets == original_presets);
    bundle.filament_presets.resize(17, original_presets[0]);
    const Project oversized{bundle.filament_presets, std::vector<std::string>(17, "#AABBCC")};
    CHECK_NOTHROW(extend_project(bundle, oversized));
    CHECK(bundle.filament_presets == oversized.presets);
}

TEST_CASE("Local-only inventory rows cannot masquerade as HA imports", "[HaProjectMaterialSync]")
{
    const Project original{{"Existing PLA"}, {"#000001"}};
    auto rows = ha_project_rows();
    rows[0].local_only = true;
    CHECK_THROWS(stage(original, rows, rows, {{0, 0, false}}));
    CHECK_THROWS(stage(original, rows, rows, {{0, 0, true}}));
    auto reviewed = rows;
    reviewed[0].local_only = false;
    CHECK_THROWS(stage(original, reviewed, rows, {{0, 0, false}}));
}
