#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaMaterialRefresh.hpp"
#include "libslic3r/HaMaterialAsyncRefresh.hpp"
#include <future>
#include <thread>

using namespace Slic3r;
using Json = nlohmann::json;
namespace {
Json material_snapshot()
{
    return {{"printer_id", "test-printer"}, {"provider_printer_id", "test-device"}, {"revision", 1},
        {"slots", Json::array({{{"id", "A1"}, {"spool_uuid", "roll"}, {"revision", 1}}})},
        {"spools", Json::array({{{"uuid", "roll"}, {"manufacturer", "Maker"}, {"product", "TPU"},
            {"material_type", "TPU"}, {"color", "#FFFFFF"}, {"material_preset", "Generic TPU"},
            {"status", "active"}, {"remaining_mg", 990000}}})},
        {"native_bundle", {{"tables", {{"stock_events", Json::array()}}}}}};
}
}

TEST_CASE("Unchanged telemetry and unrelated history do not rebuild material lists", "[HaMaterialRefresh]")
{
    HaMaterialRefresh::DisplayRevision state;
    auto snapshot = material_snapshot();
    REQUIRE(state.update("source", snapshot, ""));
    const auto rendered = state.generation();
    REQUIRE(HaMaterialRefresh::should_rebuild(rendered, 0, false));
    for (int i = 0; i < 100; ++i) {
        snapshot["revision"] = i + 2;
        snapshot["slots"][0]["revision"] = i + 2;
        snapshot["native_bundle"]["tables"]["stock_events"].push_back({{"unrelated_event", i}});
        REQUIRE_FALSE(state.update("source", snapshot, ""));
        REQUIRE_FALSE(HaMaterialRefresh::should_rebuild(state.generation(), rendered, false));
    }
}

TEST_CASE("Material changes and source availability refresh the displayed rolls", "[HaMaterialRefresh]")
{
    HaMaterialRefresh::DisplayRevision state;
    auto snapshot = material_snapshot();
    REQUIRE(state.update("source", snapshot, ""));
    const auto original = state.generation();
    snapshot["spools"][0]["remaining_mg"] = 980000;
    REQUIRE(state.update("source", snapshot, ""));
    REQUIRE(state.generation() > original);
    snapshot["spools"][0]["material_preset"] = "Flex TPU Air";
    REQUIRE(state.update("source", snapshot, ""));
    snapshot["slots"][0]["spool_uuid"] = "";
    REQUIRE(state.update("source", snapshot, ""));
    REQUIRE(state.update("source", snapshot, "unavailable"));
    REQUIRE_FALSE(state.update("source", snapshot, "unavailable"));
    REQUIRE(state.update("source", snapshot, ""));
    REQUIRE(state.update("other-source", snapshot, ""));
}

TEST_CASE("Open material pickers defer changes until a later refresh", "[HaMaterialRefresh]")
{
    REQUIRE_FALSE(HaMaterialRefresh::should_rebuild(2, 1, true));
    REQUIRE(HaMaterialRefresh::should_rebuild(2, 1, false));
    REQUIRE_FALSE(HaMaterialRefresh::should_rebuild(2, 2, false));
}

TEST_CASE("A changed profile digest refreshes the list even when its label stays unchanged", "[HaMaterialRefresh]")
{
    HaMaterialRefresh::DisplayRevision state;
    auto snapshot = material_snapshot();
    snapshot["spools"][0]["material_profile"] = {{"sha256", "first"}};
    REQUIRE(state.update("source", snapshot, ""));
    snapshot["spools"][0]["material_profile"]["sha256"] = "second";
    CHECK(state.update("source", snapshot, ""));
}

TEST_CASE("Background refresh is single flight and discards invalidated completions", "[HaMaterialRefresh]")
{
    HaMaterialRefresh::AsyncRefresh worker;
    std::promise<void> gate;
    auto released = gate.get_future().share();
    const bool started = worker.start("source", [released](const auto&) { released.wait(); return Json{{"revision",1}}; });
    CHECK(started);
    CHECK_FALSE(worker.start("source", [](const auto&) { return Json(); }));
    worker.invalidate();
    gate.set_value();
    if (started) while (!worker.ready()) std::this_thread::yield();
    CHECK_FALSE(worker.take("source").has_value());
    const bool next = worker.start("source", [](const auto&) { return Json{{"revision",2}}; });
    CHECK(next);
    if (next) while (!worker.ready()) std::this_thread::yield();
    const auto result = worker.take("source");
    REQUIRE(result.has_value());
    CHECK(result->data.at("revision") == 2);
}

TEST_CASE("Background results cannot publish to another endpoint", "[HaMaterialRefresh]")
{
    HaMaterialRefresh::AsyncRefresh worker;
    const bool started = worker.start("old", [](const auto&) { return Json{{"revision",1}}; });
    REQUIRE(started);
    while (!worker.ready()) std::this_thread::yield();
    CHECK_FALSE(worker.take("new").has_value());
    worker.shutdown();
    CHECK_FALSE(worker.start("new", [](const auto&) { return Json(); }));
}
