#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <thread>

#include <boost/filesystem.hpp>
#include <sqlite3.h>

#include "libslic3r/FilamentInventory.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r::FilamentInventory;

namespace {

struct TemporaryInventory
{
    TemporaryInventory()
        : path(fs::temp_directory_path() / fs::unique_path("quackslicer-filament-inventory-%%%%-%%%%.sqlite3"))
        , store(std::make_unique<Store>(path.string()))
    {}

    ~TemporaryInventory()
    {
        store.reset();
        boost::system::error_code ec;
        fs::remove(path, ec);
        fs::remove(path.string() + "-wal", ec);
        fs::remove(path.string() + "-shm", ec);
    }

    fs::path               path;
    std::unique_ptr<Store> store;
};

SpoolInput spool_input(const std::string &name, Milligrams remaining_mg = 1'000'000)
{
    SpoolInput input;
    input.manufacturer        = "Quack Materials";
    input.material_type       = "PLA";
    input.name                = name;
    input.color_hex           = "#12abEF";
    input.nominal_capacity_mg = 1'000'000;
    input.initial_weight_mg   = remaining_mg;
    input.warning_mode        = WarningMode::percent;
    input.warning_value       = 1'500;
    return input;
}

PrintJobInput job_input(const std::string &key, const std::string &name = "Duck")
{
    return {key, name, "duck.3mf", "printer-1"};
}

CustomerInput customer_input(const std::string &name)
{
    return {name, "Contact " + name, name + "@example.test", "+49 123", "Test customer"};
}

CustomerOrderInput order_input(const std::string &customer_id, const std::string &title)
{
    CustomerOrderInput input;
    input.customer_id  = customer_id;
    input.order_number = "Q-2026-001";
    input.title        = title;
    return input;
}

void create_v1_database(const fs::path &path)
{
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    const char *sql = R"SQL(
        CREATE TABLE spools (
            id TEXT PRIMARY KEY, manufacturer TEXT NOT NULL DEFAULT '',
            material_type TEXT NOT NULL, name TEXT NOT NULL,
            filament_preset_id TEXT NOT NULL DEFAULT '', color_hex TEXT NOT NULL,
            diameter_mm REAL NOT NULL, density_g_cm3 REAL NOT NULL,
            nominal_capacity_mg INTEGER NOT NULL, warning_mode TEXT NOT NULL,
            warning_value INTEGER NOT NULL, status TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE spool_identifiers (
            kind TEXT NOT NULL, value TEXT NOT NULL, spool_id TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT '', PRIMARY KEY(kind, value)
        );
        CREATE TABLE print_jobs (
            id TEXT PRIMARY KEY, idempotency_key TEXT NOT NULL UNIQUE,
            job_name TEXT NOT NULL, project_path TEXT NOT NULL DEFAULT '',
            printer_id TEXT NOT NULL DEFAULT '', state TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL DEFAULT '',
            completed_at TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE job_identifiers (
            provider TEXT NOT NULL, kind TEXT NOT NULL, value TEXT NOT NULL,
            job_id TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT '',
            PRIMARY KEY(provider, kind, value)
        );
        CREATE TABLE allocations (
            id TEXT PRIMARY KEY, job_id TEXT NOT NULL, spool_id TEXT NOT NULL,
            filament_index INTEGER NOT NULL, estimated_weight_mg INTEGER NOT NULL,
            actual_weight_mg INTEGER, UNIQUE(job_id, filament_index)
        );
        CREATE TABLE stock_events (
            id TEXT PRIMARY KEY, spool_id TEXT NOT NULL, job_id TEXT,
            allocation_id TEXT, event_type TEXT NOT NULL, delta_mg INTEGER NOT NULL,
            balance_after_mg INTEGER NOT NULL, operation_key TEXT NOT NULL UNIQUE,
            note TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT ''
        );
        INSERT INTO spools (
            id, material_type, name, color_hex, diameter_mm, density_g_cm3,
            nominal_capacity_mg, warning_mode, warning_value, status
        ) VALUES ('legacy-spool', 'PLA', 'Legacy', '#FFFFFF', 1.75, 1.24,
                  1000000, 'none', 0, 'active');
        INSERT INTO stock_events (
            id, spool_id, event_type, delta_mg, balance_after_mg, operation_key
        ) VALUES ('legacy-event', 'legacy-spool', 'initial', 500000, 500000, 'legacy-initial');
        INSERT INTO print_jobs (
            id, idempotency_key, job_name, project_path, printer_id, state
        ) VALUES (
            'legacy-job', 'legacy:key', 'Legacy print', 'legacy.3mf',
            'legacy-printer', 'printing'
        );
        INSERT INTO allocations (
            id, job_id, spool_id, filament_index, estimated_weight_mg
        ) VALUES (
            'legacy-allocation', 'legacy-job', 'legacy-spool', 0, 100000
        );
        PRAGMA user_version = 1;
    )SQL";
    char *error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    CHECK(result == SQLITE_OK);
    sqlite3_free(error);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}

void check_error_code(const std::function<void()> &operation, ErrorCode expected)
{
    try {
        operation();
        FAIL("Expected filament inventory operation to fail");
    } catch (const Error &error) {
        CHECK(error.code() == expected);
    }
}

void set_job_updated_at_seconds_ago(
    const fs::path &path, const std::string &job_id, int seconds)
{
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(
        sqlite3_prepare_v2(
            db,
            "UPDATE print_jobs "
            "SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now', ?) "
            "WHERE id = ?",
            -1, &statement, nullptr) == SQLITE_OK);
    const std::string modifier = "-" + std::to_string(seconds) + " seconds";
    REQUIRE(
        sqlite3_bind_text(
            statement, 1, modifier.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    REQUIRE(
        sqlite3_bind_text(
            statement, 2, job_id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_DONE);
    CHECK(sqlite3_finalize(statement) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}

} // namespace

TEST_CASE("filament colors normalize standard input models to sRGB", "[FilamentInventory][Color]")
{
    CHECK(canonical_color(ColorModel::hex, "12abef") == "#12ABEF");
    CHECK(canonical_color(ColorModel::rgb, "255, 0, 0") == "#FF0000");
    CHECK(canonical_color(ColorModel::cmyk, "0%, 100%, 100%, 0%") == "#FF0000");
    CHECK(canonical_color(ColorModel::hsl, "240, 100, 50") == "#0000FF");
    CHECK(canonical_color(ColorModel::hsv, "120, 100, 100") == "#00FF00");
    CHECK(canonical_color(ColorModel::hsv, "-120, 100, 100") == "#0000FF");
    CHECK_THROWS_AS(canonical_color(ColorModel::rgb, "256, 0, 0"), Error);
    CHECK_THROWS_AS(canonical_color(ColorModel::cmyk, "0, 0, 0"), Error);
}

TEST_CASE("sliced filament usage includes total extruded volume safely", "[FilamentInventory][Usage]")
{
    Slic3r::GCodeProcessorResult result;
    result.filament_diameters = {2.0f, 1.75f};
    result.filament_densities = {1.25f, 1.24f};
    result.print_statistics.total_volumes_per_extruder[0] = 1'000.0;
    result.print_statistics.model_volumes_per_extruder[0] = 700.0;
    result.print_statistics.support_volumes_per_extruder[0] = 300.0;
    result.print_statistics.total_volumes_per_extruder[2] = 500.0;
    result.print_statistics.total_volumes_per_extruder[1] =
        std::numeric_limits<double>::quiet_NaN();

    const std::vector<Slic3r::FilamentInfo> usages = Slic3r::collect_filament_usage(result);
    REQUIRE(usages.size() == 1);
    CHECK(usages.front().id == 0);
    CHECK(usages.front().used_g == Catch::Approx(1.25f));
    CHECK(usages.front().used_m == Catch::Approx(1.0 / 3.14159265358979323846));
    CHECK(usages.front().used_for_object);
    CHECK(usages.front().used_for_support);
}

TEST_CASE("filament inventory creates and reopens its versioned database", "[FilamentInventory]")
{
    TemporaryInventory inventory;
    CHECK(inventory.store->current_schema_version() == Store::schema_version);

    const Spool created = inventory.store->create_spool(spool_input("Ocean Blue", 750'000));
    CHECK(created.color_hex == "#12ABEF");
    CHECK(created.current_weight_mg == 750'000);
    CHECK(created.available_weight_mg == 750'000);

    const fs::path path = inventory.path;
    inventory.store.reset();
    inventory.store = std::make_unique<Store>(path.string());

    const Spool reopened = inventory.store->get_spool(created.id);
    CHECK(reopened.name == "Ocean Blue");
    CHECK(reopened.current_weight_mg == 750'000);
    CHECK(reopened.warning_mode == WarningMode::percent);
    CHECK(reopened.warning_value == 1'500);
}

TEST_CASE("filament inventory migrates v1 data with safe cost defaults", "[FilamentInventory][Migration]")
{
    const fs::path path =
        fs::temp_directory_path() /
        fs::unique_path("quackslicer-filament-v1-%%%%-%%%%.sqlite3");
    create_v1_database(path);

    {
        Store store(path.string());
        CHECK(store.current_schema_version() == Store::schema_version);
        const InventorySettings settings = store.get_settings();
        CHECK(settings.currency == "EUR");
        CHECK(settings.electricity_price_per_kwh_micros == 400'000);
        CHECK(settings.default_machine_power_watts == 150);

        const Spool legacy = store.get_spool("legacy-spool");
        CHECK(legacy.current_weight_mg == 500'000);
        CHECK(legacy.material_price_per_kg_micros == 0);
        CHECK(legacy.price_currency == "EUR");

        const PrintJob legacy_job = store.get_job("legacy-job");
        CHECK(legacy_job.started_at.empty());
        CHECK_FALSE(legacy_job.actual_runtime_seconds);
        REQUIRE(legacy_job.allocations.size() == 1);
        CHECK(legacy_job.allocations.front().spool_name == "Legacy");
        CHECK(legacy_job.allocations.front().material_type == "PLA");
        CHECK(legacy_job.allocations.front().color_hex == "#FFFFFF");
        CHECK(
            legacy_job.allocations.front()
                .estimated_material_cost_micros == 10'000);
        store.mark_printing(legacy_job.id);
        CHECK_FALSE(store.get_job(legacy_job.id).started_at.empty());
    }

    boost::system::error_code cleanup_error;
    fs::remove(path, cleanup_error);
    fs::remove(path.string() + "-wal", cleanup_error);
    fs::remove(path.string() + "-shm", cleanup_error);
}

TEST_CASE("filament inventory migrates allocation snapshots to cent costs",
          "[FilamentInventory][Migration][Cost]")
{
    TemporaryInventory inventory;
    SpoolInput input = spool_input("Migration cent");
    input.material_price_per_kg_micros = 20'000'000;
    const Spool spool = inventory.store->create_spool(input);
    const PrintJob completed = inventory.store->reserve_job(
        job_input("migration:completed"),
        {{spool.id, 0, 800}, {spool.id, 1, 1}});
    inventory.store->commit_job(completed.id, {{0, 800}, {1, 0}});
    const PrintJob open = inventory.store->reserve_job(
        job_input("migration:open"), {{spool.id, 0, 1}});

    inventory.store.reset();
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(inventory.path.string().c_str(), &db) == SQLITE_OK);
    char *error = nullptr;
    const int result = sqlite3_exec(
        db,
        "UPDATE allocations "
        "SET estimated_material_cost_micros = 1, "
        "    actual_material_cost_micros = "
        "        CASE WHEN actual_weight_mg IS NULL THEN NULL ELSE 1 END;"
        "PRAGMA user_version = 3;",
        nullptr, nullptr, &error);
    const std::string sqlite_error =
        error != nullptr ? error : std::string {};
    INFO(sqlite_error);
    REQUIRE(result == SQLITE_OK);
    sqlite3_free(error);
    REQUIRE(sqlite3_close(db) == SQLITE_OK);

    inventory.store = std::make_unique<Store>(inventory.path.string());
    CHECK(inventory.store->current_schema_version() == Store::schema_version);
    const PrintJob migrated_completed =
        inventory.store->get_job(completed.id);
    REQUIRE(migrated_completed.allocations.size() == 2);
    CHECK(
        migrated_completed.allocations[0]
            .estimated_material_cost_micros == 20'000);
    CHECK(
        migrated_completed.allocations[0]
            .actual_material_cost_micros == 20'000);
    CHECK(
        migrated_completed.allocations[1]
            .estimated_material_cost_micros == 10'000);
    CHECK(
        migrated_completed.allocations[1]
            .actual_material_cost_micros == 0);
    const PrintJob migrated_open = inventory.store->get_job(open.id);
    REQUIRE(migrated_open.allocations.size() == 1);
    CHECK(
        migrated_open.allocations[0]
            .estimated_material_cost_micros == 10'000);
    CHECK_FALSE(
        migrated_open.allocations[0].actual_material_cost_micros);

    inventory.store.reset();
    inventory.store = std::make_unique<Store>(inventory.path.string());
    CHECK(
        inventory.store->get_job(completed.id).allocations[0]
            .estimated_material_cost_micros == 20'000);
}

TEST_CASE("first-use schema migration is serialized across connections", "[FilamentInventory]")
{
    const fs::path path =
        fs::temp_directory_path() /
        fs::unique_path("quackslicer-filament-migration-%%%%-%%%%.sqlite3");
    std::atomic<bool> start {false};
    std::exception_ptr first_error;
    std::exception_ptr second_error;
    int first_version = 0;
    int second_version = 0;

    const auto open_store = [&](int &version, std::exception_ptr &error) {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            Store store(path.string());
            version = store.current_schema_version();
        } catch (...) {
            error = std::current_exception();
        }
    };

    std::thread first(open_store, std::ref(first_version), std::ref(first_error));
    std::thread second(open_store, std::ref(second_version), std::ref(second_error));
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    boost::system::error_code cleanup_error;
    fs::remove(path, cleanup_error);
    fs::remove(path.string() + "-wal", cleanup_error);
    fs::remove(path.string() + "-shm", cleanup_error);

    if (first_error)
        std::rethrow_exception(first_error);
    if (second_error)
        std::rethrow_exception(second_error);
    CHECK(first_version == Store::schema_version);
    CHECK(second_version == Store::schema_version);
}

TEST_CASE("filament inventory keeps NFC and Bambu identifiers unambiguous", "[FilamentInventory][NFC]")
{
    TemporaryInventory inventory;
    const Spool first  = inventory.store->create_spool(spool_input("First"));
    const Spool second = inventory.store->create_spool(spool_input("Second"));

    inventory.store->bind_identifier(first.id, IdentifierKind::nfc_uid, "04:aa-bb cc");
    const auto by_uid = inventory.store->find_spool(IdentifierKind::nfc_uid, "04 AA BB CC");
    REQUIRE(by_uid);
    CHECK(by_uid->id == first.id);

    const std::string payload_uuid = "35A46B26-82B9-4C0D-B09E-F620F1D4C324";
    inventory.store->bind_identifier(first.id, IdentifierKind::quack_ndef_uuid, payload_uuid);
    const auto by_payload = inventory.store->find_spool(
        IdentifierKind::quack_ndef_uuid, "35a46b26-82b9-4c0d-b09e-f620f1d4c324");
    REQUIRE(by_payload);
    CHECK(by_payload->id == first.id);

    check_error_code(
        [&] { inventory.store->bind_identifier(second.id, IdentifierKind::nfc_uid, "04AABBCC"); },
        ErrorCode::conflict);
    CHECK(inventory.store->list_identifiers(first.id).size() == 3);
    CHECK(inventory.store->list_identifiers().size() == 4);

    const auto by_default_payload = inventory.store->find_spool(
        IdentifierKind::quack_ndef_uuid, second.id);
    REQUIRE(by_default_payload);
    CHECK(by_default_payload->id == second.id);
}

TEST_CASE("physical filament identifiers are replaced atomically", "[FilamentInventory][NFC]")
{
    TemporaryInventory inventory;
    const Spool first = inventory.store->create_spool(
        spool_input("First"), {{IdentifierKind::nfc_uid, "01020304"}});
    const Spool second = inventory.store->create_spool(
        spool_input("Second"), {{IdentifierKind::bambu_tag_uid, "AABBCCDD"}});

    inventory.store->replace_physical_identifiers(
        first.id,
        {{IdentifierKind::nfc_uid, "11223344"}, {IdentifierKind::bambu_tag_uid, "55667788"}});
    CHECK_FALSE(inventory.store->find_spool(IdentifierKind::nfc_uid, "01020304"));
    REQUIRE(inventory.store->find_spool(IdentifierKind::nfc_uid, "11223344"));
    REQUIRE(inventory.store->find_spool(IdentifierKind::bambu_tag_uid, "55667788"));

    check_error_code(
        [&] {
            inventory.store->replace_physical_identifiers(
                first.id, {{IdentifierKind::bambu_tag_uid, "AABBCCDD"}});
        },
        ErrorCode::conflict);
    REQUIRE(inventory.store->find_spool(IdentifierKind::nfc_uid, "11223344"));
    REQUIRE(inventory.store->find_spool(IdentifierKind::bambu_tag_uid, "55667788"));
    REQUIRE(inventory.store->find_spool(IdentifierKind::bambu_tag_uid, "AABBCCDD"));
    CHECK(inventory.store->find_spool(IdentifierKind::bambu_tag_uid, "AABBCCDD")->id == second.id);
}

TEST_CASE("spool creation and supplied tag binding are atomic", "[FilamentInventory][NFC]")
{
    TemporaryInventory inventory;
    inventory.store->create_spool(
        spool_input("Tagged"), {{IdentifierKind::bambu_tag_uid, "A1B2C3D4"}});

    check_error_code(
        [&] {
            inventory.store->create_spool(
                spool_input("Must roll back"), {{IdentifierKind::bambu_tag_uid, "A1:B2:C3:D4"}});
        },
        ErrorCode::conflict);

    CHECK(inventory.store->list_spools().size() == 1);
}

TEST_CASE("customers and orders retain optional commercial amounts", "[FilamentInventory][Customer]")
{
    TemporaryInventory inventory;
    const Customer customer =
        inventory.store->create_customer(customer_input("Duck Family"));
    CHECK_FALSE(customer.archived);
    REQUIRE(inventory.store->list_customers().size() == 1);

    CustomerOrderInput free_order = order_input(customer.id, "Birthday present");
    const CustomerOrder created =
        inventory.store->create_customer_order(free_order);
    CHECK(created.status == CustomerOrderStatus::draft);
    CHECK_FALSE(created.quoted_price_micros);
    CHECK_FALSE(created.invoice_amount_micros);
    CHECK(created.currency == "EUR");

    free_order.quoted_price_micros = 12'500'000;
    free_order.invoice_amount_micros = 10'000'000;
    const CustomerOrder priced =
        inventory.store->update_customer_order(created.id, free_order);
    CHECK(priced.quoted_price_micros == 12'500'000);
    CHECK(priced.invoice_amount_micros == 10'000'000);

    inventory.store->set_customer_order_status(created.id, CustomerOrderStatus::active);
    CHECK(inventory.store->get_customer_order(created.id).status ==
          CustomerOrderStatus::active);
    REQUIRE(inventory.store->list_customer_orders(customer.id, false).size() == 1);

    inventory.store->archive_customer(customer.id);
    CHECK(inventory.store->list_customers().empty());
    REQUIRE(inventory.store->list_customers(true).size() == 1);
    CHECK(inventory.store->list_customers(true).front().archived);
}

TEST_CASE("inventory money uses normalized three-letter currencies", "[FilamentInventory][Cost]")
{
    TemporaryInventory inventory;
    InventorySettings settings = inventory.store->get_settings();
    settings.currency = "eur";
    CHECK(inventory.store->update_settings(settings).currency == "EUR");
    settings.currency = "EURO";
    check_error_code(
        [&] { inventory.store->update_settings(settings); },
        ErrorCode::validation);

    SpoolInput input = spool_input("Currency");
    input.price_currency = "usd";
    const Spool spool = inventory.store->create_spool(input);
    CHECK(spool.price_currency == "USD");

    settings.currency = "usd";
    inventory.store->update_settings(settings);
    const Customer customer =
        inventory.store->create_customer(customer_input("USD customer"));
    CustomerOrderInput order = order_input(customer.id, "USD order");
    order.currency = settings.currency;
    const CustomerOrder created_order =
        inventory.store->create_customer_order(order);
    PrintJobInput job = job_input("currency:usd", "USD print");
    job.customer_order_id = created_order.id;
    CHECK(
        inventory.store->reserve_job(job, {{spool.id, 0, 10'000}})
            .cost_currency == "USD");
}

TEST_CASE("material usage summaries combine rolls and preserve material boundaries",
          "[FilamentInventory][MaterialSummary]")
{
    PrintJob completed;
    completed.state = JobState::completed;
    Allocation first;
    first.spool_name = "Black roll 1";
    first.manufacturer = "Quack Materials";
    first.material_type = "PLA";
    first.filament_preset_id = "quack-pla";
    first.color_hex = "#111111";
    first.cost_currency = "EUR";
    first.estimated_weight_mg = 100'000;
    first.actual_weight_mg = 80'000;
    first.estimated_material_cost_micros = 2'000'000;
    first.actual_material_cost_micros = 1'600'000;
    completed.allocations.emplace_back(first);

    PrintJob open;
    open.state = JobState::printing;
    Allocation second = first;
    second.spool_name = "Black roll 2";
    second.estimated_weight_mg = 50'000;
    second.actual_weight_mg.reset();
    second.estimated_material_cost_micros = 1'500'000;
    second.actual_material_cost_micros.reset();
    open.allocations.emplace_back(second);

    Allocation zero = first;
    zero.spool_name = "Orange roll";
    zero.color_hex = "#FF6600";
    zero.estimated_weight_mg = 10'000;
    zero.actual_weight_mg = 0;
    zero.estimated_material_cost_micros = 200'000;
    zero.actual_material_cost_micros = 0;
    completed.allocations.emplace_back(zero);

    PrintJob discarded;
    discarded.state = JobState::discarded;
    Allocation ignored = first;
    ignored.estimated_weight_mg = 999'000;
    ignored.estimated_material_cost_micros = 99'000'000;
    discarded.allocations.emplace_back(ignored);

    const std::vector<MaterialUsageSummary> summaries =
        summarize_material_usage({completed, open, discarded});
    REQUIRE(summaries.size() == 2);
    const auto black = std::find_if(
        summaries.begin(), summaries.end(),
        [](const MaterialUsageSummary &summary) {
            return summary.color_hex == "#111111";
        });
    REQUIRE(black != summaries.end());
    CHECK(black->spool_name.empty());
    CHECK(black->estimated_weight_mg == 150'000);
    CHECK(black->best_known_weight_mg == 130'000);
    CHECK(black->estimated_material_cost_micros == 3'500'000);
    CHECK(black->best_known_material_cost_micros == 3'100'000);
    CHECK_FALSE(black->weight_fully_confirmed);
    CHECK_FALSE(black->cost_fully_confirmed);

    const auto orange = std::find_if(
        summaries.begin(), summaries.end(),
        [](const MaterialUsageSummary &summary) {
            return summary.color_hex == "#FF6600";
        });
    REQUIRE(orange != summaries.end());
    CHECK(orange->best_known_weight_mg == 0);
    CHECK(orange->best_known_material_cost_micros == 0);
    CHECK(orange->weight_fully_confirmed);
    CHECK(orange->cost_fully_confirmed);
}

TEST_CASE("material usage summaries keep identity and currency boundaries",
          "[FilamentInventory][MaterialSummary]")
{
    Allocation base;
    base.spool_name = "Base roll";
    base.manufacturer = "Maker";
    base.material_type = "PLA";
    base.filament_preset_id = "maker-pla";
    base.color_hex = "#111111";
    base.cost_currency = "EUR";
    base.estimated_weight_mg = 1'000;
    base.actual_weight_mg = 1'000;
    base.estimated_material_cost_micros = 10'000;
    base.actual_material_cost_micros = 10'000;

    PrintJob job;
    job.state = JobState::completed;
    job.allocations.emplace_back(base);
    Allocation same = base;
    same.spool_name = "Second physical roll";
    same.cost_currency = "eur";
    job.allocations.emplace_back(same);
    Allocation other_manufacturer = base;
    other_manufacturer.manufacturer = "Other maker";
    job.allocations.emplace_back(other_manufacturer);
    Allocation other_type = base;
    other_type.material_type = "PETG";
    job.allocations.emplace_back(other_type);
    Allocation other_preset = base;
    other_preset.filament_preset_id = "maker-pla-tough";
    job.allocations.emplace_back(other_preset);
    Allocation other_color = base;
    other_color.color_hex = "#222222";
    job.allocations.emplace_back(other_color);
    Allocation other_currency = base;
    other_currency.cost_currency = "USD";
    job.allocations.emplace_back(other_currency);
    Allocation unknown_a = base;
    unknown_a.spool_name = "Unknown A";
    unknown_a.manufacturer.clear();
    unknown_a.material_type.clear();
    unknown_a.filament_preset_id.clear();
    job.allocations.emplace_back(unknown_a);
    Allocation unknown_b = unknown_a;
    unknown_b.spool_name = "Unknown B";
    job.allocations.emplace_back(unknown_b);

    const std::vector<MaterialUsageSummary> summaries =
        summarize_material_usage({job});
    REQUIRE(summaries.size() == 8);
    const auto combined = std::find_if(
        summaries.begin(), summaries.end(),
        [](const MaterialUsageSummary &summary) {
            return summary.manufacturer == "Maker" &&
                   summary.material_type == "PLA" &&
                   summary.filament_preset_id == "maker-pla" &&
                   summary.color_hex == "#111111" &&
                   summary.cost_currency == "EUR";
        });
    REQUIRE(combined != summaries.end());
    CHECK(combined->spool_name.empty());
    CHECK(combined->best_known_weight_mg == 2'000);
    CHECK(combined->best_known_material_cost_micros == 20'000);
    CHECK(std::count_if(
              summaries.begin(), summaries.end(),
              [](const MaterialUsageSummary &summary) {
                  return summary.color_hex == "#111111";
              }) == 7);
    CHECK(std::count_if(
              summaries.begin(), summaries.end(),
              [](const MaterialUsageSummary &summary) {
                  return summary.spool_name == "Unknown A" ||
                         summary.spool_name == "Unknown B";
              }) == 2);
}

TEST_CASE("job costs combine fixed-point material and machine electricity", "[FilamentInventory][Cost]")
{
    TemporaryInventory inventory;
    InventorySettings settings = inventory.store->get_settings();
    settings.electricity_price_per_kwh_micros = 400'000; // 0.40 EUR/kWh
    settings.default_machine_power_watts = 150;
    inventory.store->update_settings(settings);

    SpoolInput priced_spool = spool_input("Priced");
    priced_spool.material_price_per_kg_micros = 20'000'000; // 20 EUR/kg
    const Spool spool = inventory.store->create_spool(priced_spool);
    const Customer customer = inventory.store->create_customer(customer_input("Acme"));
    CustomerOrderInput order_data = order_input(customer.id, "250 g duck");
    order_data.quoted_price_micros = 15'000'000;
    const CustomerOrder order = inventory.store->create_customer_order(order_data);

    PrintJobInput input = job_input("cost:duck");
    input.customer_order_id = order.id;
    input.estimated_runtime_seconds = 7'200;
    input.machine_power_watts = 0; // Uses the 150 W setting.
    const PrintJob job =
        inventory.store->reserve_job(input, {{spool.id, 0, 250'000}});

    CHECK(job.machine_power_watts == 150);
    CHECK(job.electricity_cost_micros == 120'000);
    REQUIRE(job.allocations.size() == 1);
    CHECK(job.allocations.front().material_price_per_kg_micros == 20'000'000);
    CHECK(job.allocations.front().estimated_material_cost_micros == 5'000'000);

    const CostSummary estimated = inventory.store->job_cost_summary(job.id);
    CHECK(estimated.currency == "EUR");
    CHECK(estimated.estimated_material_cost_micros == 5'000'000);
    CHECK_FALSE(estimated.actual_material_cost_micros);
    CHECK(estimated.material_cost_micros == 5'000'000);
    CHECK(estimated.electricity_cost_micros == 120'000);
    CHECK(estimated.total_cost_micros == 5'120'000);

    inventory.store->commit_job(job.id, {{0, 200'000}});
    const CostSummary actual = inventory.store->customer_order_cost_summary(order.id);
    CHECK(actual.estimated_material_cost_micros == 5'000'000);
    REQUIRE(actual.actual_material_cost_micros);
    CHECK(*actual.actual_material_cost_micros == 4'000'000);
    CHECK(actual.material_cost_micros == 4'000'000);
    CHECK(actual.electricity_cost_micros == 120'000);
    CHECK(actual.total_cost_micros == 4'120'000);
    CHECK(actual.quoted_price_micros == 15'000'000);
    CHECK_FALSE(actual.invoice_amount_micros);
}

TEST_CASE("positive material usage is booked in rounded whole cents",
          "[FilamentInventory][Cost][Reservation]")
{
    TemporaryInventory inventory;
    SpoolInput priced_input = spool_input("Cent-priced");
    priced_input.material_price_per_kg_micros = 20'000'000;
    const Spool priced = inventory.store->create_spool(priced_input);
    SpoolInput zero_price_input = spool_input("Minimum-charge");
    zero_price_input.material_price_per_kg_micros = 0;
    const Spool zero_price =
        inventory.store->create_spool(zero_price_input);
    const Customer customer =
        inventory.store->create_customer(customer_input("Cent customer"));
    const CustomerOrder order =
        inventory.store->create_customer_order(
            order_input(customer.id, "Cent order"));

    PrintJobInput input = job_input("cost:cent-rounding");
    input.customer_order_id = order.id;
    const PrintJob reserved = inventory.store->reserve_job(
        input,
        {
            {priced.id, 0, 1},       // 0.00002 EUR -> minimum 0.01 EUR.
            {priced.id, 1, 500},     // Exactly 0.01 EUR.
            {priced.id, 2, 800},     // 0.016 EUR -> 0.02 EUR.
            {zero_price.id, 3, 1'000} // Positive use always costs at least 0.01.
        });
    REQUIRE(reserved.allocations.size() == 4);
    CHECK(reserved.allocations[0].estimated_material_cost_micros == 10'000);
    CHECK(reserved.allocations[1].estimated_material_cost_micros == 10'000);
    CHECK(reserved.allocations[2].estimated_material_cost_micros == 20'000);
    CHECK(reserved.allocations[3].estimated_material_cost_micros == 10'000);
    CHECK(
        inventory.store->job_cost_summary(reserved.id)
            .material_cost_micros == 50'000);
    CHECK(
        inventory.store->customer_order_cost_summary(order.id)
            .material_cost_micros == 50'000);

    inventory.store->commit_job(
        reserved.id,
        {{0, 0}, {1, 1}, {2, 800}, {3, 1'000}});
    const PrintJob completed = inventory.store->get_job(reserved.id);
    REQUIRE(completed.allocations[0].actual_material_cost_micros);
    CHECK(*completed.allocations[0].actual_material_cost_micros == 0);
    CHECK(*completed.allocations[1].actual_material_cost_micros == 10'000);
    CHECK(*completed.allocations[2].actual_material_cost_micros == 20'000);
    CHECK(*completed.allocations[3].actual_material_cost_micros == 10'000);
    CHECK(
        inventory.store->customer_cost_summary(customer.id)
            .material_cost_micros == 40'000);

    const std::vector<MaterialUsageSummary> usage =
        summarize_material_usage({completed});
    REQUIRE(usage.size() == 1);
    CHECK(usage.front().best_known_weight_mg == 1'801);
    CHECK(usage.front().best_known_material_cost_micros == 40'000);
    CHECK(usage.front().weight_fully_confirmed);
    CHECK(usage.front().cost_fully_confirmed);

    PrintJobInput discarded_input = job_input(
        "cost:cent-discarded", "Discarded cent");
    discarded_input.customer_order_id = order.id;
    const PrintJob discarded = inventory.store->reserve_job(
        discarded_input, {{zero_price.id, 0, 1}});
    CHECK(
        discarded.allocations.front().estimated_material_cost_micros ==
        10'000);
    inventory.store->discard_job(discarded.id);
    CHECK(
        inventory.store->job_cost_summary(discarded.id)
            .material_cost_micros == 0);
    CHECK(
        inventory.store->customer_order_cost_summary(order.id)
            .material_cost_micros == 40'000);
}

TEST_CASE("one customer order aggregates several print jobs", "[FilamentInventory][Customer][Cost]")
{
    TemporaryInventory inventory;
    SpoolInput spool_data = spool_input("Order spool");
    spool_data.material_price_per_kg_micros = 10'000'000;
    const Spool spool = inventory.store->create_spool(spool_data);
    const Customer customer = inventory.store->create_customer(customer_input("Multi Job"));
    const CustomerOrder order =
        inventory.store->create_customer_order(order_input(customer.id, "Three parts"));

    PrintJobInput first_input = job_input("order:first", "First part");
    first_input.customer_order_id = order.id;
    const PrintJob first =
        inventory.store->reserve_job(first_input, {{spool.id, 0, 100'000}});
    PrintJobInput second_input = job_input("order:second", "Second part");
    second_input.customer_order_id = order.id;
    const PrintJob second =
        inventory.store->reserve_job(second_input, {{spool.id, 0, 200'000}});

    REQUIRE(first.customer_order_id);
    REQUIRE(second.customer_order_id);
    CHECK(*first.customer_order_id == order.id);
    CHECK(*second.customer_order_id == order.id);
    const CostSummary order_cost =
        inventory.store->customer_order_cost_summary(order.id);
    CHECK(order_cost.estimated_material_cost_micros == 3'000'000);
    CHECK(order_cost.total_cost_micros == 3'000'000);
    CHECK(inventory.store->customer_cost_summary(customer.id).total_cost_micros ==
          3'000'000);
    check_error_code(
        [&] { inventory.store->delete_customer_order(order.id); },
        ErrorCode::conflict);
}

TEST_CASE("customer order job details stay scoped and can include discarded jobs",
          "[FilamentInventory][Customer][Reservation]")
{
    TemporaryInventory inventory;
    const Spool spool =
        inventory.store->create_spool(spool_input("Order details"));
    SpoolInput accent_input = spool_input("Order accent");
    accent_input.manufacturer = "Accent Materials";
    accent_input.material_type = "PETG";
    accent_input.color_hex = "#FF6600";
    const Spool accent = inventory.store->create_spool(accent_input);
    const Customer customer =
        inventory.store->create_customer(customer_input("Order details customer"));
    const CustomerOrder first_order =
        inventory.store->create_customer_order(
            order_input(customer.id, "First order"));
    CustomerOrderInput second_order_input =
        order_input(customer.id, "Second order");
    second_order_input.order_number = "Q-2026-002";
    const CustomerOrder second_order =
        inventory.store->create_customer_order(second_order_input);

    PrintJobInput kept_input = job_input("order-details:kept", "Kept");
    kept_input.customer_order_id = first_order.id;
    const PrintJob kept = inventory.store->reserve_job(
        kept_input,
        {{spool.id, 0, 10'000}, {accent.id, 1, 15'000}});
    PrintJobInput discarded_input =
        job_input("order-details:discarded", "Discarded");
    discarded_input.customer_order_id = first_order.id;
    const PrintJob discarded = inventory.store->reserve_job(
        discarded_input, {{spool.id, 0, 20'000}});
    inventory.store->discard_job(discarded.id);
    PrintJobInput other_input = job_input("order-details:other", "Other");
    other_input.customer_order_id = second_order.id;
    inventory.store->reserve_job(
        other_input, {{spool.id, 0, 30'000}});
    inventory.store->reserve_job(
        job_input("order-details:personal", "Personal"),
        {{spool.id, 0, 40'000}});

    const std::vector<PrintJob> all =
        inventory.store->list_customer_order_jobs(first_order.id);
    REQUIRE(all.size() == 2);
    CHECK(std::count_if(
              all.begin(), all.end(),
              [&kept](const PrintJob &job) { return job.id == kept.id; }) == 1);
    CHECK(std::count_if(
              all.begin(), all.end(),
              [&discarded](const PrintJob &job) {
                  return job.id == discarded.id;
              }) == 1);
    const auto kept_details = std::find_if(
        all.begin(), all.end(),
        [&kept](const PrintJob &job) { return job.id == kept.id; });
    REQUIRE(kept_details != all.end());
    REQUIRE(kept_details->allocations.size() == 2);
    CHECK(kept_details->allocations[0].material_type == "PLA");
    CHECK(kept_details->allocations[0].estimated_weight_mg == 10'000);
    CHECK(kept_details->allocations[1].manufacturer == "Accent Materials");
    CHECK(kept_details->allocations[1].material_type == "PETG");
    CHECK(kept_details->allocations[1].color_hex == "#FF6600");
    CHECK(kept_details->allocations[1].estimated_weight_mg == 15'000);

    const std::vector<PrintJob> active =
        inventory.store->list_customer_order_jobs(first_order.id, false);
    REQUIRE(active.size() == 1);
    CHECK(active.front().id == kept.id);
    check_error_code(
        [&] { inventory.store->list_customer_order_jobs("missing-order"); },
        ErrorCode::not_found);
}

TEST_CASE("customer orders only close after their print jobs", "[FilamentInventory][Customer]")
{
    TemporaryInventory inventory;
    const Spool spool = inventory.store->create_spool(spool_input("Order state"));
    const Customer customer =
        inventory.store->create_customer(customer_input("Order state customer"));
    const CustomerOrder order =
        inventory.store->create_customer_order(order_input(customer.id, "Order state"));

    check_error_code(
        [&] {
            inventory.store->set_customer_order_status(
                order.id, CustomerOrderStatus::completed);
        },
        ErrorCode::conflict);
    inventory.store->set_customer_order_status(
        order.id, CustomerOrderStatus::active);

    PrintJobInput print = job_input("order:state");
    print.customer_order_id = order.id;
    const PrintJob job =
        inventory.store->reserve_job(print, {{spool.id, 0, 100'000}});
    check_error_code(
        [&] {
            inventory.store->set_customer_order_status(
                order.id, CustomerOrderStatus::completed);
        },
        ErrorCode::conflict);

    inventory.store->commit_job(job.id);
    inventory.store->set_customer_order_status(
        order.id, CustomerOrderStatus::completed);
    CHECK(inventory.store->get_customer_order(order.id).status ==
          CustomerOrderStatus::completed);
    check_error_code(
        [&] {
            inventory.store->set_customer_order_status(
                order.id, CustomerOrderStatus::active);
        },
        ErrorCode::conflict);
}

TEST_CASE(
    "job material costs follow current spool prices while other snapshots remain stable",
    "[FilamentInventory][Cost]")
{
    TemporaryInventory inventory;
    SpoolInput input = spool_input("Snapshot");
    input.material_price_per_kg_micros = 20'000'000;
    const Spool spool = inventory.store->create_spool(input);

    PrintJobInput print = job_input("cost:snapshot");
    print.estimated_runtime_seconds = 3'600;
    const PrintJob reserved =
        inventory.store->reserve_job(print, {{spool.id, 0, 100'000}});
    CHECK(reserved.electricity_cost_micros == 60'000);
    CHECK(reserved.allocations.front().estimated_material_cost_micros == 2'000'000);
    CHECK(reserved.allocations.front().spool_name == "Snapshot");
    CHECK(reserved.allocations.front().manufacturer == "Quack Materials");
    CHECK(reserved.allocations.front().material_type == "PLA");
    CHECK(reserved.allocations.front().color_hex == "#12ABEF");

    input.material_price_per_kg_micros = 50'000'000;
    input.initial_weight_mg = spool.current_weight_mg;
    input.name = "Renamed snapshot spool";
    input.manufacturer = "Different manufacturer";
    input.material_type = "PETG";
    input.filament_preset_id = "different-preset";
    input.color_hex = "#AABBCC";
    inventory.store->update_spool(spool.id, input, "snapshot-price-edit");
    InventorySettings settings = inventory.store->get_settings();
    settings.electricity_price_per_kwh_micros = 900'000;
    settings.default_machine_power_watts = 300;
    inventory.store->update_settings(settings);

    const PrintJob repeated =
        inventory.store->reserve_job(print, {{spool.id, 0, 100'000}});
    CHECK(repeated.id == reserved.id);
    CHECK(repeated.electricity_price_per_kwh_micros == 400'000);
    CHECK(repeated.machine_power_watts == 150);
    CHECK(repeated.electricity_cost_micros == 60'000);
    CHECK(repeated.allocations.front().material_price_per_kg_micros == 50'000'000);
    CHECK(repeated.allocations.front().estimated_material_cost_micros == 5'000'000);
    CHECK(repeated.allocations.front().spool_name == "Snapshot");
    CHECK(repeated.allocations.front().manufacturer == "Quack Materials");
    CHECK(repeated.allocations.front().material_type == "PLA");
    CHECK(repeated.allocations.front().filament_preset_id.empty());
    CHECK(repeated.allocations.front().color_hex == "#12ABEF");

    inventory.store->commit_job(reserved.id, {{0, 80'000}});
    const CostSummary repriced_summary =
        inventory.store->job_cost_summary(reserved.id);
    REQUIRE(repriced_summary.actual_material_cost_micros);
    CHECK(*repriced_summary.actual_material_cost_micros == 4'000'000);
    CHECK(repriced_summary.total_cost_micros == 4'060'000);

    input.material_price_per_kg_micros = 20'000'000;
    input.initial_weight_mg =
        inventory.store->get_spool(spool.id).current_weight_mg;
    inventory.store->update_spool(
        spool.id, input, "snapshot-second-price-edit");
    const CostSummary historical_summary =
        inventory.store->job_cost_summary(reserved.id);
    REQUIRE(historical_summary.actual_material_cost_micros);
    CHECK(*historical_summary.actual_material_cost_micros == 1'600'000);
    CHECK(historical_summary.total_cost_micros == 1'660'000);
}

TEST_CASE(
    "customer order material costs recalculate from the referenced spool UUID",
    "[FilamentInventory][Cost][Customer]")
{
    TemporaryInventory inventory;
    SpoolInput input = spool_input("Green order spool");
    input.material_price_per_kg_micros = 0;
    const Spool spool = inventory.store->create_spool(input);
    const std::optional<Spool> by_uuid =
        inventory.store->find_spool(
            IdentifierKind::quack_ndef_uuid, spool.id);
    REQUIRE(by_uuid);
    CHECK(by_uuid->id == spool.id);

    const Customer customer =
        inventory.store->create_customer(customer_input("UUID customer"));
    const CustomerOrder order =
        inventory.store->create_customer_order(
            order_input(customer.id, "Green filament order"));
    PrintJobInput print = job_input("cost:live-spool-price");
    print.customer_order_id = order.id;
    const PrintJob reserved = inventory.store->reserve_job(
        print, {{spool.id, 0, 21'300}});
    REQUIRE(reserved.allocations.size() == 1);
    CHECK(reserved.allocations.front().spool_id == spool.id);
    CHECK(
        reserved.allocations.front().estimated_material_cost_micros ==
        10'000);

    input.material_price_per_kg_micros = 20'000'000;
    input.initial_weight_mg = spool.current_weight_mg;
    inventory.store->update_spool(
        spool.id, input, "live-price-20-eur");

    const PrintJob repriced = inventory.store->get_job(reserved.id);
    REQUIRE(repriced.allocations.size() == 1);
    CHECK(repriced.allocations.front().spool_id == spool.id);
    CHECK(
        repriced.allocations.front().material_price_per_kg_micros ==
        20'000'000);
    CHECK(
        repriced.allocations.front().estimated_material_cost_micros ==
        430'000);
    CHECK(
        inventory.store->job_cost_summary(reserved.id)
            .material_cost_micros == 430'000);
    CHECK(
        inventory.store->customer_order_cost_summary(order.id)
            .material_cost_micros == 430'000);
    CHECK(
        inventory.store->customer_cost_summary(customer.id)
            .material_cost_micros == 430'000);

    inventory.store->commit_job(reserved.id, {{0, 21'300}});
    const PrintJob completed = inventory.store->get_job(reserved.id);
    REQUIRE(
        completed.allocations.front().actual_material_cost_micros);
    CHECK(
        *completed.allocations.front().actual_material_cost_micros ==
        430'000);

    input.material_price_per_kg_micros = 25'000'000;
    input.initial_weight_mg =
        inventory.store->get_spool(spool.id).current_weight_mg;
    inventory.store->update_spool(
        spool.id, input, "live-price-25-eur");
    const PrintJob historical = inventory.store->get_job(reserved.id);
    CHECK(
        historical.allocations.front().material_price_per_kg_micros ==
        25'000'000);
    REQUIRE(
        historical.allocations.front().actual_material_cost_micros);
    CHECK(
        *historical.allocations.front().actual_material_cost_micros ==
        530'000);
    CHECK(
        inventory.store->customer_order_cost_summary(order.id)
            .material_cost_micros == 530'000);
    CHECK(
        inventory.store->customer_cost_summary(customer.id)
            .material_cost_micros == 530'000);

    const std::vector<PrintJob> order_jobs =
        inventory.store->list_customer_order_jobs(order.id, true);
    REQUIRE(order_jobs.size() == 1);
    CHECK(
        order_jobs.front().allocations.front().spool_id ==
        spool.id);
    const std::vector<MaterialUsageSummary> usage =
        summarize_material_usage(order_jobs);
    REQUIRE(usage.size() == 1);
    CHECK(usage.front().best_known_weight_mg == 21'300);
    CHECK(usage.front().best_known_material_cost_micros == 530'000);

    SpoolInput incompatible_currency = input;
    incompatible_currency.price_currency = "USD";
    check_error_code(
        [&] {
            inventory.store->update_spool(
                spool.id, incompatible_currency,
                "live-price-currency-change");
        },
        ErrorCode::conflict);
    CHECK(
        inventory.store->get_spool(spool.id)
            .material_price_per_kg_micros == 25'000'000);
    CHECK(
        inventory.store->customer_order_cost_summary(order.id)
            .material_cost_micros == 530'000);

    inventory.store.reset();
    inventory.store =
        std::make_unique<Store>(inventory.path.string());
    CHECK(
        inventory.store->get_job(reserved.id)
            .allocations.front().material_price_per_kg_micros ==
        25'000'000);
    CHECK(
        inventory.store->customer_order_cost_summary(order.id)
            .material_cost_micros == 530'000);
}

TEST_CASE(
    "legacy spool currency changes preserve booked prices without blocking matching jobs",
    "[FilamentInventory][Cost][Migration]")
{
    TemporaryInventory inventory;
    SpoolInput input = spool_input("Legacy currency spool");
    input.material_price_per_kg_micros = 20'000'000;
    const Spool spool = inventory.store->create_spool(input);
    const PrintJob eur_job = inventory.store->reserve_job(
        job_input("cost:legacy-eur"), {{spool.id, 0, 100'000}});
    CHECK(
        inventory.store->job_cost_summary(eur_job.id)
            .material_cost_micros == 2'000'000);

    inventory.store.reset();
    sqlite3 *db = nullptr;
    REQUIRE(
        sqlite3_open(inventory.path.string().c_str(), &db) ==
        SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(
        sqlite3_prepare_v2(
            db,
            "UPDATE spools "
            "SET material_price_per_kg_micros = ?, price_currency = ? "
            "WHERE id = ?",
            -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(
        sqlite3_bind_int64(statement, 1, 50'000'000) ==
        SQLITE_OK);
    REQUIRE(
        sqlite3_bind_text(
            statement, 2, "USD", -1, SQLITE_STATIC) ==
        SQLITE_OK);
    REQUIRE(
        sqlite3_bind_text(
            statement, 3, spool.id.c_str(), -1, SQLITE_TRANSIENT) ==
        SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    REQUIRE(sqlite3_close(db) == SQLITE_OK);

    inventory.store =
        std::make_unique<Store>(inventory.path.string());
    const PrintJob legacy = inventory.store->get_job(eur_job.id);
    REQUIRE(legacy.allocations.size() == 1);
    CHECK(
        legacy.allocations.front().material_price_per_kg_micros ==
        20'000'000);
    CHECK(legacy.allocations.front().cost_currency == "EUR");
    CHECK(
        legacy.allocations.front().estimated_material_cost_micros ==
        2'000'000);

    input.material_price_per_kg_micros = 60'000'000;
    input.price_currency = "USD";
    input.initial_weight_mg =
        inventory.store->get_spool(spool.id).current_weight_mg;
    inventory.store->update_spool(
        spool.id, input, "legacy-usd-price-edit");
    CHECK(
        inventory.store->job_cost_summary(eur_job.id)
            .material_cost_micros == 2'000'000);

    InventorySettings settings = inventory.store->get_settings();
    settings.currency = "USD";
    inventory.store->update_settings(settings);
    const PrintJob usd_job = inventory.store->reserve_job(
        job_input("cost:matching-usd"), {{spool.id, 0, 100'000}});
    CHECK(
        inventory.store->job_cost_summary(usd_job.id)
            .material_cost_micros == 6'000'000);

    input.material_price_per_kg_micros = 70'000'000;
    inventory.store->update_spool(
        spool.id, input, "matching-usd-price-edit");
    CHECK(
        inventory.store->job_cost_summary(eur_job.id)
            .material_cost_micros == 2'000'000);
    CHECK(
        inventory.store->job_cost_summary(usd_job.id)
            .material_cost_micros == 7'000'000);

    SpoolInput incompatible = input;
    incompatible.material_price_per_kg_micros = 30'000'000;
    incompatible.price_currency = "EUR";
    check_error_code(
        [&] {
            inventory.store->update_spool(
                spool.id, incompatible,
                "mixed-currency-change");
        },
        ErrorCode::conflict);
    CHECK(
        inventory.store->get_spool(spool.id)
            .material_price_per_kg_micros == 70'000'000);
}

TEST_CASE("reserving filament changes availability but not physical stock", "[FilamentInventory][Reservation]")
{
    TemporaryInventory inventory;
    const Spool spool = inventory.store->create_spool(spool_input("Reserved", 500'000));

    const PrintJob first = inventory.store->reserve_job(
        job_input("slice:duck"), {{spool.id, 0, 120'000}});
    CHECK(first.state == JobState::reserved);
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 500'000);
    CHECK(inventory.store->get_spool(spool.id).reserved_weight_mg == 120'000);
    CHECK(inventory.store->get_spool(spool.id).available_weight_mg == 380'000);

    const PrintJob repeated = inventory.store->reserve_job(
        job_input("slice:duck"), {{spool.id, 0, 120'000}});
    CHECK(repeated.id == first.id);
    CHECK(inventory.store->get_spool(spool.id).reserved_weight_mg == 120'000);

    const PrintJob resliced = inventory.store->reserve_job(
        job_input("slice:duck"), {{spool.id, 0, 150'000}});
    CHECK(resliced.id == first.id);
    CHECK(inventory.store->get_spool(spool.id).reserved_weight_mg == 150'000);

    const PrintJob trimmed = inventory.store->reserve_job(
        job_input("  slice:trimmed  "), {{spool.id, 1, 10'000}});
    const PrintJob trimmed_retry = inventory.store->reserve_job(
        job_input("slice:trimmed"), {{spool.id, 1, 10'000}});
    CHECK(trimmed_retry.id == trimmed.id);
}

TEST_CASE("reservation validation rolls the complete job back", "[FilamentInventory][Reservation]")
{
    TemporaryInventory inventory;
    const Spool enough = inventory.store->create_spool(spool_input("Enough", 300'000));
    const Spool short_ = inventory.store->create_spool(spool_input("Short", 10'000));

    check_error_code(
        [&] {
            inventory.store->reserve_job(
                job_input("slice:rollback"),
                {{enough.id, 0, 100'000}, {short_.id, 1, 20'000}});
        },
        ErrorCode::insufficient_stock);

    CHECK(inventory.store->list_open_jobs().empty());
    CHECK(inventory.store->get_spool(enough.id).reserved_weight_mg == 0);
    CHECK(inventory.store->get_spool(short_.id).reserved_weight_mg == 0);
}

TEST_CASE("committing a print consumes corrected weight exactly once", "[FilamentInventory][Reservation]")
{
    TemporaryInventory inventory;
    const Spool spool = inventory.store->create_spool(spool_input("Printed", 500'000));
    const PrintJob job = inventory.store->reserve_job(
        job_input("slice:commit"), {{spool.id, 0, 100'000}});

    inventory.store->mark_printing(job.id);
    const PrintJob printing = inventory.store->get_job(job.id);
    CHECK_FALSE(printing.started_at.empty());
    CHECK_FALSE(printing.actual_runtime_seconds);
    inventory.store->mark_printing(job.id);
    CHECK(inventory.store->get_job(job.id).started_at == printing.started_at);
    inventory.store->commit_job(job.id, {{0, 82'500}});

    const PrintJob completed = inventory.store->get_job(job.id);
    CHECK(completed.state == JobState::completed);
    REQUIRE(completed.actual_runtime_seconds);
    CHECK(*completed.actual_runtime_seconds >= 0);
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 417'500);
    CHECK(inventory.store->get_spool(spool.id).reserved_weight_mg == 0);

    inventory.store->commit_job(job.id, {{0, 82'500}});
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 417'500);
    check_error_code(
        [&] { inventory.store->commit_job(job.id, {{0, 80'000}}); },
        ErrorCode::conflict);
    check_error_code(
        [&] { inventory.store->commit_job(job.id); },
        ErrorCode::conflict);
    check_error_code(
        [&] { inventory.store->commit_job(job.id, {{0, 82'500}, {99, 1}}); },
        ErrorCode::validation);
    check_error_code([&] { inventory.store->discard_job(job.id); }, ErrorCode::conflict);

    const std::vector<PrintJob> history = inventory.store->list_jobs();
    REQUIRE(history.size() == 1);
    CHECK(history.front().state == JobState::completed);
    CHECK(inventory.store->list_open_jobs().empty());

    const std::vector<StockEvent> events = inventory.store->list_stock_events(spool.id);
    REQUIRE(events.size() == 2);
    CHECK(events.front().event_type == "consumption");
    CHECK(events.front().delta_mg == -82'500);
    REQUIRE(inventory.store->list_stock_events(spool.id, 1).size() == 1);
    REQUIRE(inventory.store->list_jobs(true, 1).size() == 1);
}

TEST_CASE("manual review freezes the observed print duration",
          "[FilamentInventory][Reservation][Runtime]")
{
    TemporaryInventory inventory;
    const Spool spool =
        inventory.store->create_spool(spool_input("Runtime review"));
    const PrintJob job = inventory.store->reserve_job(
        job_input("slice:runtime-review"),
        {{spool.id, 0, 50'000}});

    inventory.store->mark_printing(job.id);
    inventory.store->mark_needs_review(job.id);
    const PrintJob review = inventory.store->get_job(job.id);
    REQUIRE(review.actual_runtime_seconds);
    CHECK(*review.actual_runtime_seconds >= 0);

    inventory.store->mark_needs_review(job.id);
    CHECK(inventory.store->get_job(job.id).actual_runtime_seconds ==
          review.actual_runtime_seconds);
    inventory.store->commit_job(job.id);
    CHECK(inventory.store->get_job(job.id).actual_runtime_seconds ==
          review.actual_runtime_seconds);

    const PrintJob resumed = inventory.store->reserve_job(
        job_input("slice:runtime-resume", "Resumed runtime"),
        {{spool.id, 0, 25'000}});
    inventory.store->mark_printing(resumed.id);
    set_job_updated_at_seconds_ago(inventory.path, resumed.id, 60);
    inventory.store->mark_needs_review(resumed.id);
    const PrintJob first_segment = inventory.store->get_job(resumed.id);
    REQUIRE(first_segment.actual_runtime_seconds);
    CHECK(*first_segment.actual_runtime_seconds >= 58);
    CHECK(*first_segment.actual_runtime_seconds <= 61);

    set_job_updated_at_seconds_ago(inventory.path, resumed.id, 3'600);
    inventory.store->mark_printing(resumed.id);
    const PrintJob resumed_printing = inventory.store->get_job(resumed.id);
    CHECK(resumed_printing.started_at == first_segment.started_at);
    CHECK(
        resumed_printing.actual_runtime_seconds ==
        first_segment.actual_runtime_seconds);
    set_job_updated_at_seconds_ago(inventory.path, resumed.id, 10);
    inventory.store->commit_job(resumed.id);
    const PrintJob resumed_completed = inventory.store->get_job(resumed.id);
    REQUIRE(resumed_completed.actual_runtime_seconds);
    CHECK(
        *resumed_completed.actual_runtime_seconds >=
        *first_segment.actual_runtime_seconds + 8);
    CHECK(
        *resumed_completed.actual_runtime_seconds <=
        *first_segment.actual_runtime_seconds + 11);

    const PrintJob no_start = inventory.store->reserve_job(
        job_input("slice:no-runtime", "No runtime"),
        {{spool.id, 0, 25'000}});
    inventory.store->commit_job(no_start.id);
    CHECK_FALSE(
        inventory.store->get_job(no_start.id).actual_runtime_seconds);
}

TEST_CASE("discarding a print releases reservations without changing stock", "[FilamentInventory][Reservation]")
{
    TemporaryInventory inventory;
    SpoolInput spool_data = spool_input("Discarded", 250'000);
    spool_data.material_price_per_kg_micros = 20'000'000;
    const Spool spool = inventory.store->create_spool(spool_data);
    PrintJobInput print_data = job_input("slice:discard");
    print_data.estimated_runtime_seconds = 3'600;
    const PrintJob job = inventory.store->reserve_job(
        print_data, {{spool.id, 0, 75'000}});

    inventory.store->mark_needs_review(job.id);
    inventory.store->discard_job(job.id);
    inventory.store->discard_job(job.id);

    CHECK(inventory.store->get_job(job.id).state == JobState::discarded);
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 250'000);
    CHECK(inventory.store->get_spool(spool.id).reserved_weight_mg == 0);
    CHECK(inventory.store->job_cost_summary(job.id).total_cost_micros == 0);
}

TEST_CASE("external identifiers distinguish jobs with identical display names", "[FilamentInventory][Reservation]")
{
    TemporaryInventory inventory;
    const Spool first_spool  = inventory.store->create_spool(spool_input("First Job"));
    const Spool second_spool = inventory.store->create_spool(spool_input("Second Job"));
    const PrintJob first = inventory.store->reserve_job(
        job_input("slice:first", "Same name"), {{first_spool.id, 0, 10'000}});
    const PrintJob second = inventory.store->reserve_job(
        job_input("slice:second", "Same name"), {{second_spool.id, 0, 10'000}});

    inventory.store->bind_job_identifier(first.id, "bambu", "task_id", "task-123");
    const auto found = inventory.store->find_job("bambu", "task_id", "task-123");
    REQUIRE(found);
    CHECK(found->id == first.id);
    CHECK(found->id != second.id);

    check_error_code(
        [&] { inventory.store->bind_job_identifier(second.id, "bambu", "task_id", "task-123"); },
        ErrorCode::conflict);
}

TEST_CASE("manual stock corrections are journaled idempotently", "[FilamentInventory]")
{
    TemporaryInventory inventory;
    const Spool spool = inventory.store->create_spool(spool_input("Correction", 100'000));

    inventory.store->set_remaining(spool.id, 125'000, "scale:2026-07-23", "Measured on scale");
    inventory.store->set_remaining(spool.id, 125'000, "scale:2026-07-23", "Repeated delivery");
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 125'000);
    check_error_code(
        [&] { inventory.store->set_remaining(spool.id, 130'000, "scale:2026-07-23"); },
        ErrorCode::conflict);
    check_error_code(
        [&] { inventory.store->adjust_stock(spool.id, 25'000, "scale:2026-07-23"); },
        ErrorCode::conflict);

    inventory.store->adjust_stock(spool.id, -5'000, "sample:1", "Material sample");
    inventory.store->adjust_stock(spool.id, -5'000, "sample:1", "Repeated delivery");
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 120'000);
    check_error_code(
        [&] { inventory.store->adjust_stock(spool.id, -6'000, "sample:1"); },
        ErrorCode::conflict);

    check_error_code(
        [&] { inventory.store->adjust_stock(spool.id, -120'001, "below-zero:1"); },
        ErrorCode::insufficient_stock);
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 120'000);
}

TEST_CASE("spool edit fill levels use exact idempotent targets", "[FilamentInventory]")
{
    TemporaryInventory inventory;
    const Spool spool = inventory.store->create_spool(spool_input("Editable", 100'000));

    SpoolInput edited = spool_input("Edited", 125'000);
    inventory.store->update_spool(spool.id, edited, "edit-fill:1");
    inventory.store->adjust_stock(spool.id, -5'000, "later-use:1");

    inventory.store->update_spool(spool.id, edited, "edit-fill:1");
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 120'000);

    SpoolInput conflicting = edited;
    conflicting.name = "Must roll back";
    conflicting.initial_weight_mg = 145'000;
    check_error_code(
        [&] { inventory.store->update_spool(spool.id, conflicting, "edit-fill:1"); },
        ErrorCode::conflict);
    CHECK(inventory.store->get_spool(spool.id).name == "Edited");
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 120'000);

    const Spool unchanged =
        inventory.store->create_spool(spool_input("Metadata only", 100'000));
    SpoolInput metadata_only = spool_input("Renamed", 100'000);
    inventory.store->update_spool(unchanged.id, metadata_only, "metadata-edit:1");
    inventory.store->adjust_stock(unchanged.id, -20'000, "metadata-later-use:1");
    inventory.store->update_spool(unchanged.id, metadata_only, "metadata-edit:1");
    CHECK(inventory.store->get_spool(unchanged.id).current_weight_mg == 80'000);
}

TEST_CASE("corrected print consumption cannot make stock negative", "[FilamentInventory]")
{
    TemporaryInventory inventory;
    const Spool spool = inventory.store->create_spool(spool_input("Short", 50'000));
    const PrintJob job = inventory.store->reserve_job(
        job_input("slice:too-much"), {{spool.id, 0, 10'000}});

    check_error_code(
        [&] { inventory.store->commit_job(job.id, {{0, 50'001}}); },
        ErrorCode::insufficient_stock);
    CHECK(inventory.store->get_job(job.id).state == JobState::reserved);
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg == 50'000);
    CHECK(inventory.store->get_spool(spool.id).reserved_weight_mg == 10'000);
}

TEST_CASE("stock arithmetic rejects values outside the milligram range", "[FilamentInventory]")
{
    TemporaryInventory inventory;
    SpoolInput input = spool_input("Overflow");
    input.nominal_capacity_mg = std::numeric_limits<Milligrams>::max();
    input.initial_weight_mg   = std::numeric_limits<Milligrams>::max();
    const Spool spool = inventory.store->create_spool(input);

    check_error_code(
        [&] { inventory.store->adjust_stock(spool.id, 1, "overflow:1"); },
        ErrorCode::validation);
    CHECK(inventory.store->get_spool(spool.id).current_weight_mg ==
          std::numeric_limits<Milligrams>::max());
}
