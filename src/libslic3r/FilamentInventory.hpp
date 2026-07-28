#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r::FilamentInventory {

using Milligrams = std::int64_t;
// One unit is one millionth of the stored ISO-4217 currency unit.
// Keeping prices integral avoids rounding drift in cost histories.
using MoneyMicros = std::int64_t;

enum class ErrorCode {
    database,
    validation,
    not_found,
    conflict,
    insufficient_stock
};

class Error : public std::runtime_error
{
public:
    Error(ErrorCode code, const std::string &message);

    ErrorCode code() const noexcept { return m_code; }

private:
    ErrorCode m_code;
};

enum class WarningMode {
    none,
    grams,
    percent
};

enum class SpoolStatus {
    active,
    empty,
    archived
};

enum class IdentifierKind {
    quack_ndef_uuid,
    nfc_uid,
    bambu_tag_uid
};

enum class ColorModel {
    hex,
    rgb,
    cmyk,
    hsl,
    hsv
};

enum class JobState {
    reserved,
    printing,
    needs_review,
    completed,
    discarded
};

enum class CustomerOrderStatus {
    draft,
    active,
    completed,
    cancelled
};

struct InventorySettings {
    std::string  currency {"EUR"};
    MoneyMicros electricity_price_per_kwh_micros {400'000};
    std::int64_t default_machine_power_watts {150};
};

struct SpoolInput {
    std::string manufacturer;
    std::string material_type;
    std::string name;
    std::string filament_preset_id;
    std::string color_hex {"#FFFFFF"};
    double      diameter_mm {1.75};
    double      density_g_cm3 {1.24};
    Milligrams nominal_capacity_mg {1'000'000};
    Milligrams initial_weight_mg {1'000'000};
    WarningMode warning_mode {WarningMode::none};
    std::int64_t warning_value {0}; // mg for grams, basis points (0..10000) for percent
    MoneyMicros material_price_per_kg_micros {0};
    std::string price_currency {"EUR"};
};

struct Spool {
    std::string id;
    std::string manufacturer;
    std::string material_type;
    std::string name;
    std::string filament_preset_id;
    std::string color_hex;
    double      diameter_mm {0.0};
    double      density_g_cm3 {0.0};
    Milligrams nominal_capacity_mg {0};
    Milligrams current_weight_mg {0};
    Milligrams reserved_weight_mg {0};
    Milligrams available_weight_mg {0};
    WarningMode warning_mode {WarningMode::none};
    std::int64_t warning_value {0};
    SpoolStatus status {SpoolStatus::active};
    MoneyMicros material_price_per_kg_micros {0};
    std::string price_currency {"EUR"};
    std::string created_at;
    std::string updated_at;
};

struct SpoolIdentifier {
    IdentifierKind kind {IdentifierKind::quack_ndef_uuid};
    std::string    value;
    std::string    spool_id;
    std::string    created_at;
};

struct SpoolIdentifierInput {
    IdentifierKind kind {IdentifierKind::quack_ndef_uuid};
    std::string    value;
};

struct PrintJobInput {
    // Stable for one logical print. Reusing it updates a still-reserved job,
    // while repeat delivery after printing has started is idempotent.
    std::string idempotency_key;
    std::string job_name;
    std::string project_path;
    std::string printer_id;
    // Optional to keep personal/family prints independent of customer tracking.
    std::optional<std::string> customer_order_id;
    std::int64_t estimated_runtime_seconds {0};
    // Zero selects InventorySettings::default_machine_power_watts.
    std::int64_t machine_power_watts {0};
};

struct AllocationInput {
    std::string spool_id;
    int         filament_index {0};
    Milligrams estimated_weight_mg {0};
};

struct PrintJobUpdateInput {
    std::string job_name;
    std::string project_path;
    std::string printer_id;
    std::optional<std::string> customer_order_id;
    std::int64_t estimated_runtime_seconds {0};
    std::int64_t machine_power_watts {0};
    // The complete desired allocation set. Material assignments may only be
    // changed while the job is still reserved.
    std::vector<AllocationInput> allocations;
};

struct Allocation {
    std::string id;
    std::string job_id;
    std::string spool_id;
    // Historical snapshots keep completed jobs understandable when a spool is
    // renamed or its material metadata is corrected later.
    std::string spool_name;
    std::string manufacturer;
    std::string material_type;
    std::string filament_preset_id;
    std::string color_hex;
    int         filament_index {0};
    Milligrams estimated_weight_mg {0};
    std::optional<Milligrams> actual_weight_mg;
    // Price and calculated costs are resolved from the currently referenced
    // spool whenever currencies match. The spool UUID remains stable, while
    // price corrections are reflected in every compatible linked job. Legacy
    // cross-currency allocations retain their booked price.
    MoneyMicros material_price_per_kg_micros {0};
    std::string cost_currency {"EUR"};
    MoneyMicros estimated_material_cost_micros {0};
    std::optional<MoneyMicros> actual_material_cost_micros;
};

struct JobIdentifier {
    std::string provider;
    std::string kind;
    std::string value;
    std::string job_id;
    std::string created_at;
};

struct PrintJob {
    std::string id;
    std::string idempotency_key;
    std::string job_name;
    std::string project_path;
    std::string printer_id;
    std::optional<std::string> customer_order_id;
    JobState    state {JobState::reserved};
    std::string cost_currency {"EUR"};
    MoneyMicros electricity_price_per_kwh_micros {400'000};
    std::int64_t machine_power_watts {0};
    std::int64_t estimated_runtime_seconds {0};
    // Estimated from sliced runtime at reservation time.
    MoneyMicros electricity_cost_micros {0};
    std::string created_at;
    std::string updated_at;
    std::string started_at;
    std::string completed_at;
    std::optional<std::int64_t> actual_runtime_seconds;
    std::vector<Allocation> allocations;
};

struct CustomerInput {
    std::string name;
    std::string contact_name;
    std::string email;
    std::string phone;
    std::string notes;
};

struct Customer {
    std::string id;
    std::string name;
    std::string contact_name;
    std::string email;
    std::string phone;
    std::string notes;
    bool        archived {false};
    std::string created_at;
    std::string updated_at;
};

struct CustomerOrderInput {
    std::string customer_id;
    std::string order_number;
    std::string title;
    std::string notes;
    std::optional<MoneyMicros> quoted_price_micros;
    std::optional<MoneyMicros> invoice_amount_micros;
    std::string currency {"EUR"};
};

struct CustomerOrder {
    std::string id;
    std::string customer_id;
    std::string order_number;
    std::string title;
    std::string notes;
    std::optional<MoneyMicros> quoted_price_micros;
    std::optional<MoneyMicros> invoice_amount_micros;
    std::string currency {"EUR"};
    CustomerOrderStatus status {CustomerOrderStatus::draft};
    std::string created_at;
    std::string updated_at;
};

struct CostSummary {
    std::string currency {"EUR"};
    MoneyMicros estimated_material_cost_micros {0};
    std::optional<MoneyMicros> actual_material_cost_micros;
    // Uses actual allocation costs where known and estimates for open jobs.
    MoneyMicros material_cost_micros {0};
    MoneyMicros electricity_cost_micros {0};
    MoneyMicros total_cost_micros {0};
    std::optional<MoneyMicros> quoted_price_micros;
    std::optional<MoneyMicros> invoice_amount_micros;
};

struct MaterialUsageSummary {
    // These fields are allocation snapshots, not live spool metadata.
    // spool_name is only populated when no stable material identity is known.
    std::string spool_name;
    std::string manufacturer;
    std::string material_type;
    std::string filament_preset_id;
    std::string color_hex;
    std::string cost_currency {"EUR"};
    Milligrams estimated_weight_mg {0};
    // Confirmed values are used where present; open allocations retain their
    // sliced estimates. A confirmed zero remains a valid zero.
    Milligrams best_known_weight_mg {0};
    MoneyMicros estimated_material_cost_micros {0};
    MoneyMicros best_known_material_cost_micros {0};
    bool weight_fully_confirmed {true};
    bool cost_fully_confirmed {true};
};

struct ActualConsumption {
    int         filament_index {0};
    Milligrams weight_mg {0};
};

struct StockEvent {
    std::string id;
    std::string spool_id;
    std::string job_id;
    std::string allocation_id;
    std::string event_type;
    Milligrams delta_mg {0};
    Milligrams balance_after_mg {0};
    std::string operation_key;
    std::string note;
    std::string created_at;
};

class Store
{
public:
    static constexpr int schema_version = 5;

    // database_path is UTF-8. Parent directories must already exist.
    explicit Store(const std::string &database_path);
    ~Store();

    Store(Store &&) noexcept;
    Store &operator=(Store &&) noexcept;

    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    int current_schema_version() const;

    InventorySettings get_settings() const;
    InventorySettings update_settings(const InventorySettings &settings);

    Spool create_spool(const SpoolInput &input,
                       const std::vector<SpoolIdentifierInput> &identifiers = {});
    Spool update_spool(const std::string &spool_id, const SpoolInput &input,
                       const std::string &weight_operation_key);
    void  archive_spool(const std::string &spool_id);

    Spool                       get_spool(const std::string &spool_id) const;
    std::vector<Spool>          list_spools(bool include_archived = false) const;
    std::optional<Spool>        find_spool(IdentifierKind kind, const std::string &value) const;
    std::vector<SpoolIdentifier> list_identifiers(const std::string &spool_id) const;
    std::vector<SpoolIdentifier> list_identifiers() const;

    void bind_identifier(const std::string &spool_id, IdentifierKind kind, const std::string &value);
    void unbind_identifier(IdentifierKind kind, const std::string &value);
    // Atomically replaces only physical tag identifiers. Quack NDEF UUID aliases
    // remain stable so already written tags cannot silently stop resolving.
    void replace_physical_identifiers(
        const std::string &spool_id, const std::vector<SpoolIdentifierInput> &identifiers);

    void set_remaining(const std::string &spool_id, Milligrams remaining_mg,
                       const std::string &operation_key, const std::string &note = {});
    void adjust_stock(const std::string &spool_id, Milligrams delta_mg,
                      const std::string &operation_key, const std::string &note = {});
    std::vector<StockEvent> list_stock_events(
        const std::string &spool_id = {}, std::size_t limit = 0) const;

    Customer create_customer(const CustomerInput &input);
    Customer update_customer(const std::string &customer_id, const CustomerInput &input);
    void     archive_customer(const std::string &customer_id);
    Customer get_customer(const std::string &customer_id) const;
    std::vector<Customer> list_customers(bool include_archived = false) const;

    CustomerOrder create_customer_order(const CustomerOrderInput &input);
    CustomerOrder update_customer_order(
        const std::string &order_id, const CustomerOrderInput &input);
    void delete_customer_order(const std::string &order_id);
    void set_customer_order_status(
        const std::string &order_id, CustomerOrderStatus status);
    CustomerOrder get_customer_order(const std::string &order_id) const;
    std::vector<CustomerOrder> list_customer_orders(
        const std::string &customer_id = {}, bool include_closed = true) const;

    PrintJob reserve_job(const PrintJobInput &job, const std::vector<AllocationInput> &allocations);
    PrintJob update_print_job(
        const std::string &job_id, const PrintJobUpdateInput &input);
    PrintJob get_job(const std::string &job_id) const;
    std::vector<PrintJob> list_jobs(bool include_closed = true, std::size_t limit = 0) const;
    std::vector<PrintJob> list_open_jobs() const;
    std::vector<PrintJob> list_customer_order_jobs(
        const std::string &order_id, bool include_discarded = true) const;

    void bind_job_identifier(const std::string &job_id, const std::string &provider,
                             const std::string &kind, const std::string &value);
    std::optional<PrintJob> find_job(const std::string &provider, const std::string &kind,
                                     const std::string &value) const;

    void mark_printing(const std::string &job_id);
    void mark_needs_review(const std::string &job_id);
    void commit_job(const std::string &job_id,
                    const std::vector<ActualConsumption> &actual_consumption = {});
    void discard_job(const std::string &job_id);

    CostSummary job_cost_summary(const std::string &job_id) const;
    CostSummary customer_order_cost_summary(const std::string &order_id) const;
    CostSummary customer_cost_summary(const std::string &customer_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

std::string to_string(WarningMode value);
std::string to_string(SpoolStatus value);
std::string to_string(IdentifierKind value);
std::string to_string(JobState value);
std::string to_string(ColorModel value);
std::string to_string(CustomerOrderStatus value);

// Aggregates multiple physical spools of the same snapshotted filament while
// keeping manufacturer, preset, colour and currency boundaries intact.
// Discarded print jobs are excluded.
std::vector<MaterialUsageSummary> summarize_material_usage(
    const std::vector<PrintJob> &jobs);

// Converts a user-entered color to the canonical sRGB #RRGGBB representation.
// CMYK conversion is device-independent and therefore an approximation.
std::string canonical_color(ColorModel model, const std::string &value);

} // namespace Slic3r::FilamentInventory
