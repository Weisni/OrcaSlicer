#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>
#include <filesystem>
#include <chrono>
#include "libslic3r/FilamentInventory.hpp"
#include "nlohmann/json.hpp"

using namespace Slic3r::FilamentInventory;
using Json = nlohmann::json;

TEST_CASE("Authoritative imports write only changed rows and roll back invalid graphs", "[HaInventoryImport]")
{
    const auto path = std::filesystem::temp_directory_path() / ("ha-incremental-import-" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + ".sqlite3");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path,error); } } cleanup{path};
    Store store(path.string());
    SpoolInput input;
    input.name = "Initial"; input.manufacturer = "Test"; input.material_type = "PLA"; input.color_hex = "#123456";
    input.nominal_capacity_mg = 1000000; input.initial_weight_mg = 1000000;
    store.create_spool(input);
    const auto initial = Json::parse(store.export_ha_demo_bundle());
    sqlite3 *raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    struct Close { sqlite3 *db; ~Close() { sqlite3_close(db); } } close{raw};
    REQUIRE(sqlite3_exec(raw,"CREATE TABLE mutation_audit(kind TEXT);"
        "CREATE TRIGGER audit_spool_update AFTER UPDATE ON spools BEGIN INSERT INTO mutation_audit VALUES ('update'); END;"
        "CREATE TRIGGER audit_spool_delete AFTER DELETE ON spools BEGIN INSERT INTO mutation_audit VALUES ('delete'); END;"
        "CREATE TRIGGER audit_stock_insert AFTER INSERT ON stock_events BEGIN INSERT INTO mutation_audit VALUES ('stock'); END;",
        nullptr,nullptr,nullptr) == SQLITE_OK);
    const auto ack = [](const Json &before) { return Json{{"explicit_sync",true},{"endpoint","test"},{"revision",1},{"_expected_local",before}}.dump(); };
    auto updated = initial; updated["tables"]["spools"][0]["name"] = "Changed";
    REQUIRE_NOTHROW(store.import_authoritative_ha_bundle(updated.dump(),ack(initial)));
    REQUIRE_NOTHROW(store.import_authoritative_ha_bundle(updated.dump(),ack(updated)));
    sqlite3_stmt *query = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw,"SELECT kind,COUNT(*) FROM mutation_audit GROUP BY kind",-1,&query,nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(query) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char *>(sqlite3_column_text(query,0))) == "update");
    CHECK(sqlite3_column_int(query,1) == 1);
    CHECK(sqlite3_step(query) == SQLITE_DONE);
    sqlite3_finalize(query);
    auto invalid = updated; invalid["tables"]["spools"].clear();
    CHECK_THROWS(store.import_authoritative_ha_bundle(invalid.dump(),ack(updated)));
    CHECK(Json::parse(store.export_ha_demo_bundle()) == updated);
    auto duplicate = updated; duplicate["tables"]["spools"].push_back(duplicate["tables"]["spools"][0]);
    CHECK_THROWS(store.import_authoritative_ha_bundle(duplicate.dump(),ack(updated)));
    CHECK(Json::parse(store.export_ha_demo_bundle()) == updated);
    CHECK_THROWS(store.import_authoritative_ha_bundle(initial.dump(),ack(initial)));
    auto malformed = updated; malformed["tables"]["spools"][0]["name"] = std::string("bad\0name",8);
    CHECK_THROWS(store.import_authoritative_ha_bundle(malformed.dump(),ack(updated)));
    CHECK(Json::parse(store.export_ha_demo_bundle()) == updated);
    auto removed = updated; removed["tables"]["stock_events"].clear(); removed["tables"]["spools"].clear();
    CHECK_THROWS(store.import_authoritative_ha_bundle(removed.dump(),ack(updated)));
    CHECK(Json::parse(store.export_ha_demo_bundle()) == updated);
    removed["tables"]["spool_identifiers"].clear();
    REQUIRE_NOTHROW(store.import_authoritative_ha_bundle(removed.dump(),ack(updated)));
    CHECK(Json::parse(store.export_ha_demo_bundle()) == removed);
}
