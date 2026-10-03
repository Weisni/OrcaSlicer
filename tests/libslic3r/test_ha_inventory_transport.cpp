#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaInventoryTransport.hpp"

using namespace Slic3r::HaInventoryTransport;

static Json transport_inventory()
{
    return {{"schema_version", 8}, {"tables", {
        {"inventory_settings", Json::array({{{"id", 1}, {"currency", "EUR"}}})},
        {"spools", Json::array({{{"id", "roll"}, {"name", "Original"}, {"color_hex", "#123456"}}})},
        {"spool_identifiers", Json::array()}, {"customers", Json::array()},
        {"customer_orders", Json::array()}, {"print_jobs", Json::array()},
        {"allocations", Json::array()}, {"job_identifiers", Json::array()},
        {"stock_events", Json::array()}, {"print_job_manual_overrides", Json::array()}}}};
}

static Json page(const Json &bundle, const std::string &table, size_t cursor, std::int64_t revision)
{
    const auto &all = bundle.at("tables").at(table);
    const size_t end = std::min(cursor + size_t(64), all.size());
    const Json next = end == all.size() ? Json(nullptr) : Json(end);
    return {{"schema_version", 8}, {"revision", revision}, {"table", table},
        {"rows", Json(all.begin() + cursor, all.begin() + end)}, {"total", all.size()}, {"next_cursor", next}};
}

TEST_CASE("Paged authority snapshots retain history larger than the former mirror limits", "[HaInventoryTransport]")
{
    auto expected = transport_inventory();
    for (size_t i = 0; i < 2200; ++i)
        expected["tables"]["stock_events"].push_back({{"id", "event-" + std::to_string(i)},
            {"spool_id", "roll"}, {"delta_mg", 1}, {"note", std::string(600, 'x')}});
    REQUIRE(expected.dump().size() > 1024 * 1024);
    const auto actual = read_bundle(17, [&](const std::string &table, size_t cursor, std::int64_t revision) {
        return page(expected, table, cursor, revision);
    });
    CHECK(actual == expected);
}

TEST_CASE("Paged authority snapshots reject changing revisions and broken pagination", "[HaInventoryTransport]")
{
    const auto source = transport_inventory();
    for (const auto *failure : {"revision", "schema_version", "table", "total", "next_cursor"}) {
        INFO(failure);
        CHECK_THROWS(read_bundle(17, [&](const std::string &table, size_t cursor, std::int64_t revision) {
            auto result = page(source, table, cursor, revision);
            if (std::string(failure) == "table") result[failure] = "unrelated";
            else result[failure] = 99;
            return result;
        }));
    }
    CHECK_THROWS(read_bundle(17, [&](const std::string &table, size_t cursor, std::int64_t revision) {
        auto result = page(source, table, cursor, revision);
        result["rows"].push_back(result["rows"][0]);
        result["total"] = result["rows"].size();
        return result;
    }));
}

TEST_CASE("Paged authority snapshots enforce a bounded resource budget", "[HaInventoryTransport]")
{
    const auto source = transport_inventory();
    CHECK_THROWS(read_bundle(17, [&](const std::string &table, size_t cursor, std::int64_t revision) {
        auto result = page(source, table, cursor, revision);
        result["total"] = 1000000000;
        return result;
    }));
    CHECK_THROWS(read_bundle(17, [&](const std::string &table, size_t cursor, std::int64_t revision) {
        auto result = page(source, table, cursor, revision);
        result["rows"][0]["note"] = std::string(1024 * 1024, 'x');
        return result;
    }));
}

TEST_CASE("Authority deltas carry only touched rows and their HA baselines", "[HaInventoryTransport]")
{
    auto remote = transport_inventory();
    for (int i = 0; i < 2200; ++i)
        remote["tables"]["stock_events"].push_back({{"id", std::to_string(i)}, {"note", std::string(600, 'x')}});
    auto before = remote, after = before;
    after["tables"]["spools"][0]["name"] = "Edited";
    after["tables"]["customers"].push_back({{"id", "customer"}, {"name", "New customer"}});
    const auto delta = changes(remote, before, after);
    REQUIRE(delta.size() == 2);
    CHECK(delta[0]["table"] == "spools");
    CHECK(delta[0]["before"]["name"] == "Original");
    CHECK(delta[0]["after"]["name"] == "Edited");
    CHECK(delta[1]["before"].is_null());
    CHECK(delta[1]["after"]["id"] == "customer");
    CHECK(delta.dump().size() < 1024);
    CHECK(remote["tables"]["spools"][0]["name"] == "Original");
}

TEST_CASE("Authority deltas preserve untouched server fields and ignore timestamp noise", "[HaInventoryTransport]")
{
    auto remote = transport_inventory(), before = remote, after = before;
    remote["tables"]["spools"][0]["color_hex"] = "#FFFFFF";
    after["tables"]["spools"][0]["name"] = "Edited";
    const auto delta = changes(remote, before, after);
    REQUIRE(delta.size() == 1);
    CHECK(delta[0]["before"]["color_hex"] == "#FFFFFF");
    CHECK(delta[0]["after"]["color_hex"] == "#FFFFFF");
    after = before;
    after["tables"]["spools"][0]["updated_at"] = "later";
    CHECK(changes(remote, before, after).empty());
}

TEST_CASE("Authority delta bytes leave room inside the server request envelope", "[HaInventoryTransport][Regression]")
{
    const auto before = transport_inventory();
    auto after = before;
    for (int i = 0; i < 300; ++i)
        after["tables"]["customers"].push_back({{"id",std::to_string(i)},{"name","Customer"},{"notes",std::string(3300,'x')}});
    CHECK_THROWS(changes(before,before,after));
    after["tables"]["customers"].erase(after["tables"]["customers"].begin()+200,after["tables"]["customers"].end());
    CHECK(changes(before,before,after).size() == 200);
}

TEST_CASE("Authority delta limits account for ASCII escaped server receipts", "[HaInventoryTransport][Regression]")
{
    const auto before = transport_inventory();
    auto after = before;
    std::string note;
    for (int i=0; i<1000; ++i) note += "\xC3\xA9";
    for (int i=0; i<200; ++i)
        after["tables"]["customers"].push_back({{"id",std::to_string(i)},{"notes",note}});
    REQUIRE(after.dump().size() < 990000);
    CHECK_THROWS(changes(before,before,after));
    after["tables"]["customers"].erase(after["tables"]["customers"].begin()+50,after["tables"]["customers"].end());
    CHECK(changes(before,before,after).size() == 50);
}

TEST_CASE("Authority deltas retain compound identities and reject ambiguous edits", "[HaInventoryTransport]")
{
    auto before = transport_inventory();
    before["tables"]["job_identifiers"].push_back({{"provider", "printer"}, {"kind", "job"}, {"value", "one"}, {"job_id", "first"}});
    auto after = before;
    after["tables"]["job_identifiers"][0]["job_id"] = "second";
    const auto delta = changes(before, before, after);
    REQUIRE(delta.size() == 1);
    CHECK(delta[0]["before"]["job_id"] == "first");
    CHECK(delta[0]["after"]["job_id"] == "second");
    after["tables"]["job_identifiers"].push_back(after["tables"]["job_identifiers"][0]);
    CHECK_THROWS(changes(before, before, after));
    after = before;
    after["tables"]["spools"].clear();
    const auto removed = changes(before, before, after);
    REQUIRE(removed.size() == 1);
    CHECK(removed[0]["after"].is_null());
    after = before;
    for (size_t i = 0; i < 1001; ++i) after["tables"]["customers"].push_back({{"id", std::to_string(i)}});
    CHECK_THROWS(changes(before, before, after));
}

TEST_CASE("Paged reads restart the whole snapshot after a conflicting revision and stop after repeated conflicts", "[HaInventoryTransport]")
{
    const auto source = transport_inventory();
    int calls = 0;
    const auto metadata = [&] { return Json{{"revision", 17 + calls++}}; };
    const auto snapshot = read_snapshot(metadata, [&](const std::string &table, size_t cursor, std::int64_t revision) {
        if (revision == 17 && table == "spools") throw RevisionChanged();
        return page(source, table, cursor, revision);
    });
    REQUIRE(snapshot.contains("native_bundle"));
    CHECK(snapshot["revision"] == 18);
    CHECK(snapshot["native_bundle"] == source);
    CHECK(calls == 2);
    calls = 0;
    CHECK_THROWS_AS(read_snapshot(metadata, [&](const std::string &, size_t, std::int64_t) -> Json {
        throw RevisionChanged();
    }), RevisionChanged);
    CHECK(calls == 3);
}
