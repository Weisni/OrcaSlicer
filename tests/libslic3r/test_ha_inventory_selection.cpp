#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaInventorySelection.hpp"

using namespace Slic3r::HaInventorySelection;

static Json inventory()
{
    Json result={{"schema_version",8},{"tables",{{"spools",Json::array()},{"stock_events",Json::array()},
        {"customers",Json::array()},{"print_jobs",Json::array()},{"allocations",Json::array()}}}};
    for(int i=0;i<31;++i) {
        auto id="roll-"+std::to_string(i);
        result["tables"]["spools"].push_back({{"id",id},{"name",id},{"manufacturer","Vendor"},
            {"material_type","PLA"},{"filament_preset_id","Generic PLA"},{"status","active"}});
        result["tables"]["stock_events"].push_back({{"spool_id",id},{"delta_mg",1000000}});
    }
    return result;
}

TEST_CASE("Selected profile uploads leave other rolls and stock untouched", "[HaInventorySelection]")
{
    auto remote=inventory(), local=remote;
    local["tables"]["spools"][0]["filament_preset_id"]="Special PLA";
    local["tables"]["spools"][1]["filament_preset_id"]="Special PETG";
    local["tables"]["spools"][2]["name"]="Pending local edit";
    const std::vector<Selection> selected={{"roll-0",{"filament_preset_id"}},{"roll-1",{"filament_preset_id"}}};
    auto changes=upload_changes(local,remote,selected);
    REQUIRE(changes.size()==2);
    CHECK(changes[0]["expected"]["filament_preset_id"]=="Generic PLA");
    CHECK(changes[0]["fields"].size()==1);
    CHECK_FALSE(changes[0].contains("remaining_mg"));
    CHECK(upload_changes(local,remote,{}).empty());
}

TEST_CASE("Selected downloads preserve pending edits and all job history", "[HaInventorySelection]")
{
    auto local=inventory(), remote=local;
    local["tables"]["spools"][2]["name"]="Pending local edit";
    local["tables"]["customers"].push_back({{"id","customer"}});
    remote["tables"]["spools"][0]["filament_preset_id"]="Special PLA";
    auto merged=download_bundle(local,remote,{{"roll-0",{"filament_preset_id"}}},"request","2026-10-02T12:00:00Z");
    CHECK(merged["tables"]["spools"].size()==31);
    CHECK(merged["tables"]["spools"][0]["filament_preset_id"]=="Special PLA");
    CHECK(merged["tables"]["spools"][2]==local["tables"]["spools"][2]);
    CHECK(merged["tables"]["stock_events"]==local["tables"]["stock_events"]);
    CHECK(merged["tables"]["customers"]==local["tables"]["customers"]);
}

TEST_CASE("Selected stock downloads append a correction instead of replacing history", "[HaInventorySelection]")
{
    auto local=inventory(), remote=local;
    remote["tables"]["stock_events"][0]["delta_mg"]=700000;
    Selection selected{"roll-0",{},true};
    auto merged=download_bundle(local,remote,{selected},"request","2026-10-02T12:00:00Z");
    CHECK(balance(merged,"roll-0")==700000);
    CHECK(merged["tables"]["stock_events"].size()==32);
    CHECK(merged["tables"]["stock_events"][0]==local["tables"]["stock_events"][0]);
    CHECK(balance(merged,"roll-1")==1000000);
}

TEST_CASE("Selected uploads cannot delete by omission or transfer unknown fields", "[HaInventorySelection]")
{
    auto local=inventory(), remote=local;
    local["tables"]["spools"].erase(0);
    CHECK(upload_changes(local,remote,{}).empty());
    CHECK_THROWS(upload_changes(local,remote,{{"roll-0",{"name"}}}));
    CHECK_THROWS(upload_changes(local,remote,{{"roll-1",{"id"}}}));
    CHECK_THROWS(upload_changes(local,remote,{{"roll-1",{"name"}},{"roll-1",{"name"}}}));
}

TEST_CASE("Archive is an explicit status field and refuses open allocations", "[HaInventorySelection]")
{
    auto local=inventory(), remote=local;
    remote["tables"]["spools"][0]["status"]="archived";
    auto profile_only=download_bundle(local,remote,{{"roll-0",{"filament_preset_id"}}},"request","2026-10-02T12:00:00Z");
    CHECK(profile_only["tables"]["spools"][0]["status"]=="active");
    local["tables"]["print_jobs"].push_back({{"id","job"},{"state","printing"}});
    local["tables"]["allocations"].push_back({{"job_id","job"},{"spool_id","roll-0"}});
    CHECK_THROWS(download_bundle(local,remote,{{"roll-0",{"status"}}},"request","2026-10-02T12:00:00Z"));
}

TEST_CASE("Independent field imports reject contradictory stock lifecycle and capacity", "[HaInventorySelection]")
{
    auto local=inventory(), remote=local;
    remote["tables"]["spools"][0]["status"]="empty";
    remote["tables"]["stock_events"][0]["delta_mg"]=0;
    CHECK_THROWS(download_bundle(local,remote,{{"roll-0",{},true}},"request","2026-10-02T12:00:00Z"));
    CHECK_THROWS(download_bundle(local,remote,{{"roll-0",{"status"}}},"request","2026-10-02T12:00:00Z"));
    auto coupled=download_bundle(local,remote,{{"roll-0",{"status"},true}},"request","2026-10-02T12:00:00Z");
    CHECK(balance(coupled,"roll-0")==0);
    CHECK(coupled["tables"]["spools"][0]["status"]=="empty");
    remote=local;remote["tables"]["spools"][0]["nominal_capacity_mg"]=500000;
    CHECK_THROWS(download_bundle(local,remote,{{"roll-0",{"nominal_capacity_mg"}}},"request","2026-10-02T12:00:00Z"));
}

TEST_CASE("Acknowledging selected fields preserves other pending associations", "[HaInventorySelection]")
{
    auto local=inventory();
    local["tables"]["spools"][0]["filament_preset_id"]="New profile";
    Json state={{"acknowledged",{{"roll-1",{{"filament_preset_id","Previous profile"}}}}}};
    auto result=acknowledge(state,local,{{"roll-0",{"filament_preset_id"}}});
    CHECK(result["acknowledged"]["roll-0"]["filament_preset_id"]=="New profile");
    CHECK(result["acknowledged"]["roll-1"]==state["acknowledged"]["roll-1"]);
    CHECK_FALSE(result["acknowledged"]["roll-0"].contains("remaining_mg"));
}
