#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaProjectRollPublish.hpp"
#include "libslic3r/HaProjectMaterialDifferences.hpp"

using namespace Slic3r::HaProjectRollPublish;

static const std::string roll_id = "641c70ee-6b92-44dc-b1c1-ddc1c86ce001";
static const std::string new_id = "641c70ee-6b92-44dc-b1c1-ddc1c86ce002";

static Json remote_rolls()
{
    return {{"schema_version", 8}, {"tables", {
        {"spools", Json::array({{{"id", roll_id}, {"name", "Existing roll"},
            {"manufacturer", "Maker"}, {"material_type", "PLA"},
            {"filament_preset_id", "Original PLA"}, {"color_hex", "#112233"},
            {"status", "active"}, {"nominal_capacity_mg", 1000000}}})},
        {"stock_events", Json::array({{{"spool_id", roll_id}, {"delta_mg", 600000}}})},
        {"allocations", Json::array()}, {"print_jobs", Json::array()}}}};
}

static Slic3r::HaProjectMaterialSync::Project project()
{
    return {{"Special PLA", "Other PETG"}, {"#FFFFFF", "#0088AA"}};
}

static RollPublish existing()
{
    return {0, roll_id, 600000};
}

static RollPublish new_roll()
{
    return {0, new_id, 750000, true, "Physical PLA roll", "Maker", "PLA", 1000000};
}

TEST_CASE("Publishing project fields preserves unchanged stock and its provenance", "[HaProjectRollPublish]")
{
    const auto remote = remote_rolls();
    const auto changes = publish_roll_changes(project(), remote, {existing()});
    REQUIRE(changes.size() == 1);
    CHECK(changes[0]["spool_uuid"] == roll_id);
    CHECK(changes[0]["fields"] == Json{{"filament_preset_id", "Special PLA"}, {"color_hex", "#FFFFFF"}});
    CHECK(changes[0]["expected"] == Json{{"filament_preset_id", "Original PLA"}, {"color_hex", "#112233"}});
    CHECK_FALSE(changes[0].contains("remaining_mg"));
    CHECK_FALSE(changes[0].contains("quality"));
    CHECK(remote == remote_rolls());
}

TEST_CASE("Explicit stock publication includes the HA baseline and estimated provenance", "[HaProjectRollPublish]")
{
    auto selected = existing();
    selected.remaining_mg = 450123;
    selected.color_only = true;
    const auto changes = publish_roll_changes(project(), remote_rolls(), {selected});
    REQUIRE(changes.size() == 1);
    CHECK(changes[0]["remaining_mg"] == 450123);
    CHECK(changes[0]["expected_remaining_mg"] == 600000);
    CHECK(changes[0]["quality"] == "estimated");
    CHECK_FALSE(changes[0]["fields"].contains("filament_preset_id"));
    selected.profile_only = true;
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
}

TEST_CASE("Metadata publication preserves legacy stock above nominal capacity", "[HaProjectRollPublish][Regression]")
{
    auto remote = remote_rolls();
    remote["tables"]["stock_events"][0]["delta_mg"] = 1050000;
    auto selected = existing();
    selected.remaining_mg = 1050000;
    const auto before = remote;
    const auto changes = publish_roll_changes(project(), remote, {selected});
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].contains("remaining_mg"));
    CHECK_FALSE(changes[0].contains("quality"));
    CHECK(changes[0]["fields"].contains("filament_preset_id"));
    CHECK(changes[0]["fields"].contains("color_hex"));
    CHECK(remote == before);
    selected.remaining_mg = 1040000;
    CHECK_THROWS(publish_roll_changes(project(), remote, {selected}));
    selected.remaining_mg = 990000;
    CHECK(publish_roll_changes(project(), remote, {selected})[0]["remaining_mg"] == 990000);
}

TEST_CASE("Creating a physical roll requires explicit identity metadata and stock", "[HaProjectRollPublish]")
{
    auto selected = new_roll();
    const auto changes = publish_roll_changes(project(), remote_rolls(), {selected});
    REQUIRE(changes.size() == 1);
    CHECK(changes[0]["create"] == true);
    CHECK(changes[0]["expected"].empty());
    CHECK(changes[0]["spool_uuid"] == new_id);
    CHECK(changes[0]["remaining_mg"] == 750000);
    CHECK(changes[0]["fields"]["status"] == "active");
    CHECK(changes[0]["fields"]["filament_preset_id"] == "Special PLA");
    CHECK(changes[0]["fields"]["name"] == "Physical PLA roll");
    CHECK_FALSE(changes[0].contains("expected_remaining_mg"));
    selected.remaining_mg = 0;
    CHECK(publish_roll_changes(project(), remote_rolls(), {selected})[0]["fields"]["status"] == "empty");
}

TEST_CASE("Unchanged project roll publication produces no transfer", "[HaProjectRollPublish]")
{
    auto same = project();
    same.presets[0] = "Original PLA";
    same.colors[0] = "#112233";
    CHECK(publish_roll_changes(same, remote_rolls(), {existing()}).empty());
    CHECK(publish_roll_changes(project(), remote_rolls(), {}).empty());
    auto profile = existing();
    profile.profile_only = true;
    const auto changes = publish_roll_changes(project(), remote_rolls(), {profile});
    REQUIRE(changes.size() == 1);
    CHECK(changes[0]["fields"].size() == 1);
    CHECK_FALSE(changes[0]["fields"].contains("color_hex"));
}

TEST_CASE("Publishing rejects invalid identities duplicate targets and contradictory amounts", "[HaProjectRollPublish]")
{
    for (const auto &id : {std::string("not-a-uuid"), std::string("641C70EE-6b92-44dc-b1c1-ddc1c86ce001")}) {
        auto selected = existing(); selected.uuid = id;
        CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
    }
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {existing(), existing()}));
    auto selected = new_roll(); selected.uuid = roll_id;
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
    selected = existing(); selected.uuid = new_id;
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
    selected = existing(); selected.project_index = 2;
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
    selected = new_roll(); selected.remaining_mg = -1;
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
    selected.remaining_mg = 1000001;
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
    selected = new_roll(); selected.nominal_mg = 0;
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
    selected = new_roll(); selected.name = " ";
    CHECK_THROWS(publish_roll_changes(project(), remote_rolls(), {selected}));
}

TEST_CASE("Stock publication protects reservations and archived rolls while permitting profile links", "[HaProjectRollPublish]")
{
    auto remote = remote_rolls();
    remote["tables"]["print_jobs"].push_back({{"id", "job"}, {"state", "needs_review"}});
    remote["tables"]["allocations"].push_back({{"job_id", "job"}, {"spool_id", roll_id}, {"estimated_weight_mg", 0}});
    CHECK_NOTHROW(publish_roll_changes(project(), remote, {existing()}));
    auto stock = existing(); stock.remaining_mg = 500000;
    CHECK_THROWS(publish_roll_changes(project(), remote, {stock}));
    remote = remote_rolls(); remote["tables"]["spools"][0]["status"] = "archived";
    CHECK_THROWS(publish_roll_changes(project(), remote, {stock}));
}

TEST_CASE("Durable roll publication rejects unrelated fields and malformed stock provenance", "[HaProjectRollPublish]")
{
    const auto good = publish_roll_changes(project(), remote_rolls(), {new_roll()});
    REQUIRE(good.size() == 1);
    CHECK_NOTHROW(validate_changes(good));
    auto bad = good; bad[0]["fields"]["price_currency"] = "EUR";
    CHECK_THROWS(validate_changes(bad));
    bad = good; bad[0]["fields"]["status"] = "archived";
    CHECK_THROWS(validate_changes(bad));
    bad = good; bad[0]["quality"] = "measured";
    CHECK_THROWS(validate_changes(bad));
    bad = good; bad[0]["remaining_mg"] = 1.5;
    CHECK_THROWS(validate_changes(bad));
    bad = good; bad[0]["remaining_mg"] = true;
    CHECK_THROWS(validate_changes(bad));
    bad = good; bad[0]["expected_remaining_mg"] = 0;
    CHECK_THROWS(validate_changes(bad));
    bad = good; bad[0]["fields"]["nominal_capacity_mg"] = 100;
    CHECK_THROWS(validate_changes(bad));
    bad = good; bad[0]["slot"] = "A1";
    CHECK_THROWS(validate_changes(bad));
    auto stock = existing(); stock.remaining_mg = 0;
    bad = publish_roll_changes(project(), remote_rolls(), {stock});
    REQUIRE(bad.size() == 1);
    bad[0].erase("expected_remaining_mg");
    CHECK_THROWS(validate_changes(bad));
}

TEST_CASE("Pending project roll requests retain the exact confirmed scope", "[HaProjectRollPublish]")
{
    const Json good = {{"revision", 12}, {"request_key", "reviewed-create-1"},
        {"confirmed", true}, {"changes", publish_roll_changes(project(), remote_rolls(), {new_roll()})}};
    REQUIRE(good["changes"].size() == 1);
    CHECK_NOTHROW(validate_payload(good));
    auto bad = good; bad["confirmed"] = 1;
    CHECK_THROWS(validate_payload(bad));
    bad = good; bad["revision"] = -1;
    CHECK_THROWS(validate_payload(bad));
    bad = good; bad["request_key"] = "";
    CHECK_THROWS(validate_payload(bad));
    bad = good; bad["bundle"] = remote_rolls();
    CHECK_THROWS(validate_payload(bad));
    bad = good; bad["changes"].push_back(bad["changes"][0]);
    CHECK_THROWS(validate_payload(bad));
}

namespace {
namespace Differences = Slic3r::HaProjectMaterialDifferences;
using MaterialRow = Slic3r::HaProjectMaterialSync::Row;
using MaterialProject = Slic3r::HaProjectMaterialSync::Project;
using Binding = Slic3r::HaMaterialBinding::Binding;
MaterialRow comparison_row()
{
    return {{"", roll_id, "Maker", "Physical roll", "PLA", "#112233", "Original PLA", 1},
            "Original PLA", "", 600000};
}
Binding comparison_binding() { return {roll_id, "ha-source", "printer"}; }
}

TEST_CASE("Bound unmounted rolls compare profile and color independently", "[HaProjectRollPublish][HaProjectMaterialDifferences]")
{
    const MaterialProject current{{"Special PLA"}, {"#FFFFFF"}};
    const auto diff = Differences::compare(current, {comparison_row()}, {comparison_binding()}, "ha-source", "printer");
    REQUIRE(diff.size() == 2);
    CHECK(diff[0].field == Differences::Field::Profile);
    CHECK(diff[0].before == "Original PLA");
    CHECK(diff[0].after == "Special PLA");
    CHECK(diff[1].field == Differences::Field::Color);
    CHECK(diff[1].before == "#112233");
    CHECK(diff[1].after == "#FFFFFF");
    CHECK(diff[0].problem.empty());
    CHECK(diff[1].problem.empty());
    const auto all = Differences::selected_rolls(diff, {true, true}, {comparison_row()});
    REQUIRE(all.size() == 1);
    CHECK(all[0].uuid == roll_id);
    CHECK_FALSE(all[0].create);
    CHECK_FALSE(all[0].color_only);
    CHECK_FALSE(all[0].profile_only);
    CHECK(all[0].remaining_mg == 600000);
    const auto changes = publish_roll_changes(current, remote_rolls(), all);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0]["fields"].size() == 2);
    CHECK_FALSE(changes[0].contains("remaining_mg"));
    const auto color = Differences::selected_rolls(diff, {false, true}, {comparison_row()});
    REQUIRE(color.size() == 1);
    CHECK(color[0].color_only);
    CHECK_FALSE(color[0].profile_only);
    const auto profile = Differences::selected_rolls(diff, {true, false}, {comparison_row()});
    REQUIRE(profile.size() == 1);
    CHECK(profile[0].profile_only);
    CHECK_FALSE(profile[0].color_only);
    CHECK(Differences::selected_rolls(diff, {false, false}, {comparison_row()}).empty());
}

TEST_CASE("Comparisons preserve Generic fallback and ignore equivalent color spelling", "[HaProjectRollPublish][HaProjectMaterialDifferences]")
{
    auto row = comparison_row();
    row.assignment.color = "#aabbCC";
    row.assignment.material_preset.clear();
    row.resolved_preset = "Generic PLA @P2S";
    MaterialProject current{{row.resolved_preset}, {"#AABBcc"}};
    CHECK(Differences::compare(current, {row}, {comparison_binding()}, "ha-source", "printer").empty());
    row.assignment.material_preset = "Missing custom PLA";
    CHECK(Differences::compare(current, {row}, {comparison_binding()}, "ha-source", "printer").empty());
    current.presets[0] = "Special PLA";
    auto diff = Differences::compare(current, {row}, {comparison_binding()}, "ha-source", "printer");
    REQUIRE(diff.size() == 1);
    CHECK(diff[0].field == Differences::Field::Profile);
    CHECK(diff[0].after == "Special PLA");
}

TEST_CASE("Comparisons only include bindings from the current HA printer and existing remote rows", "[HaProjectRollPublish][HaProjectMaterialDifferences]")
{
    const MaterialProject current{{"Special PLA"}, {"#FFFFFF"}};
    auto binding = comparison_binding();
    binding.source = "other-source";
    CHECK(Differences::compare(current, {comparison_row()}, {binding}, "ha-source", "printer").empty());
    binding = comparison_binding(); binding.printer_id = "other-printer";
    CHECK(Differences::compare(current, {comparison_row()}, {binding}, "ha-source", "printer").empty());
    binding = comparison_binding(); binding.spool_uuid.clear();
    CHECK(Differences::compare(current, {comparison_row()}, {binding}, "ha-source", "printer").empty());
    CHECK(Differences::compare(current, {}, {comparison_binding()}, "ha-source", "printer").empty());
    auto row = comparison_row(); row.local_only = true;
    CHECK(Differences::compare(current, {row}, {comparison_binding()}, "ha-source", "printer").empty());
}

TEST_CASE("Repeated roll bindings deduplicate matching values and block conflicting fields", "[HaProjectRollPublish][HaProjectMaterialDifferences]")
{
    const std::vector<Binding> bindings{comparison_binding(), comparison_binding()};
    MaterialProject current{{"Special PLA", "Special PLA"}, {"#FFFFFF", "#ffffff"}};
    auto diff = Differences::compare(current, {comparison_row()}, bindings, "ha-source", "printer");
    REQUIRE(diff.size() == 2);
    CHECK(diff[0].problem.empty());
    CHECK(diff[1].problem.empty());
    CHECK(Differences::selected_rolls(diff, {true, true}, {comparison_row()}).size() == 1);
    // The first project value already matches HA: the other project's competing
    // value must still block that field instead of silently winning publication.
    current.presets[0] = "Original PLA";
    diff = Differences::compare(current, {comparison_row()}, bindings, "ha-source", "printer");
    REQUIRE(diff.size() == 2);
    CHECK_FALSE(diff[0].problem.empty());
    CHECK(diff[1].problem.empty());
    CHECK_THROWS(Differences::selected_rolls(diff, {true, false}, {comparison_row()}));
    const auto color = Differences::selected_rolls(diff, {false, true}, {comparison_row()});
    REQUIRE(color.size() == 1);
    CHECK(color[0].color_only);
    current.presets = {"Special PLA", "Special PLA"};
    current.colors = {"#112233", "#FFFFFF"};
    diff = Differences::compare(current, {comparison_row()}, bindings, "ha-source", "printer");
    REQUIRE(diff.size() == 2);
    CHECK(diff[0].problem.empty());
    CHECK_FALSE(diff[1].problem.empty());
}

TEST_CASE("Selected differences reject ambiguous payloads and preserve unchecked problems", "[HaProjectRollPublish][HaProjectMaterialDifferences]")
{
    using Differences::Difference;
    using Differences::Field;
    const std::vector<Difference> diff{{0, 0, Field::Profile, "Original PLA", "Special PLA", ""},
                                     {0, 0, Field::Profile, "Original PLA", "Other PLA", ""}};
    CHECK_THROWS(Differences::selected_rolls(diff, {true, true}, {comparison_row()}));
    CHECK_THROWS(Differences::selected_rolls(diff, {true}, {comparison_row()}));
    CHECK_THROWS(Differences::selected_rolls(diff, {true, false}, {}));
    auto blocked = diff; blocked[1].problem = "Conflicting profile";
    CHECK_NOTHROW(Differences::selected_rolls(blocked, {true, false}, {comparison_row()}));
    auto local = comparison_row(); local.local_only = true;
    CHECK_THROWS(Differences::selected_rolls(diff, {true, false}, {local}));

    auto agreeing = diff;
    agreeing[1].after = agreeing[0].after;
    agreeing[1].project_index = 1;
    const auto merged = Differences::selected_rolls(agreeing, {true, true}, {comparison_row()});
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].profile_only);
}
