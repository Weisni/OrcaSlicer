#include <catch2/catch_all.hpp>

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
        CHECK(store.current_schema_version() == 2);
        const InventorySettings settings = store.get_settings();
        CHECK(settings.currency == "EUR");
        CHECK(settings.electricity_price_per_kwh_micros == 400'000);
        CHECK(settings.default_machine_power_watts == 150);

        const Spool legacy = store.get_spool("legacy-spool");
        CHECK(legacy.current_weight_mg == 500'000);
        CHECK(legacy.material_price_per_kg_micros == 0);
        CHECK(legacy.price_currency == "EUR");
    }

    boost::system::error_code cleanup_error;
    fs::remove(path, cleanup_error);
    fs::remove(path.string() + "-wal", cleanup_error);
    fs::remove(path.string() + "-shm", cleanup_error);
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
    CHECK(inventory.store->create_spool(input).price_currency == "USD");
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

TEST_CASE("job cost snapshots ignore later price changes", "[FilamentInventory][Cost]")
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

    input.material_price_per_kg_micros = 50'000'000;
    input.initial_weight_mg = spool.current_weight_mg;
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
    CHECK(repeated.allocations.front().material_price_per_kg_micros == 20'000'000);

    inventory.store->commit_job(reserved.id, {{0, 80'000}});
    const CostSummary summary = inventory.store->job_cost_summary(reserved.id);
    REQUIRE(summary.actual_material_cost_micros);
    CHECK(*summary.actual_material_cost_micros == 1'600'000);
    CHECK(summary.total_cost_micros == 1'660'000);
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
    inventory.store->commit_job(job.id, {{0, 82'500}});

    CHECK(inventory.store->get_job(job.id).state == JobState::completed);
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
