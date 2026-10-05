#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include "libslic3r/HaMaterialSource.hpp"
#include "libslic3r/HaMaterialCatalog.hpp"
#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r::HaMaterialSource;
namespace HaMaterialContext = Slic3r::HaMaterialContext;

TEST_CASE("ASA plus rolls use ASA filament profiles without changing their physical type", "[HaMaterialSource]")
{
    CHECK(material_family("ASA+") == "ASA");
    CHECK(same_material_family("ASA+", "ASA"));
    CHECK_FALSE(same_material_family("ASA+", "ABS"));
    CHECK_FALSE(same_material_family("PLA+", "PLA"));
}

TEST_CASE("Nozzle profile contexts distinguish diameter and flow with canonical keys", "[HaMaterialSource]")
{
    CHECK(HaMaterialContext::key(HaMaterialContext::make("Bambu Lab P2S", 0.4, "standard")) == "Bambu Lab P2S|0.4|standard");
    CHECK(HaMaterialContext::key(HaMaterialContext::make("Bambu Lab P2S", 0.8, "high_flow")) == "Bambu Lab P2S|0.8|high_flow");
    CHECK_THROWS(HaMaterialContext::make("P2S|bad", 0.4, "standard"));
    CHECK_THROWS(HaMaterialContext::make("P2S", 0.4, "hybrid"));
    CHECK_THROWS(HaMaterialContext::make("P2S", 0.0, "standard"));
}

TEST_CASE("Active project nozzle flow takes precedence over the printer preset default", "[HaMaterialSource]")
{
    Slic3r::DynamicPrintConfig printer, project;
    printer.set_key_value("printer_model", new Slic3r::ConfigOptionString("Bambu Lab P2S"));
    printer.set_key_value("nozzle_diameter", new Slic3r::ConfigOptionFloats{0.8});
    printer.set_key_value("nozzle_volume_type", new Slic3r::ConfigOptionEnumsGeneric{0});
    project.set_key_value("nozzle_volume_type", new Slic3r::ConfigOptionEnumsGeneric{1});
    CHECK(HaMaterialContext::key(profile_context(printer, project)) == "Bambu Lab P2S|0.8|high_flow");
    project.set_key_value("nozzle_volume_type", new Slic3r::ConfigOptionEnumsGeneric{2});
    CHECK_THROWS(profile_context(printer, project));
}

TEST_CASE("Nozzle profile support is read from the actual provider capability envelope", "[HaMaterialSource]")
{
    nlohmann::json wire = {{"provider_api_version", 1}, {"capabilities", {
        {"native_apply_fields", true}, {"material_profiles_v1", true}, {"material_profile_variants_v1", true}}}};
    CHECK(supports_profile_variants(wire));
    wire["capabilities"].erase("material_profile_variants_v1");
    CHECK_FALSE(supports_profile_variants(wire));
    wire["material_profile_variants_v1"] = true;
    CHECK_FALSE(supports_profile_variants(wire));
}

static nlohmann::json snapshot()
{
    return {{"schema_version", 1}, {"demo_mode", true}, {"printer_id", "duck-poop-demo"},
        {"captured_unix", std::time(nullptr)},
        {"spools", {{{"uuid", "1d1aa340-8c40-42a1-832f-87a6a7dcf94f"},
            {"manufacturer", "Actual vendor"}, {"product", "Actual PLA"},
            {"material_type", "PLA"}, {"color", "#367AF5"},
            {"material_preset", "Actual PLA @P2S"}, {"bambu_material", "Bambu PLA"}}}},
        {"slots", {{{"id", "A1"}, {"spool_uuid", "1d1aa340-8c40-42a1-832f-87a6a7dcf94f"}, {"revision", 1}}}}};
}

TEST_CASE("ASA plus roll resolution uses compatible ASA profiles at standard and high flow diameters", "[HaMaterialSource]")
{
    for (const auto &nozzle : {std::string("0.4 standard"), std::string("0.8 high flow")}) {
        Slic3r::PresetBundle bundle;
        auto &filaments = bundle.filaments;
        Slic3r::DynamicPrintConfig config(filaments.default_preset().config);
        config.set_key_value("filament_type", new Slic3r::ConfigOptionStrings{"ASA"});
        auto &preset = filaments.load_preset(std::string(), "Generic ASA - No Warp " + nozzle, config, false);
        preset.is_compatible = true;
        const auto *resolved = resolve_preset(filaments, preset.name, "ASA+");
        REQUIRE(resolved != nullptr);
        CHECK(resolved->name == preset.name);
        CHECK(resolve_preset(filaments, preset.name, "ABS") == nullptr);
        preset.is_compatible = false;
        CHECK(resolve_preset(filaments, preset.name, "ASA+") == nullptr);
    }
}

TEST_CASE("Nozzle variants preserve legacy profiles and use only the requested association", "[HaMaterialSource]")
{
    auto data = snapshot();
    auto &spool = data["spools"][0];
    const nlohmann::json summary = {{"schema_version", 1}, {"name", "PLA HF"}, {"material_type", "PLA"},
        {"dependencies", {{"inherits", ""}, {"filament_id", ""}, {"vendor", ""}}}, {"sha256", std::string(64, 'a')}};
    const auto hf = HaMaterialContext::make("Bambu Lab P2S", 0.8, "high_flow");
    spool["material_profile_variants"] = {{HaMaterialContext::key(hf), summary}};
    const auto original = assignment_from_spool(spool, "A1", 1);
    const auto variant = with_context(original, hf);
    CHECK(variant.material_preset == "PLA HF");
    CHECK(variant.material_profile == summary);
    CHECK(original.material_preset == "Actual PLA @P2S");
    const auto other = with_context(original, HaMaterialContext::make("Bambu Lab P2S", 0.4, "standard"));
    CHECK(other.material_preset == original.material_preset);
    CHECK(variant_profile(other).is_null());
    spool["material_profile_variants"][HaMaterialContext::key(hf)]["material_type"] = "ABS";
    CHECK_THROWS(assignment_from_spool(spool, "A1", 1));
}

TEST_CASE("HA materials preserve exact presets and physical roll identity", "[HaMaterialSource]")
{
    const auto result = parse(snapshot().dump(), "duck-poop-demo");
    REQUIRE(result.size() == 1);
    CHECK(result[0].material_preset == "Actual PLA @P2S");
    CHECK(result[0].manufacturer == "Actual vendor");
    CHECK(result[0].spool_uuid == "1d1aa340-8c40-42a1-832f-87a6a7dcf94f");
}

TEST_CASE("HA materials reject unavailable identity rather than using printer labels", "[HaMaterialSource]")
{
    auto data = snapshot();
    data["slots"][0]["spool_uuid"] = "unknown";
    CHECK_THROWS(parse(data.dump(), "duck-poop-demo"));
    data = snapshot();
    data["spools"][0]["color"] = "not-a-color";
    CHECK_THROWS(parse(data.dump(), "duck-poop-demo"));
}

TEST_CASE("HA rolls without a special preset retain their material and physical identity", "[HaMaterialSource]")
{
    auto data = snapshot();
    data["spools"][0]["material_preset"] = "";
    data["spools"][0]["material_type"] = "PETG";
    const auto assignments = parse(data.dump(), "duck-poop-demo");
    REQUIRE(assignments.size() == 1);
    CHECK(assignments[0].material_preset.empty());
    CHECK(assignments[0].material_type == "PETG");
    CHECK(assignments[0].spool_uuid == data["spools"][0]["uuid"].get<std::string>());
}

TEST_CASE("HA materials reject conflicting slots wrong printers and stale snapshots", "[HaMaterialSource]")
{
    auto data = snapshot();
    data["slots"].push_back(data["slots"][0]);
    CHECK_THROWS(parse(data.dump(), "duck-poop-demo"));
    data = snapshot();
    CHECK_THROWS(parse(data.dump(), "another-printer"));
    data["captured_unix"] = std::time(nullptr) - 600;
    CHECK_THROWS(parse(data.dump(), "duck-poop-demo"));
    data = snapshot();
    data["demo_mode"] = false;
    CHECK_THROWS(parse(data.dump(), "duck-poop-demo"));
}

TEST_CASE("HA material snapshots retain empty slots in physical slot order", "[HaMaterialSource]")
{
    auto data = snapshot();
    data["slots"].insert(data["slots"].begin(), nlohmann::json{{"id", "EXT"}, {"spool_uuid", nullptr}, {"revision", 7}});
    data["slots"].push_back({{"id", "HT1"}, {"spool_uuid", nullptr}, {"revision", 6}});
    const auto result = parse(data.dump(), "duck-poop-demo");
    REQUIRE(result.size() == 3);
    CHECK(result[0].slot == "A1");
    CHECK(result[1].slot == "HT1");
    CHECK(result[2].slot == "EXT");
    CHECK(result[1].spool_uuid.empty());
    CHECK(result[1].material_type.empty());
    CHECK(result[1].material_preset.empty());
    CHECK(result[1].color.empty());
    CHECK(result[1].revision == 6);
}

TEST_CASE("An entirely empty HA material system remains available for display", "[HaMaterialSource]")
{
    auto data = snapshot();
    data["spools"] = nlohmann::json::array();
    data["slots"][0]["spool_uuid"] = nullptr;
    const auto result = parse(data.dump(), "duck-poop-demo");
    REQUIRE(result.size() == 1);
    CHECK(result[0].slot == "A1");
    CHECK(result[0].spool_uuid.empty());
}

TEST_CASE("Empty HA slots still require valid revisions", "[HaMaterialSource]")
{
    auto data = snapshot();
    data["slots"].push_back({{"id", "HT1"}, {"spool_uuid", nullptr}, {"revision", -1}});
    CHECK_THROWS(parse(data.dump(), "duck-poop-demo"));
}

TEST_CASE("The HA roll catalog hides empty and archived rolls but retains unmounted stock", "[HaMaterialSource][HaMaterialCatalog]")
{
    auto data = snapshot();
    data["revision"] = 4;
    auto &mounted = data["spools"][0];
    mounted["status"] = "active";
    mounted["remaining_mg"] = 42000;
    auto unmounted = mounted;
    unmounted["uuid"] = "0d1aa340-8c40-42a1-832f-87a6a7dcf94f";
    data["spools"].push_back(unmounted);
    auto archived = unmounted;
    archived["uuid"] = "2d1aa340-8c40-42a1-832f-87a6a7dcf94f";
    archived["status"] = "archived";
    data["spools"].push_back(archived);
    auto empty = unmounted;
    empty["uuid"] = "3d1aa340-8c40-42a1-832f-87a6a7dcf94f";
    empty["remaining_mg"] = 0;
    data["spools"].push_back(empty);
    auto lifecycle_empty = unmounted;
    lifecycle_empty["uuid"] = "4d1aa340-8c40-42a1-832f-87a6a7dcf94f";
    lifecycle_empty["status"] = "empty";
    data["spools"].push_back(lifecycle_empty);
    const auto rolls = Slic3r::HaMaterialCatalog::available(data);
    REQUIRE(rolls.size() == 2);
    CHECK(rolls[0].assignment.slot == "A1");
    CHECK(rolls[1].assignment.slot.empty());
    CHECK(rolls[0].assignment.spool_uuid != rolls[1].assignment.spool_uuid);
    CHECK(rolls[0].remaining_mg == 42000);
}

TEST_CASE("The HA roll catalog rejects invalid unmounted roll metadata", "[HaMaterialSource][HaMaterialCatalog]")
{
    auto data = snapshot();
    data["revision"] = 4;
    data["slots"][0]["spool_uuid"] = nullptr;
    data["spools"][0]["status"] = "active";
    data["spools"][0]["remaining_mg"] = 1000;
    data["spools"][0]["color"] = "invalid";
    CHECK_THROWS(Slic3r::HaMaterialCatalog::available(data));
}


TEST_CASE("HA catalog refresh cost is independent of unrelated native history", "[HaMaterialSource][HaMaterialCatalog][HaMaterialCatalogLatency]")
{
    // A normal 31-roll inventory with 247 historical jobs reproduced a GUI
    // stall when every unmounted roll copied and parsed the entire ledger.
    auto small = snapshot();
    small["revision"] = 12;
    const auto seed = small["spools"][0];
    small["spools"] = nlohmann::json::array();
    small["slots"] = nlohmann::json::array();
    for (int i = 0; i < 31; ++i) {
        auto roll = seed;
        const auto number = std::to_string(i);
        roll["uuid"] = std::string(8 - number.size(), '0') + number + "-8c40-42a1-832f-87a6a7dcf94f";
        roll["product"] = i == 30 ? "SainSmart Flex TPU Air" : "Inventory roll " + number;
        roll["status"] = "active";
        roll["remaining_mg"] = 990858;
        if (i == 30) { roll["material_type"] = "TPU"; roll["material_preset"] = ""; }
        small["spools"].push_back(std::move(roll));
    }
    const std::vector<std::string> slots {"A1", "A2", "A3", "A4", "HT1", "EXT"};
    for (size_t i = 0; i < slots.size(); ++i)
        small["slots"].push_back({{"id", slots[i]}, {"revision", 1},
            {"spool_uuid", i < 4 ? small["spools"][i]["uuid"] : nlohmann::json(nullptr)}});
    auto large = small;
    auto &tables = large["native_bundle"]["tables"];
    large["native_bundle"]["schema_version"] = 8;
    tables["print_jobs"] = nlohmann::json::array();
    tables["allocations"] = nlohmann::json::array();
    tables["stock_events"] = nlohmann::json::array();
    for (int i = 0; i < 247; ++i) {
        const auto id = "history-" + std::to_string(i);
        tables["print_jobs"].push_back({{"id", id}, {"state", "completed"}, {"job_name", "Historical print"},
            {"project_path", std::string(220, 'x')}, {"created_at", "2026-09-01T12:00:00Z"}});
        tables["allocations"].push_back({{"id", "allocation-" + id}, {"job_id", id},
            {"spool_id", small["spools"][0]["uuid"]}, {"estimated_weight_mg", 12000}, {"actual_weight_mg", 11000}});
        tables["stock_events"].push_back({{"id", "event-" + id}, {"job_id", id},
            {"spool_id", small["spools"][0]["uuid"]}, {"delta_mg", -11000}, {"note", std::string(200, 'n')}});
    }
    REQUIRE(large.dump().size() < 1024 * 1024);
    const auto rolls = Slic3r::HaMaterialCatalog::available(large);
    REQUIRE(rolls.size() == 31);
    CHECK(rolls.front().assignment.slot == "A1");
    const auto flex = std::find_if(rolls.begin(), rolls.end(), [](const auto &roll) {
        return roll.assignment.product == "SainSmart Flex TPU Air";
    });
    REQUIRE(flex != rolls.end());
    CHECK(flex->assignment.slot.empty());
    CHECK(flex->assignment.material_type == "TPU");
    CHECK(flex->assignment.material_preset.empty());
    CHECK(flex->remaining_mg == 990858);

    const auto elapsed = [](const nlohmann::json &data) {
        // Use the best of three batches to tolerate unrelated host activity.
        double best = 1e9;
        for (int batch = 0; batch < 3; ++batch) {
            const auto begin = std::chrono::steady_clock::now();
            for (int repeat = 0; repeat < 3; ++repeat) {
                const auto result = Slic3r::HaMaterialCatalog::available(data);
                REQUIRE(result.size() == 31);
            }
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
        }
        return best;
    };
    const auto small_seconds = elapsed(small);
    const auto large_seconds = elapsed(large);
    INFO("Small catalog seconds: " << small_seconds << "; with native history: " << large_seconds);
    // The generous ratio plus 10 ms noise allowance catches ledger-dependent
    // work without enforcing a machine-specific absolute performance budget.
    CHECK(large_seconds < small_seconds * 8 + 0.010);
}

TEST_CASE("Direct HA catalog validation preserves snapshot and unmounted roll checks", "[HaMaterialSource][HaMaterialCatalog]")
{
    auto data = snapshot();
    data["revision"] = 4;
    data["slots"][0]["spool_uuid"] = nullptr;
    data["spools"][0]["status"] = "active";
    data["spools"][0]["remaining_mg"] = 1000;
    SECTION("schema") { data["schema_version"] = 2; }
    SECTION("mode") { data["demo_mode"] = false; }
    SECTION("stale") { data["captured_unix"] = std::time(nullptr) - 600; }
    SECTION("future") { data["captured_unix"] = std::time(nullptr) + 120; }
    SECTION("empty UUID") { data["spools"][0]["uuid"] = ""; }
    SECTION("duplicate UUID") { data["spools"].push_back(data["spools"][0]); }
    SECTION("unknown slot") { data["slots"][0]["id"] = "A5"; }
    SECTION("missing mounted roll") { data["slots"][0]["spool_uuid"] = "missing"; }
    SECTION("negative unmounted revision") { data["revision"] = -1; }
    SECTION("empty type") { data["spools"][0]["material_type"] = ""; }
    SECTION("bad color") { data["spools"][0]["color"] = "#XXYYZZ"; }
    CHECK_THROWS(Slic3r::HaMaterialCatalog::available(data));
}

TEST_CASE("HA raw material input retains its size bound before parsing", "[HaMaterialSource]")
{
    auto data = snapshot();
    data["ignored_history"] = std::string(1024 * 1024, 'x');
    CHECK_THROWS(parse(data.dump(), "duck-poop-demo"));
}
