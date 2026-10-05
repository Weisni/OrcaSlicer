#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaMaterialBinding.hpp"
#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;
using namespace Slic3r::HaMaterialBinding;
static const std::string roll_a = "11111111-1111-4111-8111-111111111111";
static const std::string roll_b = "22222222-2222-4222-8222-222222222222";
static const std::string source = "http://homeassistant.local:8123/api/quack_material_demo/materials";
static nlohmann::json binding_snapshot()
{
    return {{"schema_version",1},{"demo_mode",true},{"printer_id","duck-poop-demo"},{"captured_unix",std::time(nullptr)},
        {"spools", {{{"uuid",roll_a},{"manufacturer","Vendor"},{"product","PLA"},{"material_type","PLA"},
            {"color","#112233"},{"material_preset","Special PLA"},{"status","active"},{"remaining_mg",10000},{"available_mg",10000}}}},
        {"slots", {{{"id","HT1"},{"spool_uuid",roll_a},{"revision",1}}}}};
}
static std::vector<LiveSlot> live_slots() { return {{"HT1","128","0",128,true,"PLA","112233FF"}}; }
static std::vector<Binding> bindings() { return {{roll_a,source,"duck-poop-demo"}}; }

TEST_CASE("Project roll bindings survive local material overrides and explicit index changes", "[HaMaterialBinding]")
{
    DynamicPrintConfig config;
    set(config,0,{roll_a,source,"duck-poop-demo"},2);
    set(config,1,{roll_b,source,"duck-poop-demo"},2);
    config.set_key_value("filament_settings_id",new ConfigOptionStrings{"Local override","Other"});
    REQUIRE(read(config,2)[0].spool_uuid == roll_a);
    resize(config,3);
    CHECK(read(config,3)[0].spool_uuid == roll_a);
    CHECK(read(config,3)[2].spool_uuid.empty());
    remap(config,{1,0,2},3);
    CHECK(read(config,3)[0].spool_uuid == roll_b);
    erase(config,0,3);
    CHECK(read(config,2)[0].spool_uuid == roll_a);
    CHECK(read(config,2)[1].spool_uuid.empty());
}

TEST_CASE("HA print preflight resolves UUID to the actual current HT slot", "[HaMaterialBinding]")
{
    const auto result = preflight(bindings(),{{0,"PLA",1200}},binding_snapshot(),source,"P2S","P2S",live_slots());
    REQUIRE(result.size()==1);
    CHECK(result[0].spool_uuid==roll_a);
    CHECK(result[0].slot=="HT1");
    CHECK(result[0].tray_id==128);
    CHECK(result[0].estimated_mg==1200);
}

TEST_CASE("ASA plus physical rolls pass preflight with ASA slicing and ASA printer family", "[HaMaterialBinding]")
{
    auto snapshot = binding_snapshot();
    snapshot["spools"][0]["material_type"] = "ASA+";
    auto live = live_slots();
    live[0].material_type = "ASA";
    const auto result = preflight(bindings(), {{0,"ASA",1200}}, snapshot, source, "P2S", "P2S", live);
    REQUIRE(result.size() == 1);
    CHECK(result[0].spool_uuid == roll_a);
    CHECK(snapshot["spools"][0]["material_type"] == "ASA+");
    CHECK_THROWS(preflight(bindings(), {{0,"ABS",1200}}, snapshot, source, "P2S", "P2S", live));
}

TEST_CASE("Profile baselines round trip alongside legacy physical bindings", "[HaMaterialBinding]")
{
    DynamicPrintConfig config;
    const Binding baseline{roll_a, source, "duck-poop-demo", "Bambu Lab P2S|0.8|high_flow", "ASA profile", std::string(64, 'a')};
    write(config, {baseline, {roll_b, source, "duck-poop-demo"}});
    const auto restored = read(config, 2);
    CHECK(restored[0].profile_context == baseline.profile_context);
    CHECK(restored[0].profile_sha256 == baseline.profile_sha256);
    CHECK(restored[1].profile_context.empty());
    CHECK(restored[1].spool_uuid == roll_b);
    remap(config, {1, 0}, 2);
    CHECK(read(config, 2)[1].profile_name == baseline.profile_name);
}

TEST_CASE("Nozzle changes preserve unsynchronized profile choices and settings", "[HaMaterialBinding]")
{
    PresetBundle bundle;
    auto original = bundle.filaments.get_edited_preset();
    original.name = "ASA standard";
    original.config.set_key_value("filament_type", new ConfigOptionStrings{"ASA"});
    const Binding baseline{roll_a, source, "duck-poop-demo", "Bambu Lab P2S|0.4|standard", original.name,
        HaMaterialProfile::digest(HaMaterialProfile::capture(original))};
    CHECK_FALSE(preserves_override(baseline, &original));
    auto changed = original;
    changed.name = "Local ASA experiment";
    CHECK(preserves_override(baseline, &changed));
    changed = original;
    changed.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.91});
    CHECK(preserves_override(baseline, &changed));
    CHECK(preserves_override({roll_a, source, "duck-poop-demo"}, &original));
    CHECK(preserves_override(baseline, nullptr));
}

TEST_CASE("Native printer compatibility changes preserve HA bound local overrides and truthful incompatibility", "[HaMaterialBinding][Regression]")
{
    for (const bool dirty : {false, true}) {
        PresetBundle bundle;
        auto printer_config = bundle.printers.default_preset().config;
        printer_config.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P2S"));
        printer_config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});
        bundle.printers.load_preset("", "P2S 0.4 nozzle", printer_config, true);
        printer_config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.8});
        bundle.printers.load_preset("", "P2S 0.8 nozzle", printer_config, false);
        auto filament_config = bundle.filaments.default_preset().config;
        filament_config.set_key_value("filament_type", new ConfigOptionStrings{"ASA"});
        filament_config.set_key_value("compatible_printers", new ConfigOptionStrings{"P2S 0.8 nozzle"});
        bundle.filaments.load_preset("", "Generic ASA 0.8", filament_config, false);
        filament_config.set_key_value("compatible_printers", new ConfigOptionStrings{"P2S 0.4 nozzle"});
        bundle.filaments.load_preset("", "Local ASA experiment 0.4", filament_config, true);
        bundle.filament_presets = {"Local ASA experiment 0.4"};
        const auto baseline = HaMaterialProfile::digest(HaMaterialProfile::capture(bundle.filaments.get_edited_preset()));
        write(bundle.project_config, {{roll_a, source, "duck-poop-demo", "Bambu Lab P2S|0.4|standard",
            dirty ? "Local ASA experiment 0.4" : "Central ASA 0.4", baseline}});
        if (dirty) bundle.filaments.get_edited_preset().config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.91});
        const auto original_config = bundle.filaments.get_edited_preset().config;
        bundle.update_compatible(PresetSelectCompatibleType::Never);
        REQUIRE(bundle.filaments.get_edited_preset().is_compatible);
        bundle.printers.select_preset_by_name("P2S 0.8 nozzle", true);
        bundle.update_compatible(PresetSelectCompatibleType::Always);
        CHECK(bundle.filament_presets[0] == "Local ASA experiment 0.4");
        CHECK(bundle.filaments.get_edited_preset().name == "Local ASA experiment 0.4");
        CHECK(bundle.filaments.get_edited_preset().config == original_config);
        CHECK_FALSE(bundle.filaments.get_edited_preset().is_compatible);
        CHECK_FALSE(bundle.filaments.find_preset("Local ASA experiment 0.4")->is_compatible);
        CHECK(bundle.filaments.find_preset("Generic ASA 0.8")->is_compatible);
        CHECK(read(bundle.project_config, 1)[0].spool_uuid == roll_a);
    }
}

TEST_CASE("Print profile checks allow compatible local ASA overrides and reject wrong nozzles and changed contexts", "[HaMaterialBinding][Regression]")
{
    PresetBundle bundle;
    auto printer = bundle.printers.get_edited_preset();
    printer.name = "P2S 0.8 nozzle";
    printer.config.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P2S"));
    printer.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.8});
    const auto active = HaMaterialContext::make("Bambu Lab P2S", 0.8, "high_flow");
    const auto prepared = HaMaterialContext::key(active);
    auto local_config = bundle.filaments.default_preset().config;
    local_config.set_key_value("filament_type", new ConfigOptionStrings{"ASA"});
    local_config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.91});
    local_config.set_key_value("compatible_printers", new ConfigOptionStrings{"P2S 0.8 nozzle"});
    // Load a real user preset. A renamed copy of the default sentinel retains
    // is_default=true and deliberately bypasses native compatibility restrictions.
    auto &local = bundle.filaments.load_preset("", "Local ASA settings", local_config, true);
    REQUIRE_FALSE(local.is_default);
    CHECK_NOTHROW(HaMaterialSource::validate_print_profile({local, nullptr}, {printer, nullptr}, "ASA+", prepared, active));
    CHECK_THROWS(HaMaterialSource::validate_print_profile({local, nullptr}, {printer, nullptr}, "ABS", prepared, active));
    CHECK_THROWS(HaMaterialSource::validate_print_profile({local, nullptr}, {printer, nullptr}, "ASA", prepared,
        HaMaterialContext::make("Bambu Lab P2S", 0.8, "standard")));
    local.config.set_key_value("compatible_printers", new ConfigOptionStrings{"P2S 0.4 nozzle"});
    CHECK_THROWS(HaMaterialSource::validate_print_profile({local, nullptr}, {printer, nullptr}, "ASA", prepared, active));
}

TEST_CASE("HA print preflight rejects another printer or source", "[HaMaterialBinding]")
{
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),source,"P2S","Other",live_slots()));
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),"other source","P2S","P2S",live_slots()));
}

TEST_CASE("HA print preflight rejects stale empty changed or unmounted inventory", "[HaMaterialBinding]")
{
    auto snapshot=binding_snapshot();
    snapshot["captured_unix"]=std::time(nullptr)-600;
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},snapshot,source,"P2S","P2S",live_slots()));
    snapshot=binding_snapshot(); snapshot["spools"][0]["status"]="archived";
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},snapshot,source,"P2S","P2S",live_slots()));
    snapshot=binding_snapshot(); snapshot["slots"][0]["spool_uuid"]=nullptr;
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},snapshot,source,"P2S","P2S",live_slots()));
    auto slots=live_slots();slots[0].present=false;
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),source,"P2S","P2S",slots));
    slots=live_slots();slots[0].material_type="PETG";
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),source,"P2S","P2S",slots));
    CHECK_THROWS(preflight(bindings(),{{0,"PETG",1}},binding_snapshot(),source,"P2S","P2S",live_slots()));
}

TEST_CASE("HA preflight aggregates stock across repeated project uses of one physical roll", "[HaMaterialBinding]")
{
    auto bound=bindings();bound.push_back(bound[0]);
    CHECK_THROWS(preflight(bound,{{0,"PLA",6000},{1,"PLA",6000}},binding_snapshot(),source,"P2S","P2S",live_slots()));
    CHECK(preflight(bound,{{0,"PLA",3000},{1,"PLA",4000}},binding_snapshot(),source,"P2S","P2S",live_slots()).size()==2);
}

TEST_CASE("Preset bundle preserves roll identity through count changes and project config serialization", "[HaMaterialBinding]")
{
    PresetBundle bundle;
    bundle.filament_presets = {bundle.filaments.get_selected_preset_name()};
    set(bundle.project_config,0,{roll_a,source,"duck-poop-demo"},1);
    bundle.set_num_filaments(2,std::string("#112233"));
    REQUIRE(bundle.project_config.option<ConfigOptionStrings>(config_key)->values.size()==2);
    CHECK(read(bundle.project_config,2)[0].spool_uuid==roll_a);
    CHECK(read(bundle.project_config,2)[1].spool_uuid.empty());
    set(bundle.project_config,1,{roll_b,source,"duck-poop-demo"},2);
    bundle.update_num_filaments(0);
    CHECK(read(bundle.project_config,1)[0].spool_uuid==roll_b);
    DynamicPrintConfig restored;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    REQUIRE_NOTHROW(restored.set_deserialize(config_key,bundle.project_config.option(config_key)->serialize(),substitutions));
    CHECK(read(restored,1)[0].spool_uuid==roll_b);
}




TEST_CASE("Final HA preflight credits only its own still-reserved allocation", "[HaMaterialBinding]")
{
    auto snapshot=binding_snapshot();
    snapshot["spools"][0]["available_mg"]=1000;
    snapshot["jobs"]=nlohmann::json::array({{{"uuid",roll_b},{"state","reserved"},{"settlement",nullptr},
        {"allocations",nlohmann::json::array({{{"spool_uuid",roll_a},{"weight_mg",9000}}})}}});
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",9000}},snapshot,source,"P2S","P2S",live_slots()));
    CHECK(preflight(bindings(),{{0,"PLA",9000}},snapshot,source,"P2S","P2S",live_slots(),roll_b).size()==1);
    snapshot["jobs"][0]["settlement"]={{"outcome","completed"}};
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",9000}},snapshot,source,"P2S","P2S",live_slots(),roll_b));
}

TEST_CASE("HA preflight maps the assigned roll despite the printer color approximation", "[HaMaterialBinding][Regression]")
{
    auto snapshot = binding_snapshot();
    snapshot["spools"][0]["color"] = "#FFFFFF";
    snapshot["slots"][0]["id"] = "A1";
    const auto before = snapshot;
    const std::vector<LiveSlot> slots {{"A1","0","0",0,true,"PLA","C1C1C1FF"}};

    const auto result = preflight(bindings(),{{0,"PLA",1200}},snapshot,source,"P2S","P2S",slots);

    REQUIRE(result.size() == 1);
    CHECK(result[0].spool_uuid == roll_a);
    CHECK(result[0].slot == "A1");
    CHECK(result[0].tray_id == 0);
    CHECK(result[0].estimated_mg == 1200);
    CHECK(result[0].color == "#FFFFFF");
    CHECK(snapshot == before);
}

TEST_CASE("Paged authoritative jobs authorize only their own reservation without history size limits", "[HaMaterialBinding][Regression]")
{
    auto snapshot = binding_snapshot();
    snapshot["spools"][0]["available_mg"] = 1000;
    snapshot["native_bundle"] = {{"schema_version",8},{"tables", {
        {"print_jobs", nlohmann::json::array({{{"id",roll_b},{"state","reserved"},{"printer_id","P2S"}}})},
        {"allocations", nlohmann::json::array({{{"job_id",roll_b},{"spool_id",roll_a},{"estimated_weight_mg",9000}}})},
        {"stock_events",nlohmann::json::array({{{"note",std::string(1100000,'x')}}})}}}};
    REQUIRE(preflight(bindings(),{{0,"PLA",9000}},snapshot,source,"P2S","P2S",live_slots(),roll_b).size() == 1);
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",9000}},snapshot,source,"P2S","P2S",live_slots(),roll_a));
    snapshot["native_bundle"]["tables"]["print_jobs"][0]["state"] = "printing";
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",9000}},snapshot,source,"P2S","P2S",live_slots(),roll_b));
    snapshot["native_bundle"]["tables"]["print_jobs"][0]["state"] = "reserved";
    snapshot["native_bundle"]["tables"]["print_jobs"][0]["printer_id"] = "another printer";
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",9000}},snapshot,source,"P2S","P2S",live_slots(),roll_b));
    snapshot["native_bundle"]["tables"]["print_jobs"][0]["printer_id"] = "P2S";
    snapshot["native_bundle"]["tables"]["allocations"][0]["estimated_weight_mg"] = 200;
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",9000}},snapshot,source,"P2S","P2S",live_slots(),roll_b));
}

TEST_CASE("Printer color tolerance retains HA identity and material safety checks", "[HaMaterialBinding][Regression]")
{
    auto slots = live_slots();
    slots[0].color = "C1C1C1FF";
    CHECK_NOTHROW(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),source,"P2S","P2S",slots));

    slots[0].material_type = "PETG";
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),source,"P2S","P2S",slots));
    slots[0].material_type = "PLA";
    CHECK_THROWS(preflight(bindings(),{{0,"PETG",1}},binding_snapshot(),source,"P2S","P2S",slots));
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),source,"P2S","Other",slots));
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),"other source","P2S","P2S",slots));
    slots[0].present = false;
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},binding_snapshot(),source,"P2S","P2S",slots));
    slots[0].present = true;
    auto snapshot = binding_snapshot();
    snapshot["slots"][0]["spool_uuid"] = nullptr;
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},snapshot,source,"P2S","P2S",slots));
    snapshot = binding_snapshot(); snapshot["spools"][0]["status"] = "archived";
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},snapshot,source,"P2S","P2S",slots));
    snapshot = binding_snapshot(); snapshot["spools"][0]["remaining_mg"] = 0;
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},snapshot,source,"P2S","P2S",slots));
    snapshot = binding_snapshot(); snapshot["captured_unix"] = std::time(nullptr) - 600;
    CHECK_THROWS(preflight(bindings(),{{0,"PLA",1}},snapshot,source,"P2S","P2S",slots));
}

TEST_CASE("HA preflight rejects malformed saved identity", "[HaMaterialBinding]")
{
    DynamicPrintConfig config;
    config.set_key_value(config_key,new ConfigOptionStrings{"{invalid"});
    CHECK_THROWS(read(config,1));
    CHECK_THROWS(set(config,0,{"fake",source,"duck-poop-demo"},1));
}
