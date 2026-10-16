#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaJobPreheat.hpp"

using namespace Slic3r::HaJobPreheat;
namespace {
Json parameters() { return {{"enabled",true},{"chamber_target",55},{"print_bed_target",60},{"preheat_bed_target",110}}; }
Json ready() { return {{"schema_version",1},{"instance_id","instance-a"},{"job_uuid","job-a"},{"printer_id","printer-a"},{"version",2},
    {"phase","ready"},{"reason","target_reached"},{"ready",true},{"expires_at",2000},{"parameters",parameters()}}; }
Session session() { return {"http://ha/api/quack_materials/materials","job-a","printer-a","instance-a",parameters()}; }
}
TEST_CASE("Only capable provider jobs opt into server preheating", "[HaJobPreheat]")
{
    Json snapshot={{"capabilities",{{"job_preheat_v1",true}}}};
    CHECK(use_server(true,snapshot));
    CHECK_FALSE(use_server(false,snapshot));
    CHECK_FALSE(use_server(true,Json::object()));
    CHECK_FALSE(use_server(true,{{"capabilities",{{"job_preheat_v1",false}}}}));
}
TEST_CASE("Request identity binds all preheat commands to one job and instance", "[HaJobPreheat]")
{
    auto s=session();
    CHECK(s.url() == "http://ha/api/quack_materials/preheat/job-a");
    CHECK(s.url("request") == "http://ha/api/quack_materials/preheat/job-a/request");
    const auto request=s.request();
    CHECK(request.at("printer_id") == "printer-a");
    CHECK(request.at("instance_id") == "instance-a");
    CHECK(request.at("parameters") == parameters());
    CHECK(s.request() == request);
    CHECK_THROWS(s.url("claim"));
}
TEST_CASE("Readiness rejects wrong job printer parameters and expired responses", "[HaJobPreheat]")
{
    auto s=session();
    auto response=ready();
    CHECK(s.accept(response,1000));
    CHECK(s.claim(1000) == Json{{"instance_id","instance-a"},{"version",2}});
    CHECK_THROWS(s.claim(2000));
    for (const auto& field : {"job_uuid","printer_id"}) {
        auto invalid=response; invalid[field]="other";
        CHECK_THROWS(s.accept(invalid,1000));
    }
    response["parameters"]["chamber_target"]=56;
    CHECK_THROWS(s.accept(response,1000));
    response=ready(); response["expires_at"]=1000;
    CHECK_THROWS(s.accept(response,1000));
}
TEST_CASE("A ready flag cannot bypass the ready phase or a newer version", "[HaJobPreheat]")
{
    auto s=session(); auto response=ready();
    REQUIRE(s.accept(response,1000));
    response["version"]=1;
    CHECK_THROWS(s.accept(response,1000));
    response=ready(); response["phase"]="warming";
    CHECK_THROWS(s.accept(response,1000));
    response=ready(); response["phase"]="interrupted"; response["ready"]=false;
    CHECK_THROWS(s.accept(response,1000));
}
TEST_CASE("Cancel and skip use server commands with the latest version", "[HaJobPreheat]")
{
    auto s=session(); REQUIRE(s.accept(ready(),1000));
    for (const auto& command : {"cancel","skip","retry","extend"}) {
        CHECK(s.url(command) == std::string("http://ha/api/quack_materials/preheat/job-a/")+command);
        const auto payload=s.command(command);
        CHECK(payload.at("version")==2);
        CHECK(payload.at("instance_id")=="instance-a");
        CHECK(payload==s.command(command));
        CHECK(payload.at("request_key")!=s.request().at("request_key"));
    }
}
TEST_CASE("A server claim permits at most one physical dispatch attempt", "[HaJobPreheat]")
{
    DispatchGuard guard;
    guard.begin_claim();
    CHECK(guard.uncertain());
    CHECK_THROWS(guard.begin_claim());
    guard.confirm_claim();
    guard.begin_dispatch();
    CHECK_THROWS(guard.begin_dispatch());
    CHECK_THROWS(guard.begin_claim());
    CHECK_FALSE(guard.uncertain());
}

TEST_CASE("Responses require the requesting instance and invalidate earlier readiness", "[HaJobPreheat]")
{
    auto s=session(); REQUIRE(s.accept(ready(),1000));
    auto response=ready(); response["instance_id"]="another-process";
    CHECK_THROWS(s.accept(response,1000));
    CHECK_THROWS(s.claim(1000));
    response=ready(); response.erase("instance_id");
    CHECK_THROWS(s.accept(response,1000));
}
TEST_CASE("Disabled preheating still requests admission and a dispatch claim", "[HaJobPreheat]")
{
    auto config=parameters(); config["enabled"]=false;
    Session s("http://ha/api/quack_materials/materials","job-a","printer-a","instance-a",config);
    CHECK_FALSE(s.request().at("parameters").at("enabled").get<bool>());
    auto response=ready(); response["parameters"]=config; response["reason"]="not_required";
    REQUIRE(s.accept(response,1000));
    CHECK(s.claim(1000).at("version")==2);
}
TEST_CASE("Unknown preheat phases cannot keep a print waiting indefinitely", "[HaJobPreheat]")
{
    auto s=session(); auto response=ready(); response["ready"]=false; response["phase"]="unknown";
    CHECK_THROWS(s.accept(response,1000));
}

TEST_CASE("A definitive claim rejection never authorizes a physical attempt", "[HaJobPreheat]")
{
    DispatchGuard guard;
    guard.begin_claim(); guard.reject_claim();
    CHECK_FALSE(guard.uncertain());
    CHECK_THROWS(guard.confirm_claim());
    CHECK_THROWS(guard.begin_dispatch());
    CHECK_THROWS(guard.begin_claim());
}
TEST_CASE("Every supported waiting phase withholds the dispatch claim", "[HaJobPreheat]")
{
    for (const auto& phase : {"selecting_air","heating","conditioning_bed"}) {
        auto s=session(); auto response=ready(); response["phase"]=phase; response["ready"]=false;
        CHECK_FALSE(s.accept(response,1000));
        CHECK_THROWS(s.claim(1000));
        CHECK(s.command("cancel").at("version")==2);
    }
}

TEST_CASE("Unix expiry timestamps accept fractional seconds from Home Assistant", "[HaJobPreheat]")
{
    auto s=session(); auto response=ready(); response["expires_at"]=2000.5;
    REQUIRE(s.accept(response,1000));
    CHECK(s.claim(2000).at("version")==2);
    CHECK_THROWS(s.claim(2001));
}

TEST_CASE("Terminal HA preheat states report review without a dispatch claim", "[HaJobPreheat]")
{
    for (const auto& phase : {"cancelled","expired","manual_takeover","handoff","failed","needs_review"}) {
        auto s=session(); auto response=ready(); response["phase"]=phase; response["ready"]=false;
        CHECK_THROWS_AS(s.accept(response,1000), StatusError);
        CHECK(s.terminal());
        CHECK_THROWS(s.claim(1000));
    }
}
