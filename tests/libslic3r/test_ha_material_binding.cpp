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
