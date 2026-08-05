#include "FilamentInventory.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace Slic3r::FilamentInventory {

Error::Error(ErrorCode code, const std::string &message) : std::runtime_error(message), m_code(code) {}

std::string to_string(WarningMode value)
{
    switch (value) {
    case WarningMode::none:    return "none";
    case WarningMode::grams:   return "grams";
    case WarningMode::percent: return "percent";
    }
    return "none";
}

std::string to_string(SpoolStatus value)
{
    switch (value) {
    case SpoolStatus::active:   return "active";
    case SpoolStatus::empty:    return "empty";
    case SpoolStatus::archived: return "archived";
    }
    return "active";
}

std::string to_string(IdentifierKind value)
{
    switch (value) {
    case IdentifierKind::quack_ndef_uuid: return "quack_ndef_uuid";
    case IdentifierKind::nfc_uid:         return "nfc_uid";
    case IdentifierKind::bambu_tag_uid:   return "bambu_tag_uid";
    }
    return "quack_ndef_uuid";
}

std::string to_string(JobState value)
{
    switch (value) {
    case JobState::reserved:     return "reserved";
    case JobState::printing:     return "printing";
    case JobState::needs_review: return "needs_review";
    case JobState::completed:    return "completed";
    case JobState::discarded:    return "discarded";
    }
    return "reserved";
}

std::string to_string(ColorModel value)
{
    switch (value) {
    case ColorModel::hex:  return "HEX";
    case ColorModel::rgb:  return "RGB";
    case ColorModel::cmyk: return "CMYK";
    case ColorModel::hsl:  return "HSL";
    case ColorModel::hsv:  return "HSV";
    }
    return "HEX";
}

std::string to_string(CustomerOrderStatus value)
{
    switch (value) {
    case CustomerOrderStatus::draft:     return "draft";
    case CustomerOrderStatus::active:    return "active";
    case CustomerOrderStatus::completed: return "completed";
    case CustomerOrderStatus::cancelled: return "cancelled";
    }
    return "draft";
}

namespace {

WarningMode warning_mode_from_string(const std::string &value)
{
    if (value == "none")    return WarningMode::none;
    if (value == "grams")   return WarningMode::grams;
    if (value == "percent") return WarningMode::percent;
    throw Error(ErrorCode::database, "Unknown warning mode in filament inventory: " + value);
}

SpoolStatus spool_status_from_string(const std::string &value)
{
    if (value == "active")   return SpoolStatus::active;
    if (value == "empty")    return SpoolStatus::empty;
    if (value == "archived") return SpoolStatus::archived;
    throw Error(ErrorCode::database, "Unknown spool status in filament inventory: " + value);
}

IdentifierKind identifier_kind_from_string(const std::string &value)
{
    if (value == "quack_ndef_uuid") return IdentifierKind::quack_ndef_uuid;
    if (value == "nfc_uid")         return IdentifierKind::nfc_uid;
    if (value == "bambu_tag_uid")   return IdentifierKind::bambu_tag_uid;
    throw Error(ErrorCode::database, "Unknown identifier kind in filament inventory: " + value);
}

JobState job_state_from_string(const std::string &value)
{
    if (value == "reserved")     return JobState::reserved;
    if (value == "printing")     return JobState::printing;
    if (value == "needs_review") return JobState::needs_review;
    if (value == "completed")    return JobState::completed;
    if (value == "discarded")    return JobState::discarded;
    throw Error(ErrorCode::database, "Unknown print-job state in filament inventory: " + value);
}

CustomerOrderStatus customer_order_status_from_string(const std::string &value)
{
    if (value == "draft")     return CustomerOrderStatus::draft;
    if (value == "active")    return CustomerOrderStatus::active;
    if (value == "completed") return CustomerOrderStatus::completed;
    if (value == "cancelled") return CustomerOrderStatus::cancelled;
    throw Error(ErrorCode::database, "Unknown customer-order status in filament inventory: " + value);
}

std::string make_uuid()
{
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

std::string trim_copy(const std::string &value)
{
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    auto last  = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    return first < last ? std::string(first, last) : std::string();
}

std::string normalize_color(std::string value)
{
    value = trim_copy(value);
    if (value.size() == 6)
        value.insert(value.begin(), '#');
    if (value.size() != 7 || value.front() != '#')
        throw Error(ErrorCode::validation, "Filament color must use canonical #RRGGBB notation");
    for (size_t i = 1; i < value.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            throw Error(ErrorCode::validation, "Filament color contains a non-hexadecimal character");
        value[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
    }
    return value;
}

template<size_t Count>
std::array<double, Count> parse_color_components(std::string value, const char *model)
{
    for (char &ch : value) {
        if (ch == ',' || ch == ';' || ch == '/')
            ch = ' ';
    }
    value.erase(std::remove(value.begin(), value.end(), '%'), value.end());

    std::istringstream stream(value);
    stream.imbue(std::locale::classic());
    std::array<double, Count> result {};
    for (double &component : result) {
        if (!(stream >> component) || !std::isfinite(component))
            throw Error(ErrorCode::validation, std::string(model) + " color has invalid components");
    }
    stream >> std::ws;
    if (!stream.eof())
        throw Error(ErrorCode::validation, std::string(model) + " color has too many components");
    return result;
}

void require_color_range(double value, double minimum, double maximum, const char *component)
{
    if (value < minimum || value > maximum) {
        std::ostringstream message;
        message << component << " must be between " << minimum << " and " << maximum;
        throw Error(ErrorCode::validation, message.str());
    }
}

std::string rgb_to_hex(double red, double green, double blue)
{
    const auto channel = [](double value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0, 255.0)));
    };
    std::ostringstream result;
    result << '#' << std::uppercase << std::hex << std::setfill('0')
           << std::setw(2) << channel(red)
           << std::setw(2) << channel(green)
           << std::setw(2) << channel(blue);
    return result.str();
}

double hsl_channel(double p, double q, double t)
{
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

std::string normalize_identifier(IdentifierKind kind, std::string value)
{
    value = trim_copy(value);
    if (value.empty())
        throw Error(ErrorCode::validation, "Spool identifier must not be empty");

    if (kind == IdentifierKind::quack_ndef_uuid) {
        try {
            return boost::uuids::to_string(boost::uuids::string_generator()(value));
        } catch (const std::exception &) {
            throw Error(ErrorCode::validation, "Quack NFC payload must contain a valid UUID");
        }
    }

    std::string compact;
    compact.reserve(value.size());
    for (unsigned char ch : value) {
        if (ch == ':' || ch == '-' || std::isspace(ch))
            continue;
        compact.push_back(static_cast<char>(std::toupper(ch)));
    }
    if (!compact.empty() &&
        std::all_of(compact.begin(), compact.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
        return compact;

    return value;
}

void validate_spool_input(const SpoolInput &input)
{
    if (trim_copy(input.material_type).empty())
        throw Error(ErrorCode::validation, "Material type must not be empty");
    if (trim_copy(input.name).empty())
        throw Error(ErrorCode::validation, "Spool name must not be empty");
    if (!std::isfinite(input.diameter_mm) || input.diameter_mm <= 0.0)
        throw Error(ErrorCode::validation, "Filament diameter must be positive");
    if (!std::isfinite(input.density_g_cm3) || input.density_g_cm3 <= 0.0)
        throw Error(ErrorCode::validation, "Filament density must be positive");
    if (input.nominal_capacity_mg <= 0)
        throw Error(ErrorCode::validation, "Nominal spool capacity must be positive");
    if (input.initial_weight_mg < 0)
        throw Error(ErrorCode::validation, "Remaining filament weight must not be negative");
    (void) normalize_color(input.color_hex);

    if (input.warning_value < 0)
        throw Error(ErrorCode::validation, "Warning threshold must not be negative");
    if (input.warning_mode == WarningMode::percent && input.warning_value > 10'000)
        throw Error(ErrorCode::validation, "Percentage warning threshold must be between 0 and 100 percent");
    if (input.material_price_per_kg_micros < 0)
        throw Error(ErrorCode::validation, "Filament price must not be negative");
}

std::string normalize_currency(std::string currency)
{
    currency = trim_copy(currency);
    if (currency.size() != 3)
        throw Error(ErrorCode::validation, "Currency must be a three-letter ISO code");
    for (char &ch : currency) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalpha(value))
            throw Error(ErrorCode::validation, "Currency must be a three-letter ISO code");
        ch = static_cast<char>(std::toupper(value));
    }
    return currency;
}

void validate_settings(const InventorySettings &settings)
{
    (void) normalize_currency(settings.currency);
    if (settings.electricity_price_per_kwh_micros < 0)
        throw Error(ErrorCode::validation, "Electricity price must not be negative");
    if (settings.default_machine_power_watts <= 0)
        throw Error(ErrorCode::validation, "Default machine power must be positive");
    if (settings.machine_wear_per_hour_micros < 0 ||
        settings.maintenance_per_hour_micros < 0 ||
        settings.repair_reserve_per_hour_micros < 0 ||
        settings.design_per_hour_micros < 0)
        throw Error(ErrorCode::validation, "Hourly cost rates must not be negative");
}

void validate_customer_input(const CustomerInput &input)
{
    if (trim_copy(input.name).empty())
        throw Error(ErrorCode::validation, "Customer name must not be empty");
}

void validate_customer_order_input(const CustomerOrderInput &input)
{
    if (trim_copy(input.customer_id).empty())
        throw Error(ErrorCode::validation, "Customer order must reference a customer");
    if (trim_copy(input.title).empty())
        throw Error(ErrorCode::validation, "Customer order title must not be empty");
    if (input.quoted_price_micros && *input.quoted_price_micros < 0)
        throw Error(ErrorCode::validation, "Quoted price must not be negative");
    if (input.invoice_amount_micros && *input.invoice_amount_micros < 0)
        throw Error(ErrorCode::validation, "Invoice amount must not be negative");
    if (input.design_time_seconds < 0 || input.design_hourly_rate_micros < 0 ||
        input.other_cost_micros < 0)
        throw Error(ErrorCode::validation, "Order costs must not be negative");
    if (input.discount_basis_points < 0 || input.discount_basis_points > 10'000)
        throw Error(ErrorCode::validation, "Discount must be between 0 and 100 percent");
    (void) normalize_currency(input.currency);
}

MoneyMicros scaled_cost(const std::vector<std::int64_t> &factors, std::int64_t divisor,
                        const char *context)
{
    if (divisor <= 0)
        throw Error(ErrorCode::validation, std::string(context) + " has an invalid divisor");
    boost::multiprecision::cpp_int value = 1;
    for (const std::int64_t factor : factors) {
        if (factor < 0)
            throw Error(ErrorCode::validation, std::string(context) + " must not be negative");
        value *= factor;
    }
    value /= divisor;
    if (value > std::numeric_limits<MoneyMicros>::max())
        throw Error(ErrorCode::validation, std::string(context) + " exceeds the supported money range");
    return value.convert_to<MoneyMicros>();
}

MoneyMicros material_cost(MoneyMicros price_per_kg_micros, Milligrams weight_mg)
{
    if (price_per_kg_micros < 0 || weight_mg < 0)
        throw Error(ErrorCode::validation, "Material cost must not be negative");
    if (weight_mg == 0)
        return 0;

    // Material costs are booked per allocation in the same two-decimal unit
    // shown throughout the UI. This keeps the sum of visible line items equal
    // to the job total and applies the business minimum of one cent whenever
    // a positive amount of material is used.
    constexpr MoneyMicros micros_per_cent = 10'000;
    const boost::multiprecision::cpp_int cent_divisor =
        boost::multiprecision::cpp_int(1'000'000) * micros_per_cent;
    const boost::multiprecision::cpp_int product =
        boost::multiprecision::cpp_int(price_per_kg_micros) * weight_mg;
    boost::multiprecision::cpp_int cents =
        (product + cent_divisor / 2) / cent_divisor;
    if (cents == 0)
        cents = 1;
    const boost::multiprecision::cpp_int value = cents * micros_per_cent;
    if (value > std::numeric_limits<MoneyMicros>::max())
        throw Error(
            ErrorCode::validation,
            "Material cost exceeds the supported money range");
    return value.convert_to<MoneyMicros>();
}

MoneyMicros electricity_cost(MoneyMicros price_per_kwh_micros,
                             std::int64_t power_watts, std::int64_t runtime_seconds)
{
    return scaled_cost(
        {price_per_kwh_micros, power_watts, runtime_seconds},
        3'600'000,
        "Electricity cost");
}

std::string to_string(InvoiceCostCategory value)
{
    switch (value) {
    case InvoiceCostCategory::material:       return "material";
    case InvoiceCostCategory::electricity:    return "electricity";
    case InvoiceCostCategory::machine_wear:   return "machine_wear";
    case InvoiceCostCategory::maintenance:    return "maintenance";
    case InvoiceCostCategory::repair_reserve: return "repair_reserve";
    case InvoiceCostCategory::design:         return "design";
    case InvoiceCostCategory::other:          return "other";
    case InvoiceCostCategory::discount:       return "discount";
    }
    return "other";
}

MoneyMicros hourly_cost(MoneyMicros rate_per_hour_micros,
                        std::int64_t runtime_seconds, const char *context)
{
    return scaled_cost({rate_per_hour_micros, runtime_seconds}, 3'600, context);
}

void validate_operation_key(const std::string &value)
{
    if (trim_copy(value).empty())
        throw Error(ErrorCode::validation, "An idempotency key is required for stock changes");
}

Milligrams checked_add(Milligrams lhs, Milligrams rhs, const char *context)
{
    if ((rhs > 0 && lhs > std::numeric_limits<Milligrams>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<Milligrams>::min() - rhs))
        throw Error(ErrorCode::validation, std::string(context) + " exceeds the supported inventory range");
    return lhs + rhs;
}

Milligrams checked_subtract(Milligrams lhs, Milligrams rhs, const char *context)
{
    if ((rhs > 0 && lhs < std::numeric_limits<Milligrams>::min() + rhs) ||
        (rhs < 0 && lhs > std::numeric_limits<Milligrams>::max() + rhs))
        throw Error(ErrorCode::validation, std::string(context) + " exceeds the supported inventory range");
    return lhs - rhs;
}

[[noreturn]] void throw_sqlite(sqlite3 *db, int result, const std::string &context)
{
    std::ostringstream message;
    message << context << " (SQLite " << result << ")";
    if (db != nullptr)
        message << ": " << sqlite3_errmsg(db);
    throw Error(ErrorCode::database, message.str());
}

void exec_sql(sqlite3 *db, const char *sql)
{
    char *error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (result == SQLITE_OK)
        return;
    const std::string detail = error != nullptr ? error : "unknown SQLite error";
    sqlite3_free(error);
    throw Error(ErrorCode::database, "Filament inventory database command failed: " + detail);
}

void exec_sql_retrying_busy(sqlite3 *db, const char *sql, std::chrono::milliseconds retry_window)
{
    const auto deadline = std::chrono::steady_clock::now() + retry_window;
    int        delay_ms = 1;

    for (;;) {
        char     *error  = nullptr;
        const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
        if (result == SQLITE_OK)
            return;

        const std::string detail = error != nullptr ? error : "unknown SQLite error";
        sqlite3_free(error);

        // SQLite may deliberately skip the configured busy handler when it
        // detects a lock-upgrade deadlock. Retrying the complete command lets
        // the competing connection finish without extending normal lock waits.
        if ((result & 0xff) != SQLITE_BUSY || std::chrono::steady_clock::now() >= deadline)
            throw Error(ErrorCode::database, "Filament inventory database command failed: " + detail);

        sqlite3_sleep(delay_ms);
        delay_ms = std::min(delay_ms * 2, 25);
    }
}

class Statement
{
public:
    Statement(sqlite3 *db, const char *sql) : m_db(db)
    {
        const int result = sqlite3_prepare_v2(db, sql, -1, &m_statement, nullptr);
        if (result != SQLITE_OK)
            throw_sqlite(db, result, "Could not prepare filament inventory query");
    }

    ~Statement() { sqlite3_finalize(m_statement); }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    void bind(int index, const std::string &value)
    {
        const int result = sqlite3_bind_text(m_statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        if (result != SQLITE_OK)
            throw_sqlite(m_db, result, "Could not bind text to filament inventory query");
    }

    void bind(int index, int value)
    {
        const int result = sqlite3_bind_int(m_statement, index, value);
        if (result != SQLITE_OK)
            throw_sqlite(m_db, result, "Could not bind integer to filament inventory query");
    }

    void bind(int index, std::int64_t value)
    {
        const int result = sqlite3_bind_int64(m_statement, index, value);
        if (result != SQLITE_OK)
            throw_sqlite(m_db, result, "Could not bind 64-bit integer to filament inventory query");
    }

    void bind(int index, double value)
    {
        const int result = sqlite3_bind_double(m_statement, index, value);
        if (result != SQLITE_OK)
            throw_sqlite(m_db, result, "Could not bind number to filament inventory query");
    }

    void bind_null(int index)
    {
        const int result = sqlite3_bind_null(m_statement, index);
        if (result != SQLITE_OK)
            throw_sqlite(m_db, result, "Could not bind NULL to filament inventory query");
    }

    bool step()
    {
        const int result = sqlite3_step(m_statement);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        throw_sqlite(m_db, result, "Could not execute filament inventory query");
    }

    void execute()
    {
        if (step())
            throw Error(ErrorCode::database, "Filament inventory command unexpectedly returned a row");
    }

    std::string text(int column) const
    {
        const unsigned char *value = sqlite3_column_text(m_statement, column);
        return value != nullptr ? reinterpret_cast<const char *>(value) : std::string();
    }

    int integer(int column) const { return sqlite3_column_int(m_statement, column); }
    std::int64_t integer64(int column) const { return sqlite3_column_int64(m_statement, column); }
    double number(int column) const { return sqlite3_column_double(m_statement, column); }
    bool is_null(int column) const { return sqlite3_column_type(m_statement, column) == SQLITE_NULL; }

private:
    sqlite3      *m_db {nullptr};
    sqlite3_stmt *m_statement {nullptr};
};

class Transaction
{
public:
    explicit Transaction(sqlite3 *db) : m_db(db) { exec_sql(m_db, "BEGIN IMMEDIATE"); }

    ~Transaction()
    {
        if (m_active)
            sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    void commit()
    {
        exec_sql(m_db, "COMMIT");
        m_active = false;
    }

private:
    sqlite3 *m_db {nullptr};
    bool     m_active {true};
};

struct StockEventRecord {
    std::string spool_id;
    std::string job_id;
    std::string allocation_id;
    std::string event_type;
    Milligrams delta_mg {0};
    Milligrams balance_after_mg {0};
};

} // namespace

std::string canonical_color(ColorModel model, const std::string &value)
{
    if (model == ColorModel::hex)
        return normalize_color(value);

    if (model == ColorModel::rgb) {
        const auto rgb = parse_color_components<3>(value, "RGB");
        for (size_t index = 0; index < rgb.size(); ++index)
            require_color_range(rgb[index], 0.0, 255.0, index == 0 ? "Red" : index == 1 ? "Green" : "Blue");
        return rgb_to_hex(rgb[0], rgb[1], rgb[2]);
    }

    if (model == ColorModel::cmyk) {
        const auto cmyk = parse_color_components<4>(value, "CMYK");
        static constexpr const char *names[] = {"Cyan", "Magenta", "Yellow", "Black"};
        for (size_t index = 0; index < cmyk.size(); ++index)
            require_color_range(cmyk[index], 0.0, 100.0, names[index]);
        const double cyan    = cmyk[0] / 100.0;
        const double magenta = cmyk[1] / 100.0;
        const double yellow  = cmyk[2] / 100.0;
        const double black   = cmyk[3] / 100.0;
        return rgb_to_hex(
            255.0 * (1.0 - cyan) * (1.0 - black),
            255.0 * (1.0 - magenta) * (1.0 - black),
            255.0 * (1.0 - yellow) * (1.0 - black));
    }

    const auto components = parse_color_components<3>(value, model == ColorModel::hsl ? "HSL" : "HSV");
    require_color_range(components[1], 0.0, 100.0, "Saturation");
    require_color_range(components[2], 0.0, 100.0, model == ColorModel::hsl ? "Lightness" : "Value");
    double hue = std::fmod(components[0], 360.0);
    if (hue < 0.0)
        hue += 360.0;

    if (model == ColorModel::hsl) {
        const double saturation = components[1] / 100.0;
        const double lightness  = components[2] / 100.0;
        if (saturation == 0.0)
            return rgb_to_hex(lightness * 255.0, lightness * 255.0, lightness * 255.0);
        const double q = lightness < 0.5 ?
                         lightness * (1.0 + saturation) :
                         lightness + saturation - lightness * saturation;
        const double p = 2.0 * lightness - q;
        const double normalized_hue = hue / 360.0;
        return rgb_to_hex(
            hsl_channel(p, q, normalized_hue + 1.0 / 3.0) * 255.0,
            hsl_channel(p, q, normalized_hue) * 255.0,
            hsl_channel(p, q, normalized_hue - 1.0 / 3.0) * 255.0);
    }

    const double saturation = components[1] / 100.0;
    const double brightness = components[2] / 100.0;
    const double chroma = brightness * saturation;
    const double x = chroma * (1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0));
    const double match = brightness - chroma;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (hue < 60.0) {
        red = chroma; green = x;
    } else if (hue < 120.0) {
        red = x; green = chroma;
    } else if (hue < 180.0) {
        green = chroma; blue = x;
    } else if (hue < 240.0) {
        green = x; blue = chroma;
    } else if (hue < 300.0) {
        red = x; blue = chroma;
    } else {
        red = chroma; blue = x;
    }
    return rgb_to_hex((red + match) * 255.0, (green + match) * 255.0, (blue + match) * 255.0);
}

struct Store::Impl
{
    explicit Impl(const std::string &database_path)
    {
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        int result = sqlite3_open_v2(database_path.c_str(), &db, flags, nullptr);
        if (result != SQLITE_OK) {
            const std::string detail = db != nullptr ? sqlite3_errmsg(db) : "could not allocate SQLite handle";
            if (db != nullptr)
                sqlite3_close_v2(db);
            db = nullptr;
            throw Error(ErrorCode::database, "Could not open filament inventory database: " + detail);
        }

        try {
            sqlite3_extended_result_codes(db, 1);
            result = sqlite3_busy_timeout(db, 5'000);
            if (result != SQLITE_OK)
                throw_sqlite(db, result, "Could not configure filament inventory busy timeout");

            exec_sql(db, "PRAGMA foreign_keys = ON");
            exec_sql_retrying_busy(db, "PRAGMA journal_mode = WAL", std::chrono::seconds(1));
            exec_sql(db, "PRAGMA synchronous = NORMAL");
            migrate();
        } catch (...) {
            sqlite3_close_v2(db);
            db = nullptr;
            throw;
        }
    }

    ~Impl()
    {
        if (db != nullptr)
            sqlite3_close_v2(db);
    }

    int current_schema_version_unlocked() const
    {
        Statement statement(db, "PRAGMA user_version");
        if (!statement.step())
            throw Error(ErrorCode::database, "Could not read filament inventory schema version");
        return statement.integer(0);
    }

    void migrate()
    {
        // Serialize first-use migration across processes/connections, then read
        // the version inside the write transaction so a waiter observes the
        // schema created by the connection that acquired the lock first.
        Transaction transaction(db);
        const int version = current_schema_version_unlocked();
        if (version > Store::schema_version)
            throw Error(ErrorCode::database, "Filament inventory was created by a newer QuackSlicer version");
        if (version == Store::schema_version) {
            transaction.commit();
            return;
        }

        if (version < 1) {
            exec_sql(db, R"SQL(
                CREATE TABLE spools (
                    id                    TEXT PRIMARY KEY,
                    manufacturer          TEXT NOT NULL DEFAULT '',
                    material_type         TEXT NOT NULL,
                    name                  TEXT NOT NULL,
                    filament_preset_id    TEXT NOT NULL DEFAULT '',
                    color_hex             TEXT NOT NULL,
                    diameter_mm           REAL NOT NULL CHECK (diameter_mm > 0),
                    density_g_cm3         REAL NOT NULL CHECK (density_g_cm3 > 0),
                    nominal_capacity_mg   INTEGER NOT NULL CHECK (nominal_capacity_mg > 0),
                    warning_mode          TEXT NOT NULL CHECK (warning_mode IN ('none', 'grams', 'percent')),
                    warning_value         INTEGER NOT NULL CHECK (warning_value >= 0),
                    status                TEXT NOT NULL CHECK (status IN ('active', 'empty', 'archived')),
                    created_at            TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                    updated_at            TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
                );

                CREATE TABLE spool_identifiers (
                    kind        TEXT NOT NULL CHECK (kind IN ('quack_ndef_uuid', 'nfc_uid', 'bambu_tag_uid')),
                    value       TEXT NOT NULL,
                    spool_id    TEXT NOT NULL REFERENCES spools(id) ON DELETE CASCADE,
                    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                    PRIMARY KEY (kind, value)
                );
                CREATE INDEX spool_identifiers_spool_idx ON spool_identifiers(spool_id);

                CREATE TABLE print_jobs (
                    id                TEXT PRIMARY KEY,
                    idempotency_key   TEXT NOT NULL UNIQUE,
                    job_name          TEXT NOT NULL,
                    project_path      TEXT NOT NULL DEFAULT '',
                    printer_id        TEXT NOT NULL DEFAULT '',
                    state             TEXT NOT NULL CHECK (state IN ('reserved', 'printing', 'needs_review', 'completed', 'discarded')),
                    created_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                    updated_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                    completed_at      TEXT NOT NULL DEFAULT ''
                );

                CREATE TABLE job_identifiers (
                    provider    TEXT NOT NULL,
                    kind        TEXT NOT NULL,
                    value       TEXT NOT NULL,
                    job_id      TEXT NOT NULL REFERENCES print_jobs(id) ON DELETE CASCADE,
                    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                    PRIMARY KEY (provider, kind, value)
                );
                CREATE INDEX job_identifiers_job_idx ON job_identifiers(job_id);

                CREATE TABLE allocations (
                    id                    TEXT PRIMARY KEY,
                    job_id                TEXT NOT NULL REFERENCES print_jobs(id) ON DELETE CASCADE,
                    spool_id              TEXT NOT NULL REFERENCES spools(id) ON DELETE RESTRICT,
                    filament_index        INTEGER NOT NULL CHECK (filament_index >= 0),
                    estimated_weight_mg   INTEGER NOT NULL CHECK (estimated_weight_mg > 0),
                    actual_weight_mg      INTEGER CHECK (actual_weight_mg >= 0),
                    UNIQUE (job_id, filament_index)
                );
                CREATE INDEX allocations_spool_idx ON allocations(spool_id);

                CREATE TABLE stock_events (
                    id                TEXT PRIMARY KEY,
                    spool_id          TEXT NOT NULL REFERENCES spools(id) ON DELETE RESTRICT,
                    job_id            TEXT REFERENCES print_jobs(id) ON DELETE RESTRICT,
                    allocation_id     TEXT REFERENCES allocations(id) ON DELETE RESTRICT,
                    event_type        TEXT NOT NULL CHECK (event_type IN ('initial', 'set_remaining', 'adjustment', 'refill', 'consumption')),
                    delta_mg          INTEGER NOT NULL,
                    balance_after_mg  INTEGER NOT NULL,
                    operation_key     TEXT NOT NULL UNIQUE,
                    note              TEXT NOT NULL DEFAULT '',
                    created_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
                );
                CREATE INDEX stock_events_spool_idx ON stock_events(spool_id, created_at);

                PRAGMA user_version = 1;
            )SQL");
        }
        if (version < 2) {
            exec_sql(db, R"SQL(
                CREATE TABLE inventory_settings (
                    id                                  INTEGER PRIMARY KEY CHECK (id = 1),
                    currency                            TEXT NOT NULL,
                    electricity_price_per_kwh_micros    INTEGER NOT NULL CHECK (electricity_price_per_kwh_micros >= 0),
                    default_machine_power_watts         INTEGER NOT NULL CHECK (default_machine_power_watts > 0),
                    updated_at                          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
                );
                INSERT INTO inventory_settings (
                    id, currency, electricity_price_per_kwh_micros,
                    default_machine_power_watts
                ) VALUES (1, 'EUR', 400000, 200);

                CREATE TABLE customers (
                    id            TEXT PRIMARY KEY,
                    name          TEXT NOT NULL,
                    contact_name  TEXT NOT NULL DEFAULT '',
                    email         TEXT NOT NULL DEFAULT '',
                    phone         TEXT NOT NULL DEFAULT '',
                    notes         TEXT NOT NULL DEFAULT '',
                    archived      INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0, 1)),
                    created_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                    updated_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
                );
                CREATE INDEX customers_name_idx ON customers(name COLLATE NOCASE);

                CREATE TABLE customer_orders (
                    id                      TEXT PRIMARY KEY,
                    customer_id             TEXT NOT NULL REFERENCES customers(id) ON DELETE RESTRICT,
                    order_number            TEXT NOT NULL DEFAULT '',
                    title                   TEXT NOT NULL,
                    notes                   TEXT NOT NULL DEFAULT '',
                    quoted_price_micros      INTEGER CHECK (quoted_price_micros >= 0),
                    invoice_amount_micros   INTEGER CHECK (invoice_amount_micros >= 0),
                    currency                TEXT NOT NULL,
                    status                  TEXT NOT NULL CHECK (status IN ('draft', 'active', 'completed', 'cancelled')),
                    created_at              TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                    updated_at              TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
                );
                CREATE INDEX customer_orders_customer_idx
                    ON customer_orders(customer_id, created_at);

                ALTER TABLE spools
                    ADD COLUMN material_price_per_kg_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (material_price_per_kg_micros >= 0);
                ALTER TABLE spools
                    ADD COLUMN price_currency TEXT NOT NULL DEFAULT 'EUR';

                ALTER TABLE print_jobs
                    ADD COLUMN customer_order_id TEXT REFERENCES customer_orders(id) ON DELETE SET NULL;
                ALTER TABLE print_jobs
                    ADD COLUMN cost_currency TEXT NOT NULL DEFAULT 'EUR';
                ALTER TABLE print_jobs
                    ADD COLUMN electricity_price_per_kwh_micros INTEGER NOT NULL DEFAULT 400000
                    CHECK (electricity_price_per_kwh_micros >= 0);
                ALTER TABLE print_jobs
                    ADD COLUMN machine_power_watts INTEGER NOT NULL DEFAULT 0
                    CHECK (machine_power_watts >= 0);
                ALTER TABLE print_jobs
                    ADD COLUMN estimated_runtime_seconds INTEGER NOT NULL DEFAULT 0
                    CHECK (estimated_runtime_seconds >= 0);
                ALTER TABLE print_jobs
                    ADD COLUMN electricity_cost_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (electricity_cost_micros >= 0);
                CREATE INDEX print_jobs_customer_order_idx
                    ON print_jobs(customer_order_id, created_at);

                ALTER TABLE allocations
                    ADD COLUMN material_price_per_kg_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (material_price_per_kg_micros >= 0);
                ALTER TABLE allocations
                    ADD COLUMN cost_currency TEXT NOT NULL DEFAULT 'EUR';
                ALTER TABLE allocations
                    ADD COLUMN estimated_material_cost_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (estimated_material_cost_micros >= 0);
                ALTER TABLE allocations
                    ADD COLUMN actual_material_cost_micros INTEGER
                    CHECK (actual_material_cost_micros >= 0);
                UPDATE allocations
                    SET actual_material_cost_micros = 0
                    WHERE actual_weight_mg IS NOT NULL;

                PRAGMA user_version = 2;
            )SQL");
        }
        if (version < 3) {
            exec_sql(db, R"SQL(
                ALTER TABLE print_jobs
                    ADD COLUMN started_at TEXT NOT NULL DEFAULT '';
                ALTER TABLE print_jobs
                    ADD COLUMN actual_runtime_seconds INTEGER
                    CHECK (actual_runtime_seconds >= 0);

                ALTER TABLE allocations
                    ADD COLUMN spool_name TEXT NOT NULL DEFAULT '';
                ALTER TABLE allocations
                    ADD COLUMN manufacturer TEXT NOT NULL DEFAULT '';
                ALTER TABLE allocations
                    ADD COLUMN material_type TEXT NOT NULL DEFAULT '';
                ALTER TABLE allocations
                    ADD COLUMN filament_preset_id TEXT NOT NULL DEFAULT '';
                ALTER TABLE allocations
                    ADD COLUMN color_hex TEXT NOT NULL DEFAULT '#FFFFFF';
                UPDATE allocations
                    SET spool_name = COALESCE(
                            (SELECT name FROM spools WHERE spools.id = allocations.spool_id),
                            ''),
                        manufacturer = COALESCE(
                            (SELECT manufacturer FROM spools
                             WHERE spools.id = allocations.spool_id),
                            ''),
                        material_type = COALESCE(
                            (SELECT material_type FROM spools
                             WHERE spools.id = allocations.spool_id),
                            ''),
                        filament_preset_id = COALESCE(
                            (SELECT filament_preset_id FROM spools
                             WHERE spools.id = allocations.spool_id),
                            ''),
                        color_hex = COALESCE(
                            (SELECT color_hex FROM spools
                             WHERE spools.id = allocations.spool_id),
                            '#FFFFFF');

                PRAGMA user_version = 3;
            )SQL");
        }
        if (version < 4) {
            struct AllocationCostMigration {
                std::string id;
                MoneyMicros estimated_cost_micros {0};
                std::optional<MoneyMicros> actual_cost_micros;
            };
            std::vector<AllocationCostMigration> corrected_costs;
            Statement allocations(db, R"SQL(
                SELECT id, material_price_per_kg_micros,
                       estimated_weight_mg, actual_weight_mg
                FROM allocations
            )SQL");
            while (allocations.step()) {
                AllocationCostMigration corrected;
                corrected.id = allocations.text(0);
                const MoneyMicros price_per_kg_micros =
                    allocations.integer64(1);
                corrected.estimated_cost_micros = material_cost(
                    price_per_kg_micros, allocations.integer64(2));
                if (!allocations.is_null(3))
                    corrected.actual_cost_micros = material_cost(
                        price_per_kg_micros, allocations.integer64(3));
                corrected_costs.emplace_back(std::move(corrected));
            }

            for (const AllocationCostMigration &corrected : corrected_costs) {
                Statement update_costs(db, R"SQL(
                    UPDATE allocations
                    SET estimated_material_cost_micros = ?,
                        actual_material_cost_micros = ?
                    WHERE id = ?
                )SQL");
                update_costs.bind(1, corrected.estimated_cost_micros);
                if (corrected.actual_cost_micros)
                    update_costs.bind(2, *corrected.actual_cost_micros);
                else
                    update_costs.bind_null(2);
                update_costs.bind(3, corrected.id);
                update_costs.execute();
            }
            exec_sql(db, "PRAGMA user_version = 4;");
        }
        if (version < 5) {
            exec_sql(db, R"SQL(
                CREATE TABLE IF NOT EXISTS print_job_manual_overrides (
                    job_id      TEXT PRIMARY KEY
                                REFERENCES print_jobs(id) ON DELETE CASCADE,
                    created_at  TEXT NOT NULL DEFAULT
                                (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
                );

                PRAGMA user_version = 5;
            )SQL");
        }
        if (version < 6) {
            bool cost_columns_exist = false;
            Statement columns(db, "PRAGMA table_info(inventory_settings)");
            while (columns.step()) {
                if (columns.text(1) == "machine_wear_per_hour_micros") {
                    cost_columns_exist = true;
                    break;
                }
            }
            if (!cost_columns_exist) {
                exec_sql(db, R"SQL(
                ALTER TABLE inventory_settings
                    ADD COLUMN machine_wear_per_hour_micros INTEGER NOT NULL DEFAULT 1250000
                    CHECK (machine_wear_per_hour_micros >= 0);
                ALTER TABLE inventory_settings
                    ADD COLUMN maintenance_per_hour_micros INTEGER NOT NULL DEFAULT 500000
                    CHECK (maintenance_per_hour_micros >= 0);
                ALTER TABLE inventory_settings
                    ADD COLUMN repair_reserve_per_hour_micros INTEGER NOT NULL DEFAULT 500000
                    CHECK (repair_reserve_per_hour_micros >= 0);
                ALTER TABLE inventory_settings
                    ADD COLUMN design_per_hour_micros INTEGER NOT NULL DEFAULT 45000000
                    CHECK (design_per_hour_micros >= 0);

                ALTER TABLE print_jobs
                    ADD COLUMN machine_wear_per_hour_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (machine_wear_per_hour_micros >= 0);
                ALTER TABLE print_jobs
                    ADD COLUMN maintenance_per_hour_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (maintenance_per_hour_micros >= 0);
                ALTER TABLE print_jobs
                    ADD COLUMN repair_reserve_per_hour_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (repair_reserve_per_hour_micros >= 0);
                ALTER TABLE print_jobs
                    ADD COLUMN machine_wear_cost_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (machine_wear_cost_micros >= 0);
                ALTER TABLE print_jobs
                    ADD COLUMN maintenance_cost_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (maintenance_cost_micros >= 0);
                ALTER TABLE print_jobs
                    ADD COLUMN repair_reserve_cost_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (repair_reserve_cost_micros >= 0);

                ALTER TABLE customer_orders
                    ADD COLUMN design_time_seconds INTEGER NOT NULL DEFAULT 0
                    CHECK (design_time_seconds >= 0);
                ALTER TABLE customer_orders
                    ADD COLUMN design_hourly_rate_micros INTEGER NOT NULL DEFAULT 45000000
                    CHECK (design_hourly_rate_micros >= 0);
                ALTER TABLE customer_orders
                    ADD COLUMN other_cost_micros INTEGER NOT NULL DEFAULT 0
                    CHECK (other_cost_micros >= 0);
                ALTER TABLE customer_orders
                    ADD COLUMN discount_basis_points INTEGER NOT NULL DEFAULT 0
                    CHECK (discount_basis_points BETWEEN 0 AND 10000);
                ALTER TABLE customer_orders ADD COLUMN bill_material INTEGER NOT NULL DEFAULT 1 CHECK (bill_material IN (0, 1));
                ALTER TABLE customer_orders ADD COLUMN bill_electricity INTEGER NOT NULL DEFAULT 1 CHECK (bill_electricity IN (0, 1));
                ALTER TABLE customer_orders ADD COLUMN bill_machine_wear INTEGER NOT NULL DEFAULT 1 CHECK (bill_machine_wear IN (0, 1));
                ALTER TABLE customer_orders ADD COLUMN bill_maintenance INTEGER NOT NULL DEFAULT 1 CHECK (bill_maintenance IN (0, 1));
                ALTER TABLE customer_orders ADD COLUMN bill_repair_reserve INTEGER NOT NULL DEFAULT 1 CHECK (bill_repair_reserve IN (0, 1));
                ALTER TABLE customer_orders ADD COLUMN bill_design INTEGER NOT NULL DEFAULT 1 CHECK (bill_design IN (0, 1));
                ALTER TABLE customer_orders ADD COLUMN bill_other INTEGER NOT NULL DEFAULT 1 CHECK (bill_other IN (0, 1));

                )SQL");
            }
            exec_sql(db, "PRAGMA user_version = 6;");
        }
        if (version < 7) {
            InventorySettings settings;
            Statement settings_statement(db, R"SQL(
                SELECT machine_wear_per_hour_micros,
                       maintenance_per_hour_micros,
                       repair_reserve_per_hour_micros
                FROM inventory_settings
                WHERE id = 1
            )SQL");
            if (!settings_statement.step())
                throw Error(ErrorCode::database, "Inventory cost settings are missing");
            settings.machine_wear_per_hour_micros = settings_statement.integer64(0);
            settings.maintenance_per_hour_micros = settings_statement.integer64(1);
            settings.repair_reserve_per_hour_micros = settings_statement.integer64(2);

            struct LegacyJobCostMigration {
                std::string id;
                std::int64_t runtime_seconds {0};
            };
            std::vector<LegacyJobCostMigration> legacy_jobs;
            Statement jobs(db, R"SQL(
                SELECT id, COALESCE(actual_runtime_seconds,
                                    estimated_runtime_seconds)
                FROM print_jobs
                WHERE machine_wear_per_hour_micros = 0
                  AND maintenance_per_hour_micros = 0
                  AND repair_reserve_per_hour_micros = 0
                  AND machine_wear_cost_micros = 0
                  AND maintenance_cost_micros = 0
                  AND repair_reserve_cost_micros = 0
            )SQL");
            while (jobs.step())
                legacy_jobs.push_back({jobs.text(0), jobs.integer64(1)});

            for (const LegacyJobCostMigration &job : legacy_jobs) {
                Statement update(db, R"SQL(
                    UPDATE print_jobs
                    SET machine_wear_per_hour_micros = ?,
                        maintenance_per_hour_micros = ?,
                        repair_reserve_per_hour_micros = ?,
                        machine_wear_cost_micros = ?,
                        maintenance_cost_micros = ?,
                        repair_reserve_cost_micros = ?
                    WHERE id = ?
                )SQL");
                update.bind(1, settings.machine_wear_per_hour_micros);
                update.bind(2, settings.maintenance_per_hour_micros);
                update.bind(3, settings.repair_reserve_per_hour_micros);
                update.bind(4, hourly_cost(
                    settings.machine_wear_per_hour_micros, job.runtime_seconds,
                    "Legacy machine wear cost"));
                update.bind(5, hourly_cost(
                    settings.maintenance_per_hour_micros, job.runtime_seconds,
                    "Legacy maintenance cost"));
                update.bind(6, hourly_cost(
                    settings.repair_reserve_per_hour_micros, job.runtime_seconds,
                    "Legacy repair reserve cost"));
                update.bind(7, job.id);
                update.execute();
            }
            exec_sql(db, "PRAGMA user_version = 7;");
        }
        transaction.commit();
    }

    Milligrams physical_balance(const std::string &spool_id) const
    {
        Statement statement(db, "SELECT COALESCE(SUM(delta_mg), 0) FROM stock_events WHERE spool_id = ?");
        statement.bind(1, spool_id);
        if (!statement.step())
            throw Error(ErrorCode::database, "Could not calculate spool balance");
        return statement.integer64(0);
    }

    Milligrams active_reservations(const std::string &spool_id, const std::string &excluded_job_id = {}) const
    {
        Statement statement(db, R"SQL(
            SELECT COALESCE(SUM(a.estimated_weight_mg), 0)
            FROM allocations a
            JOIN print_jobs j ON j.id = a.job_id
            WHERE a.spool_id = ?
              AND j.state IN ('reserved', 'printing', 'needs_review')
              AND (? = '' OR j.id <> ?)
        )SQL");
        statement.bind(1, spool_id);
        statement.bind(2, excluded_job_id);
        statement.bind(3, excluded_job_id);
        if (!statement.step())
            throw Error(ErrorCode::database, "Could not calculate spool reservations");
        return statement.integer64(0);
    }

    Spool read_spool(Statement &statement) const
    {
        Spool spool;
        spool.id                    = statement.text(0);
        spool.manufacturer          = statement.text(1);
        spool.material_type         = statement.text(2);
        spool.name                  = statement.text(3);
        spool.filament_preset_id    = statement.text(4);
        spool.color_hex             = statement.text(5);
        spool.diameter_mm           = statement.number(6);
        spool.density_g_cm3         = statement.number(7);
        spool.nominal_capacity_mg   = statement.integer64(8);
        spool.warning_mode          = warning_mode_from_string(statement.text(9));
        spool.warning_value         = statement.integer64(10);
        spool.status                = spool_status_from_string(statement.text(11));
        spool.material_price_per_kg_micros = statement.integer64(12);
        spool.price_currency        = statement.text(13);
        spool.current_weight_mg     = statement.integer64(14);
        spool.reserved_weight_mg    = statement.integer64(15);
        spool.available_weight_mg   = checked_subtract(
            spool.current_weight_mg, spool.reserved_weight_mg, "Available filament");
        spool.created_at            = statement.text(16);
        spool.updated_at            = statement.text(17);
        return spool;
    }

    static constexpr const char *spool_select = R"SQL(
        SELECT
            s.id, s.manufacturer, s.material_type, s.name, s.filament_preset_id,
            s.color_hex, s.diameter_mm, s.density_g_cm3, s.nominal_capacity_mg,
            s.warning_mode, s.warning_value, s.status,
            s.material_price_per_kg_micros, s.price_currency,
            COALESCE((SELECT SUM(e.delta_mg) FROM stock_events e WHERE e.spool_id = s.id), 0),
            COALESCE((
                SELECT SUM(a.estimated_weight_mg)
                FROM allocations a
                JOIN print_jobs j ON j.id = a.job_id
                WHERE a.spool_id = s.id
                  AND j.state IN ('reserved', 'printing', 'needs_review')
            ), 0),
            s.created_at, s.updated_at
        FROM spools s
    )SQL";

    Spool get_spool_unlocked(const std::string &spool_id) const
    {
        const std::string sql = std::string(spool_select) + " WHERE s.id = ?";
        Statement statement(db, sql.c_str());
        statement.bind(1, spool_id);
        if (!statement.step())
            throw Error(ErrorCode::not_found, "Filament spool was not found: " + spool_id);
        return read_spool(statement);
    }

    std::optional<StockEventRecord> stock_event_for_key(const std::string &operation_key) const
    {
        Statement statement(db, R"SQL(
            SELECT spool_id, COALESCE(job_id, ''), COALESCE(allocation_id, ''),
                   event_type, delta_mg, balance_after_mg
            FROM stock_events
            WHERE operation_key = ?
        )SQL");
        statement.bind(1, operation_key);
        if (!statement.step())
            return std::nullopt;
        return StockEventRecord {
            statement.text(0),
            statement.text(1),
            statement.text(2),
            statement.text(3),
            statement.integer64(4),
            statement.integer64(5)
        };
    }

    void update_automatic_status(const std::string &spool_id, Milligrams balance)
    {
        Statement statement(db, R"SQL(
            UPDATE spools
            SET status = CASE
                    WHEN status = 'archived' THEN status
                    WHEN ? <= 0 THEN 'empty'
                    ELSE 'active'
                END,
                updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
            WHERE id = ?
        )SQL");
        statement.bind(1, balance);
        statement.bind(2, spool_id);
        statement.execute();
    }

    void add_stock_event(const std::string &spool_id, const std::string &job_id,
                         const std::string &allocation_id, const std::string &event_type,
                         Milligrams delta_mg, const std::string &operation_key,
                         const std::string &note)
    {
        if (const auto existing = stock_event_for_key(operation_key)) {
            if (existing->spool_id != spool_id || existing->job_id != job_id ||
                existing->allocation_id != allocation_id || existing->event_type != event_type ||
                existing->delta_mg != delta_mg)
                throw Error(ErrorCode::conflict, "Stock operation key was already used for a different change");
            return;
        }

        (void) get_spool_unlocked(spool_id);
        const Milligrams balance = checked_add(physical_balance(spool_id), delta_mg, "Spool balance");
        if (balance < 0)
            throw Error(ErrorCode::insufficient_stock,
                        "A stock change cannot reduce a spool below zero");
        Statement statement(db, R"SQL(
            INSERT INTO stock_events (
                id, spool_id, job_id, allocation_id, event_type, delta_mg,
                balance_after_mg, operation_key, note
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        )SQL");
        statement.bind(1, make_uuid());
        statement.bind(2, spool_id);
        if (job_id.empty()) statement.bind_null(3); else statement.bind(3, job_id);
        if (allocation_id.empty()) statement.bind_null(4); else statement.bind(4, allocation_id);
        statement.bind(5, event_type);
        statement.bind(6, delta_mg);
        statement.bind(7, balance);
        statement.bind(8, operation_key);
        statement.bind(9, note);
        statement.execute();
        update_automatic_status(spool_id, balance);
    }

    Allocation read_allocation(Statement &statement) const
    {
        Allocation allocation;
        allocation.id                  = statement.text(0);
        allocation.job_id              = statement.text(1);
        allocation.spool_id            = statement.text(2);
        allocation.spool_name           = statement.text(3);
        allocation.manufacturer         = statement.text(4);
        allocation.material_type        = statement.text(5);
        allocation.filament_preset_id   = statement.text(6);
        allocation.color_hex            = statement.text(7);
        allocation.filament_index       = statement.integer(8);
        allocation.estimated_weight_mg  = statement.integer64(9);
        if (!statement.is_null(10))
            allocation.actual_weight_mg = statement.integer64(10);
        if (statement.is_null(11))
            throw Error(
                ErrorCode::database,
                "Allocation references a missing spool: " +
                    allocation.spool_id);
        const MoneyMicros live_price_per_kg_micros =
            statement.integer64(11);
        const std::string live_currency = statement.text(12);
        const MoneyMicros booked_price_per_kg_micros =
            statement.integer64(13);
        const std::string booked_currency = statement.text(14);
        const bool live_price_is_compatible =
            normalize_currency(live_currency) ==
            normalize_currency(booked_currency);
        allocation.material_price_per_kg_micros =
            live_price_is_compatible ?
                live_price_per_kg_micros :
                booked_price_per_kg_micros;
        allocation.cost_currency =
            live_price_is_compatible ?
                live_currency :
                booked_currency;
        allocation.estimated_material_cost_micros = material_cost(
            allocation.material_price_per_kg_micros,
            allocation.estimated_weight_mg);
        if (allocation.actual_weight_mg)
            allocation.actual_material_cost_micros = material_cost(
                allocation.material_price_per_kg_micros,
                *allocation.actual_weight_mg);
        return allocation;
    }

    PrintJob read_job(Statement &statement) const
    {
        PrintJob job;
        job.id              = statement.text(0);
        job.idempotency_key = statement.text(1);
        job.job_name        = statement.text(2);
        job.project_path    = statement.text(3);
        job.printer_id      = statement.text(4);
        if (!statement.is_null(5))
            job.customer_order_id = statement.text(5);
        job.state           = job_state_from_string(statement.text(6));
        job.cost_currency   = statement.text(7);
        job.electricity_price_per_kwh_micros = statement.integer64(8);
        job.machine_power_watts = statement.integer64(9);
        job.estimated_runtime_seconds = statement.integer64(10);
        job.electricity_cost_micros = statement.integer64(11);
        job.machine_wear_per_hour_micros = statement.integer64(12);
        job.maintenance_per_hour_micros = statement.integer64(13);
        job.repair_reserve_per_hour_micros = statement.integer64(14);
        job.machine_wear_cost_micros = statement.integer64(15);
        job.maintenance_cost_micros = statement.integer64(16);
        job.repair_reserve_cost_micros = statement.integer64(17);
        job.created_at      = statement.text(18);
        job.updated_at      = statement.text(19);
        job.started_at      = statement.text(20);
        job.completed_at    = statement.text(21);
        if (!statement.is_null(22))
            job.actual_runtime_seconds = statement.integer64(22);

        // Allocation metadata remains a booking-time snapshot, but price and
        // calculated costs deliberately follow the stable spool UUID. Legacy
        // allocations whose spool was changed to another currency retain
        // their booked price because no implicit currency conversion is safe.
        // The legacy cost columns stay for backward compatibility and are no
        // longer authoritative when jobs are read.
        Statement allocations_statement(db, R"SQL(
            SELECT a.id, a.job_id, a.spool_id, a.spool_name,
                   a.manufacturer, a.material_type, a.filament_preset_id,
                   a.color_hex, a.filament_index, a.estimated_weight_mg,
                   a.actual_weight_mg, s.material_price_per_kg_micros,
                   s.price_currency, a.material_price_per_kg_micros,
                   a.cost_currency
            FROM allocations a
            LEFT JOIN spools s ON s.id = a.spool_id
            WHERE a.job_id = ?
            ORDER BY a.filament_index
        )SQL");
        allocations_statement.bind(1, job.id);
        while (allocations_statement.step())
            job.allocations.emplace_back(read_allocation(allocations_statement));
        return job;
    }

    PrintJob get_job_unlocked(const std::string &job_id) const
    {
        Statement statement(db, R"SQL(
            SELECT id, idempotency_key, job_name, project_path, printer_id,
                   customer_order_id, state, cost_currency,
                   electricity_price_per_kwh_micros, machine_power_watts,
                   estimated_runtime_seconds, electricity_cost_micros,
                   machine_wear_per_hour_micros, maintenance_per_hour_micros,
                   repair_reserve_per_hour_micros, machine_wear_cost_micros,
                   maintenance_cost_micros, repair_reserve_cost_micros,
                   created_at, updated_at, started_at, completed_at,
                   actual_runtime_seconds
            FROM print_jobs
            WHERE id = ?
        )SQL");
        statement.bind(1, job_id);
        if (!statement.step())
            throw Error(ErrorCode::not_found, "Print job was not found: " + job_id);
        return read_job(statement);
    }

    std::optional<PrintJob> find_job_by_key(const std::string &idempotency_key) const
    {
        Statement statement(db, R"SQL(
            SELECT id, idempotency_key, job_name, project_path, printer_id,
                   customer_order_id, state, cost_currency,
                   electricity_price_per_kwh_micros, machine_power_watts,
                   estimated_runtime_seconds, electricity_cost_micros,
                   machine_wear_per_hour_micros, maintenance_per_hour_micros,
                   repair_reserve_per_hour_micros, machine_wear_cost_micros,
                   maintenance_cost_micros, repair_reserve_cost_micros,
                   created_at, updated_at, started_at, completed_at,
                   actual_runtime_seconds
            FROM print_jobs
            WHERE idempotency_key = ?
        )SQL");
        statement.bind(1, idempotency_key);
        if (!statement.step())
            return std::nullopt;
        return read_job(statement);
    }

    static bool allocations_match(const std::vector<Allocation> &existing,
                                  const std::vector<AllocationInput> &requested)
    {
        if (existing.size() != requested.size())
            return false;
        for (const Allocation &allocation : existing) {
            auto found = std::find_if(requested.begin(), requested.end(), [&allocation](const AllocationInput &input) {
                return input.filament_index == allocation.filament_index;
            });
            if (found == requested.end() || found->spool_id != allocation.spool_id ||
                found->estimated_weight_mg != allocation.estimated_weight_mg)
                return false;
        }
        return true;
    }

    InventorySettings get_settings_unlocked() const
    {
        Statement statement(db, R"SQL(
            SELECT currency, electricity_price_per_kwh_micros,
                   default_machine_power_watts,
                   machine_wear_per_hour_micros,
                   maintenance_per_hour_micros,
                   repair_reserve_per_hour_micros,
                   design_per_hour_micros
            FROM inventory_settings
            WHERE id = 1
        )SQL");
        if (!statement.step())
            throw Error(ErrorCode::database, "Filament inventory settings are missing");
        return {
            statement.text(0), statement.integer64(1), statement.integer64(2),
            statement.integer64(3), statement.integer64(4),
            statement.integer64(5), statement.integer64(6)};
    }

    Customer read_customer(Statement &statement) const
    {
        return {
            statement.text(0),
            statement.text(1),
            statement.text(2),
            statement.text(3),
            statement.text(4),
            statement.text(5),
            statement.integer(6) != 0,
            statement.text(7),
            statement.text(8)
        };
    }

    Customer get_customer_unlocked(const std::string &customer_id) const
    {
        Statement statement(db, R"SQL(
            SELECT id, name, contact_name, email, phone, notes, archived,
                   created_at, updated_at
            FROM customers
            WHERE id = ?
        )SQL");
        statement.bind(1, customer_id);
        if (!statement.step())
            throw Error(ErrorCode::not_found, "Customer was not found: " + customer_id);
        return read_customer(statement);
    }

    CustomerOrder read_customer_order(Statement &statement) const
    {
        CustomerOrder order;
        order.id           = statement.text(0);
        order.customer_id  = statement.text(1);
        order.order_number = statement.text(2);
        order.title        = statement.text(3);
        order.notes        = statement.text(4);
        if (!statement.is_null(5))
            order.quoted_price_micros = statement.integer64(5);
        if (!statement.is_null(6))
            order.invoice_amount_micros = statement.integer64(6);
        order.currency   = statement.text(7);
        order.status     = customer_order_status_from_string(statement.text(8));
        order.design_time_seconds = statement.integer64(9);
        order.design_hourly_rate_micros = statement.integer64(10);
        order.other_cost_micros = statement.integer64(11);
        order.discount_basis_points = statement.integer64(12);
        order.bill_material = statement.integer(13) != 0;
        order.bill_electricity = statement.integer(14) != 0;
        order.bill_machine_wear = statement.integer(15) != 0;
        order.bill_maintenance = statement.integer(16) != 0;
        order.bill_repair_reserve = statement.integer(17) != 0;
        order.bill_design = statement.integer(18) != 0;
        order.bill_other = statement.integer(19) != 0;
        order.created_at = statement.text(20);
        order.updated_at = statement.text(21);
        return order;
    }

    CustomerOrder get_customer_order_unlocked(const std::string &order_id) const
    {
        Statement statement(db, R"SQL(
            SELECT id, customer_id, order_number, title, notes,
                   quoted_price_micros, invoice_amount_micros, currency, status,
                   design_time_seconds, design_hourly_rate_micros,
                   other_cost_micros, discount_basis_points,
                   bill_material, bill_electricity, bill_machine_wear,
                   bill_maintenance, bill_repair_reserve, bill_design, bill_other,
                   created_at, updated_at
            FROM customer_orders
            WHERE id = ?
        )SQL");
        statement.bind(1, order_id);
        if (!statement.step())
            throw Error(ErrorCode::not_found, "Customer order was not found: " + order_id);
        return read_customer_order(statement);
    }

    sqlite3           *db {nullptr};
    mutable std::mutex mutex;
};

Store::Store(const std::string &database_path) : m_impl(std::make_unique<Impl>(database_path)) {}
Store::~Store() = default;
Store::Store(Store &&) noexcept = default;
Store &Store::operator=(Store &&) noexcept = default;

int Store::current_schema_version() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->current_schema_version_unlocked();
}

InventorySettings Store::get_settings() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->get_settings_unlocked();
}

InventorySettings Store::update_settings(const InventorySettings &settings)
{
    validate_settings(settings);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Statement statement(m_impl->db, R"SQL(
        UPDATE inventory_settings
        SET currency = ?, electricity_price_per_kwh_micros = ?,
            default_machine_power_watts = ?, machine_wear_per_hour_micros = ?,
            maintenance_per_hour_micros = ?, repair_reserve_per_hour_micros = ?,
            design_per_hour_micros = ?,
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = 1
    )SQL");
    statement.bind(1, normalize_currency(settings.currency));
    statement.bind(2, settings.electricity_price_per_kwh_micros);
    statement.bind(3, settings.default_machine_power_watts);
    statement.bind(4, settings.machine_wear_per_hour_micros);
    statement.bind(5, settings.maintenance_per_hour_micros);
    statement.bind(6, settings.repair_reserve_per_hour_micros);
    statement.bind(7, settings.design_per_hour_micros);
    statement.execute();
    return m_impl->get_settings_unlocked();
}

std::size_t Store::recalculate_customer_order_costs(
    const std::vector<std::string> &order_ids)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    const InventorySettings settings = m_impl->get_settings_unlocked();
    const std::set<std::string> unique_order_ids(order_ids.begin(), order_ids.end());
    std::size_t updated_jobs = 0;

    for (const std::string &order_id : unique_order_ids) {
        if (trim_copy(order_id).empty())
            throw Error(ErrorCode::validation, "Customer order ID must not be empty");
        const CustomerOrder order = m_impl->get_customer_order_unlocked(order_id);
        if (normalize_currency(order.currency) != normalize_currency(settings.currency))
            throw Error(
                ErrorCode::conflict,
                "Customer-order currency does not match the current cost settings");

        Statement update_order(m_impl->db, R"SQL(
            UPDATE customer_orders
            SET design_hourly_rate_micros = ?,
                updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
            WHERE id = ?
        )SQL");
        update_order.bind(1, settings.design_per_hour_micros);
        update_order.bind(2, order_id);
        update_order.execute();

        struct RuntimeSnapshot {
            std::string id;
            std::int64_t runtime_seconds {0};
        };
        std::vector<RuntimeSnapshot> jobs;
        Statement select_jobs(m_impl->db, R"SQL(
            SELECT id, COALESCE(actual_runtime_seconds,
                                estimated_runtime_seconds)
            FROM print_jobs
            WHERE customer_order_id = ? AND state != 'discarded'
        )SQL");
        select_jobs.bind(1, order_id);
        while (select_jobs.step())
            jobs.push_back({select_jobs.text(0), select_jobs.integer64(1)});

        for (const RuntimeSnapshot &job : jobs) {
            Statement update_job(m_impl->db, R"SQL(
                UPDATE print_jobs
                SET electricity_price_per_kwh_micros = ?,
                    machine_power_watts = ?,
                    electricity_cost_micros = ?,
                    machine_wear_per_hour_micros = ?,
                    maintenance_per_hour_micros = ?,
                    repair_reserve_per_hour_micros = ?,
                    machine_wear_cost_micros = ?,
                    maintenance_cost_micros = ?,
                    repair_reserve_cost_micros = ?,
                    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
                WHERE id = ?
            )SQL");
            update_job.bind(1, settings.electricity_price_per_kwh_micros);
            update_job.bind(2, settings.default_machine_power_watts);
            update_job.bind(3, electricity_cost(
                settings.electricity_price_per_kwh_micros,
                settings.default_machine_power_watts, job.runtime_seconds));
            update_job.bind(4, settings.machine_wear_per_hour_micros);
            update_job.bind(5, settings.maintenance_per_hour_micros);
            update_job.bind(6, settings.repair_reserve_per_hour_micros);
            update_job.bind(7, hourly_cost(
                settings.machine_wear_per_hour_micros, job.runtime_seconds,
                "Recalculated machine wear cost"));
            update_job.bind(8, hourly_cost(
                settings.maintenance_per_hour_micros, job.runtime_seconds,
                "Recalculated maintenance cost"));
            update_job.bind(9, hourly_cost(
                settings.repair_reserve_per_hour_micros, job.runtime_seconds,
                "Recalculated repair reserve cost"));
            update_job.bind(10, job.id);
            update_job.execute();
            ++updated_jobs;
        }
    }

    transaction.commit();
    return updated_jobs;
}

Spool Store::create_spool(const SpoolInput &input,
                          const std::vector<SpoolIdentifierInput> &identifiers)
{
    validate_spool_input(input);
    const std::string id = make_uuid();
    std::vector<std::pair<IdentifierKind, std::string>> normalized_identifiers {
        {IdentifierKind::quack_ndef_uuid, id}
    };
    std::set<std::pair<std::string, std::string>> seen_identifiers {
        {to_string(IdentifierKind::quack_ndef_uuid), id}
    };
    for (const SpoolIdentifierInput &identifier : identifiers) {
        const std::string normalized = normalize_identifier(identifier.kind, identifier.value);
        if (seen_identifiers.emplace(to_string(identifier.kind), normalized).second)
            normalized_identifiers.emplace_back(identifier.kind, normalized);
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    Statement statement(m_impl->db, R"SQL(
        INSERT INTO spools (
            id, manufacturer, material_type, name, filament_preset_id, color_hex,
            diameter_mm, density_g_cm3, nominal_capacity_mg, warning_mode,
            warning_value, status, material_price_per_kg_micros, price_currency
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL");
    statement.bind(1, id);
    statement.bind(2, trim_copy(input.manufacturer));
    statement.bind(3, trim_copy(input.material_type));
    statement.bind(4, trim_copy(input.name));
    statement.bind(5, trim_copy(input.filament_preset_id));
    statement.bind(6, normalize_color(input.color_hex));
    statement.bind(7, input.diameter_mm);
    statement.bind(8, input.density_g_cm3);
    statement.bind(9, input.nominal_capacity_mg);
    statement.bind(10, to_string(input.warning_mode));
    statement.bind(11, input.warning_value);
    statement.bind(12, input.initial_weight_mg > 0 ? "active" : "empty");
    statement.bind(13, input.material_price_per_kg_micros);
    statement.bind(14, normalize_currency(input.price_currency));
    statement.execute();

    m_impl->add_stock_event(id, {}, {}, "initial", input.initial_weight_mg, "initial:" + id, "Initial fill level");
    for (const auto &[kind, value] : normalized_identifiers) {
        Statement existing(m_impl->db, "SELECT spool_id FROM spool_identifiers WHERE kind = ? AND value = ?");
        existing.bind(1, to_string(kind));
        existing.bind(2, value);
        if (existing.step())
            throw Error(ErrorCode::conflict, "This NFC/RFID identifier already belongs to another spool");

        Statement identifier(m_impl->db,
            "INSERT INTO spool_identifiers (kind, value, spool_id) VALUES (?, ?, ?)");
        identifier.bind(1, to_string(kind));
        identifier.bind(2, value);
        identifier.bind(3, id);
        identifier.execute();
    }
    transaction.commit();
    return m_impl->get_spool_unlocked(id);
}

Spool Store::update_spool(const std::string &spool_id, const SpoolInput &input,
                          const std::string &weight_operation_key)
{
    validate_spool_input(input);
    const std::string price_currency =
        normalize_currency(input.price_currency);

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    const Spool existing = m_impl->get_spool_unlocked(spool_id);
    const std::string normalized_key = trim_copy(weight_operation_key);
    validate_operation_key(normalized_key);

    if (price_currency != normalize_currency(existing.price_currency)) {
        Statement incompatible_currency(m_impl->db, R"SQL(
            SELECT 1
            FROM allocations a
            JOIN print_jobs j ON j.id = a.job_id
            WHERE a.spool_id = ? AND j.cost_currency <> ?
            LIMIT 1
        )SQL");
        incompatible_currency.bind(1, spool_id);
        incompatible_currency.bind(2, price_currency);
        if (incompatible_currency.step())
            throw Error(
                ErrorCode::conflict,
                "A referenced spool must keep the currency used by its print jobs");
    }

    Statement referenced_weights(m_impl->db, R"SQL(
        SELECT a.estimated_weight_mg, a.actual_weight_mg
        FROM allocations a
        JOIN print_jobs j ON j.id = a.job_id
        WHERE a.spool_id = ? AND j.cost_currency = ?
    )SQL");
    referenced_weights.bind(1, spool_id);
    referenced_weights.bind(2, price_currency);
    while (referenced_weights.step()) {
        (void) material_cost(
            input.material_price_per_kg_micros,
            referenced_weights.integer64(0));
        if (!referenced_weights.is_null(1))
            (void) material_cost(
                input.material_price_per_kg_micros,
                referenced_weights.integer64(1));
    }

    Statement statement(m_impl->db, R"SQL(
        UPDATE spools
        SET manufacturer = ?, material_type = ?, name = ?, filament_preset_id = ?,
            color_hex = ?, diameter_mm = ?, density_g_cm3 = ?, nominal_capacity_mg = ?,
            warning_mode = ?, warning_value = ?, material_price_per_kg_micros = ?,
            price_currency = ?,
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, trim_copy(input.manufacturer));
    statement.bind(2, trim_copy(input.material_type));
    statement.bind(3, trim_copy(input.name));
    statement.bind(4, trim_copy(input.filament_preset_id));
    statement.bind(5, normalize_color(input.color_hex));
    statement.bind(6, input.diameter_mm);
    statement.bind(7, input.density_g_cm3);
    statement.bind(8, input.nominal_capacity_mg);
    statement.bind(9, to_string(input.warning_mode));
    statement.bind(10, input.warning_value);
    statement.bind(11, input.material_price_per_kg_micros);
    statement.bind(12, price_currency);
    statement.bind(13, spool_id);
    statement.execute();

    const Milligrams delta = checked_subtract(
        input.initial_weight_mg, existing.current_weight_mg, "Fill-level adjustment");
    if (const auto stock_event = m_impl->stock_event_for_key(normalized_key)) {
        const bool is_matching_edit =
            stock_event->spool_id == spool_id &&
            stock_event->job_id.empty() &&
            stock_event->allocation_id.empty() &&
            stock_event->event_type == "set_remaining" &&
            stock_event->balance_after_mg == input.initial_weight_mg;
        if (!is_matching_edit)
            throw Error(
                ErrorCode::conflict,
                "Stock operation key was already used for a different fill level");
    } else {
        m_impl->add_stock_event(
            spool_id, {}, {}, "set_remaining", delta, normalized_key,
            delta == 0 ? "Fill level confirmed while editing spool" :
                         "Fill level changed while editing spool");
    }
    transaction.commit();
    return m_impl->get_spool_unlocked(spool_id);
}

void Store::archive_spool(const std::string &spool_id)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    (void) m_impl->get_spool_unlocked(spool_id);
    if (m_impl->active_reservations(spool_id) != 0)
        throw Error(ErrorCode::conflict, "A spool with active print reservations cannot be archived");

    Statement statement(m_impl->db, R"SQL(
        UPDATE spools
        SET status = 'archived', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, spool_id);
    statement.execute();
    transaction.commit();
}

Spool Store::get_spool(const std::string &spool_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->get_spool_unlocked(spool_id);
}

std::vector<Spool> Store::list_spools(bool include_archived) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    std::string sql = Impl::spool_select;
    if (!include_archived)
        sql += " WHERE s.status <> 'archived'";
    sql += " ORDER BY s.name COLLATE NOCASE, s.created_at";

    Statement statement(m_impl->db, sql.c_str());
    std::vector<Spool> result;
    while (statement.step())
        result.emplace_back(m_impl->read_spool(statement));
    return result;
}

std::optional<Spool> Store::find_spool(IdentifierKind kind, const std::string &value) const
{
    const std::string normalized = normalize_identifier(kind, value);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Statement statement(m_impl->db, "SELECT spool_id FROM spool_identifiers WHERE kind = ? AND value = ?");
    statement.bind(1, to_string(kind));
    statement.bind(2, normalized);
    if (!statement.step())
        return std::nullopt;
    return m_impl->get_spool_unlocked(statement.text(0));
}

std::vector<SpoolIdentifier> Store::list_identifiers(const std::string &spool_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    (void) m_impl->get_spool_unlocked(spool_id);
    Statement statement(m_impl->db, R"SQL(
        SELECT kind, value, spool_id, created_at
        FROM spool_identifiers
        WHERE spool_id = ?
        ORDER BY kind, value
    )SQL");
    statement.bind(1, spool_id);

    std::vector<SpoolIdentifier> result;
    while (statement.step()) {
        result.push_back({
            identifier_kind_from_string(statement.text(0)),
            statement.text(1),
            statement.text(2),
            statement.text(3)
        });
    }
    return result;
}

std::vector<SpoolIdentifier> Store::list_identifiers() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Statement statement(m_impl->db, R"SQL(
        SELECT kind, value, spool_id, created_at
        FROM spool_identifiers
        ORDER BY spool_id, kind, value
    )SQL");

    std::vector<SpoolIdentifier> result;
    while (statement.step()) {
        result.push_back({
            identifier_kind_from_string(statement.text(0)),
            statement.text(1),
            statement.text(2),
            statement.text(3)
        });
    }
    return result;
}

void Store::bind_identifier(const std::string &spool_id, IdentifierKind kind, const std::string &value)
{
    const std::string normalized = normalize_identifier(kind, value);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    (void) m_impl->get_spool_unlocked(spool_id);

    Statement existing(m_impl->db, "SELECT spool_id FROM spool_identifiers WHERE kind = ? AND value = ?");
    existing.bind(1, to_string(kind));
    existing.bind(2, normalized);
    if (existing.step()) {
        if (existing.text(0) != spool_id)
            throw Error(ErrorCode::conflict, "This NFC/RFID identifier already belongs to another spool");
        transaction.commit();
        return;
    }

    Statement statement(m_impl->db, "INSERT INTO spool_identifiers (kind, value, spool_id) VALUES (?, ?, ?)");
    statement.bind(1, to_string(kind));
    statement.bind(2, normalized);
    statement.bind(3, spool_id);
    statement.execute();
    transaction.commit();
}

void Store::unbind_identifier(IdentifierKind kind, const std::string &value)
{
    const std::string normalized = normalize_identifier(kind, value);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Statement statement(m_impl->db, "DELETE FROM spool_identifiers WHERE kind = ? AND value = ?");
    statement.bind(1, to_string(kind));
    statement.bind(2, normalized);
    statement.execute();
}

void Store::replace_physical_identifiers(
    const std::string &spool_id, const std::vector<SpoolIdentifierInput> &identifiers)
{
    std::vector<std::pair<IdentifierKind, std::string>> normalized_identifiers;
    std::set<std::pair<std::string, std::string>> seen;
    for (const SpoolIdentifierInput &identifier : identifiers) {
        if (identifier.kind == IdentifierKind::quack_ndef_uuid)
            throw Error(ErrorCode::validation, "Quack NDEF UUID aliases cannot be replaced as physical tags");
        const std::string normalized = normalize_identifier(identifier.kind, identifier.value);
        if (seen.emplace(to_string(identifier.kind), normalized).second)
            normalized_identifiers.emplace_back(identifier.kind, normalized);
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    (void) m_impl->get_spool_unlocked(spool_id);

    for (const auto &[kind, value] : normalized_identifiers) {
        Statement existing(
            m_impl->db, "SELECT spool_id FROM spool_identifiers WHERE kind = ? AND value = ?");
        existing.bind(1, to_string(kind));
        existing.bind(2, value);
        if (existing.step() && existing.text(0) != spool_id)
            throw Error(ErrorCode::conflict, "This NFC/RFID identifier already belongs to another spool");
    }

    Statement remove(m_impl->db, R"SQL(
        DELETE FROM spool_identifiers
        WHERE spool_id = ? AND kind IN ('nfc_uid', 'bambu_tag_uid')
    )SQL");
    remove.bind(1, spool_id);
    remove.execute();

    for (const auto &[kind, value] : normalized_identifiers) {
        Statement insert(m_impl->db, R"SQL(
            INSERT INTO spool_identifiers (kind, value, spool_id) VALUES (?, ?, ?)
        )SQL");
        insert.bind(1, to_string(kind));
        insert.bind(2, value);
        insert.bind(3, spool_id);
        insert.execute();
    }
    transaction.commit();
}

void Store::set_remaining(const std::string &spool_id, Milligrams remaining_mg,
                          const std::string &operation_key, const std::string &note)
{
    if (remaining_mg < 0)
        throw Error(ErrorCode::validation, "Remaining filament weight must not be negative");
    const std::string normalized_key = trim_copy(operation_key);
    validate_operation_key(normalized_key);

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    if (const auto existing = m_impl->stock_event_for_key(normalized_key)) {
        const bool is_manual_event =
            existing->job_id.empty() && existing->allocation_id.empty() &&
            existing->event_type == "set_remaining";
        if (existing->spool_id != spool_id || existing->balance_after_mg != remaining_mg ||
            !is_manual_event)
            throw Error(ErrorCode::conflict, "Stock operation key was already used for a different fill level");
        transaction.commit();
        return;
    }

    const Milligrams delta = checked_subtract(
        remaining_mg, m_impl->physical_balance(spool_id), "Fill-level adjustment");
    m_impl->add_stock_event(spool_id, {}, {}, "set_remaining", delta, normalized_key, note);
    transaction.commit();
}

void Store::adjust_stock(const std::string &spool_id, Milligrams delta_mg,
                         const std::string &operation_key, const std::string &note)
{
    const std::string normalized_key = trim_copy(operation_key);
    validate_operation_key(normalized_key);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    m_impl->add_stock_event(spool_id, {}, {}, delta_mg >= 0 ? "refill" : "adjustment",
                            delta_mg, normalized_key, note);
    transaction.commit();
}

std::vector<StockEvent> Store::list_stock_events(
    const std::string &spool_id, std::size_t limit) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!spool_id.empty())
        (void) m_impl->get_spool_unlocked(spool_id);

    std::string sql = R"SQL(
        SELECT id, spool_id, COALESCE(job_id, ''), COALESCE(allocation_id, ''),
               event_type, delta_mg, balance_after_mg, operation_key, note, created_at
        FROM stock_events
    )SQL";
    if (!spool_id.empty())
        sql += " WHERE spool_id = ?";
    sql += " ORDER BY created_at DESC, rowid DESC";
    if (limit != 0)
        sql += " LIMIT ?";

    Statement statement(m_impl->db, sql.c_str());
    int parameter = 1;
    if (!spool_id.empty())
        statement.bind(parameter++, spool_id);
    if (limit != 0) {
        if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw Error(ErrorCode::validation, "Stock-event history limit is too large");
        statement.bind(parameter, static_cast<std::int64_t>(limit));
    }

    std::vector<StockEvent> result;
    while (statement.step()) {
        result.push_back({
            statement.text(0),
            statement.text(1),
            statement.text(2),
            statement.text(3),
            statement.text(4),
            statement.integer64(5),
            statement.integer64(6),
            statement.text(7),
            statement.text(8),
            statement.text(9)
        });
    }
    return result;
}

Customer Store::create_customer(const CustomerInput &input)
{
    validate_customer_input(input);
    const std::string id = make_uuid();
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Statement statement(m_impl->db, R"SQL(
        INSERT INTO customers (id, name, contact_name, email, phone, notes)
        VALUES (?, ?, ?, ?, ?, ?)
    )SQL");
    statement.bind(1, id);
    statement.bind(2, trim_copy(input.name));
    statement.bind(3, trim_copy(input.contact_name));
    statement.bind(4, trim_copy(input.email));
    statement.bind(5, trim_copy(input.phone));
    statement.bind(6, input.notes);
    statement.execute();
    return m_impl->get_customer_unlocked(id);
}

Customer Store::update_customer(const std::string &customer_id, const CustomerInput &input)
{
    validate_customer_input(input);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    (void) m_impl->get_customer_unlocked(customer_id);
    Statement statement(m_impl->db, R"SQL(
        UPDATE customers
        SET name = ?, contact_name = ?, email = ?, phone = ?, notes = ?,
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, trim_copy(input.name));
    statement.bind(2, trim_copy(input.contact_name));
    statement.bind(3, trim_copy(input.email));
    statement.bind(4, trim_copy(input.phone));
    statement.bind(5, input.notes);
    statement.bind(6, customer_id);
    statement.execute();
    return m_impl->get_customer_unlocked(customer_id);
}

void Store::archive_customer(const std::string &customer_id)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    (void) m_impl->get_customer_unlocked(customer_id);
    Statement statement(m_impl->db, R"SQL(
        UPDATE customers
        SET archived = 1, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, customer_id);
    statement.execute();
}

Customer Store::get_customer(const std::string &customer_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->get_customer_unlocked(customer_id);
}

std::vector<Customer> Store::list_customers(bool include_archived) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    std::string sql = R"SQL(
        SELECT id, name, contact_name, email, phone, notes, archived,
               created_at, updated_at
        FROM customers
    )SQL";
    if (!include_archived)
        sql += " WHERE archived = 0";
    sql += " ORDER BY name COLLATE NOCASE, created_at";

    Statement statement(m_impl->db, sql.c_str());
    std::vector<Customer> result;
    while (statement.step())
        result.emplace_back(m_impl->read_customer(statement));
    return result;
}

CustomerOrder Store::create_customer_order(const CustomerOrderInput &input)
{
    validate_customer_order_input(input);
    const std::string id = make_uuid();
    const std::string currency = normalize_currency(input.currency);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Customer customer = m_impl->get_customer_unlocked(trim_copy(input.customer_id));
    if (customer.archived)
        throw Error(ErrorCode::conflict, "An archived customer cannot receive a new order");

    Statement statement(m_impl->db, R"SQL(
        INSERT INTO customer_orders (
            id, customer_id, order_number, title, notes,
            quoted_price_micros, invoice_amount_micros, currency, status,
            design_time_seconds, design_hourly_rate_micros, other_cost_micros,
            discount_basis_points, bill_material, bill_electricity,
            bill_machine_wear, bill_maintenance, bill_repair_reserve,
            bill_design, bill_other
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'draft', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL");
    statement.bind(1, id);
    statement.bind(2, customer.id);
    statement.bind(3, trim_copy(input.order_number));
    statement.bind(4, trim_copy(input.title));
    statement.bind(5, input.notes);
    if (input.quoted_price_micros) statement.bind(6, *input.quoted_price_micros);
    else statement.bind_null(6);
    if (input.invoice_amount_micros) statement.bind(7, *input.invoice_amount_micros);
    else statement.bind_null(7);
    statement.bind(8, currency);
    statement.bind(9, input.design_time_seconds);
    statement.bind(10, input.design_hourly_rate_micros);
    statement.bind(11, input.other_cost_micros);
    statement.bind(12, input.discount_basis_points);
    statement.bind(13, input.bill_material ? 1 : 0);
    statement.bind(14, input.bill_electricity ? 1 : 0);
    statement.bind(15, input.bill_machine_wear ? 1 : 0);
    statement.bind(16, input.bill_maintenance ? 1 : 0);
    statement.bind(17, input.bill_repair_reserve ? 1 : 0);
    statement.bind(18, input.bill_design ? 1 : 0);
    statement.bind(19, input.bill_other ? 1 : 0);
    statement.execute();
    return m_impl->get_customer_order_unlocked(id);
}

CustomerOrder Store::update_customer_order(
    const std::string &order_id, const CustomerOrderInput &input)
{
    validate_customer_order_input(input);
    const std::string customer_id = trim_copy(input.customer_id);
    const std::string currency    = normalize_currency(input.currency);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    const CustomerOrder existing = m_impl->get_customer_order_unlocked(order_id);
    const Customer customer = m_impl->get_customer_unlocked(customer_id);
    if (customer.archived && customer.id != existing.customer_id)
        throw Error(ErrorCode::conflict, "An order cannot be moved to an archived customer");

    Statement currency_conflict(m_impl->db, R"SQL(
        SELECT 1 FROM print_jobs
        WHERE customer_order_id = ? AND cost_currency <> ?
        LIMIT 1
    )SQL");
    currency_conflict.bind(1, order_id);
    currency_conflict.bind(2, currency);
    if (currency_conflict.step())
        throw Error(
            ErrorCode::conflict,
            "Order currency cannot change after a print-job cost snapshot was created");

    Statement statement(m_impl->db, R"SQL(
        UPDATE customer_orders
        SET customer_id = ?, order_number = ?, title = ?, notes = ?,
            quoted_price_micros = ?, invoice_amount_micros = ?, currency = ?,
            design_time_seconds = ?, design_hourly_rate_micros = ?,
            other_cost_micros = ?, discount_basis_points = ?,
            bill_material = ?, bill_electricity = ?, bill_machine_wear = ?,
            bill_maintenance = ?, bill_repair_reserve = ?, bill_design = ?,
            bill_other = ?,
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, customer_id);
    statement.bind(2, trim_copy(input.order_number));
    statement.bind(3, trim_copy(input.title));
    statement.bind(4, input.notes);
    if (input.quoted_price_micros) statement.bind(5, *input.quoted_price_micros);
    else statement.bind_null(5);
    if (input.invoice_amount_micros) statement.bind(6, *input.invoice_amount_micros);
    else statement.bind_null(6);
    statement.bind(7, currency);
    statement.bind(8, input.design_time_seconds);
    statement.bind(9, input.design_hourly_rate_micros);
    statement.bind(10, input.other_cost_micros);
    statement.bind(11, input.discount_basis_points);
    statement.bind(12, input.bill_material ? 1 : 0);
    statement.bind(13, input.bill_electricity ? 1 : 0);
    statement.bind(14, input.bill_machine_wear ? 1 : 0);
    statement.bind(15, input.bill_maintenance ? 1 : 0);
    statement.bind(16, input.bill_repair_reserve ? 1 : 0);
    statement.bind(17, input.bill_design ? 1 : 0);
    statement.bind(18, input.bill_other ? 1 : 0);
    statement.bind(19, order_id);
    statement.execute();
    const CustomerOrder result =
        m_impl->get_customer_order_unlocked(order_id);
    transaction.commit();
    return result;
}

void Store::delete_customer_order(const std::string &order_id)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    (void) m_impl->get_customer_order_unlocked(order_id);
    Statement jobs(m_impl->db, "SELECT 1 FROM print_jobs WHERE customer_order_id = ? LIMIT 1");
    jobs.bind(1, order_id);
    if (jobs.step())
        throw Error(ErrorCode::conflict, "An order with print jobs cannot be deleted");
    Statement statement(m_impl->db, "DELETE FROM customer_orders WHERE id = ?");
    statement.bind(1, order_id);
    statement.execute();
    transaction.commit();
}

void Store::set_customer_order_status(
    const std::string &order_id, CustomerOrderStatus status)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    const CustomerOrder order = m_impl->get_customer_order_unlocked(order_id);
    if (order.status == status) {
        transaction.commit();
        return;
    }

    const bool valid_transition =
        (order.status == CustomerOrderStatus::draft &&
         (status == CustomerOrderStatus::active ||
          status == CustomerOrderStatus::cancelled)) ||
        (order.status == CustomerOrderStatus::active &&
         (status == CustomerOrderStatus::completed ||
          status == CustomerOrderStatus::cancelled));
    if (!valid_transition)
        throw Error(ErrorCode::conflict, "This customer-order status transition is not allowed");

    if (status == CustomerOrderStatus::completed ||
        status == CustomerOrderStatus::cancelled) {
        Statement open_jobs(m_impl->db, R"SQL(
            SELECT 1 FROM print_jobs
            WHERE customer_order_id = ?
              AND state IN ('reserved', 'printing', 'needs_review')
            LIMIT 1
        )SQL");
        open_jobs.bind(1, order_id);
        if (open_jobs.step())
            throw Error(
                ErrorCode::conflict,
                "A customer order with open print jobs cannot be closed");
    }

    Statement statement(m_impl->db, R"SQL(
        UPDATE customer_orders
        SET status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, to_string(status));
    statement.bind(2, order_id);
    statement.execute();
    transaction.commit();
}

CustomerOrder Store::get_customer_order(const std::string &order_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->get_customer_order_unlocked(order_id);
}

std::vector<CustomerOrder> Store::list_customer_orders(
    const std::string &customer_id, bool include_closed) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!customer_id.empty())
        (void) m_impl->get_customer_unlocked(customer_id);
    std::string sql = R"SQL(
        SELECT id, customer_id, order_number, title, notes,
               quoted_price_micros, invoice_amount_micros, currency, status,
               design_time_seconds, design_hourly_rate_micros,
               other_cost_micros, discount_basis_points,
               bill_material, bill_electricity, bill_machine_wear,
               bill_maintenance, bill_repair_reserve, bill_design, bill_other,
               created_at, updated_at
        FROM customer_orders
    )SQL";
    bool has_where = false;
    if (!customer_id.empty()) {
        sql += " WHERE customer_id = ?";
        has_where = true;
    }
    if (!include_closed)
        sql += has_where ?
            " AND status IN ('draft', 'active')" :
            " WHERE status IN ('draft', 'active')";
    sql += " ORDER BY created_at DESC";

    Statement statement(m_impl->db, sql.c_str());
    if (!customer_id.empty())
        statement.bind(1, customer_id);
    std::vector<CustomerOrder> result;
    while (statement.step())
        result.emplace_back(m_impl->read_customer_order(statement));
    return result;
}

PrintJob Store::reserve_job(const PrintJobInput &job, const std::vector<AllocationInput> &allocations)
{
    const std::string normalized_job_key = trim_copy(job.idempotency_key);
    validate_operation_key(normalized_job_key);
    if (trim_copy(job.job_name).empty())
        throw Error(ErrorCode::validation, "Print job name must not be empty");
    if (allocations.empty())
        throw Error(ErrorCode::validation, "At least one spool allocation is required");
    if (job.estimated_runtime_seconds < 0)
        throw Error(ErrorCode::validation, "Estimated machine runtime must not be negative");
    if (job.machine_power_watts < 0)
        throw Error(ErrorCode::validation, "Machine power must not be negative");
    std::optional<std::string> customer_order_id;
    if (job.customer_order_id) {
        const std::string normalized = trim_copy(*job.customer_order_id);
        if (!normalized.empty())
            customer_order_id = normalized;
    }

    std::set<int> filament_indices;
    std::map<std::string, Milligrams> required_by_spool;
    for (const AllocationInput &allocation : allocations) {
        if (allocation.spool_id.empty())
            throw Error(ErrorCode::validation, "Every print filament must reference a spool");
        if (allocation.filament_index < 0)
            throw Error(ErrorCode::validation, "Filament index must not be negative");
        if (allocation.estimated_weight_mg <= 0)
            throw Error(ErrorCode::validation, "Estimated filament usage must be positive");
        if (!filament_indices.insert(allocation.filament_index).second)
            throw Error(ErrorCode::validation, "Each print filament may only be allocated once");

        Milligrams &sum = required_by_spool[allocation.spool_id];
        if (allocation.estimated_weight_mg > std::numeric_limits<Milligrams>::max() - sum)
            throw Error(ErrorCode::validation, "Estimated filament usage is too large");
        sum += allocation.estimated_weight_mg;
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    std::optional<PrintJob> existing = m_impl->find_job_by_key(normalized_job_key);
    const std::string existing_id = existing ? existing->id : std::string();
    const InventorySettings settings = m_impl->get_settings_unlocked();

    if (existing) {
        Statement manual_override(m_impl->db, R"SQL(
            SELECT 1
            FROM print_job_manual_overrides
            WHERE job_id = ?
        )SQL");
        manual_override.bind(1, existing->id);
        if (manual_override.step()) {
            transaction.commit();
            return *existing;
        }
    }

    std::string cost_currency = normalize_currency(settings.currency);
    if (customer_order_id) {
        const CustomerOrder order = m_impl->get_customer_order_unlocked(*customer_order_id);
        if (!existing && (order.status == CustomerOrderStatus::completed ||
                          order.status == CustomerOrderStatus::cancelled))
            throw Error(ErrorCode::conflict, "A closed customer order cannot receive another print job");
        cost_currency = order.currency;
    }

    if (existing) {
        const bool same_order = existing->customer_order_id == customer_order_id;
        const bool same_runtime =
            existing->estimated_runtime_seconds == job.estimated_runtime_seconds;
        const bool same_explicit_power =
            job.machine_power_watts == 0 ||
            existing->machine_power_watts == job.machine_power_watts;
        if (!same_order || !same_runtime || !same_explicit_power)
            throw Error(
                ErrorCode::conflict,
                "A print job's order, runtime, or machine power cannot be "
                "changed after reservation");
        cost_currency = existing->cost_currency;
    }

    if (existing && existing->state != JobState::reserved) {
        if (!Impl::allocations_match(existing->allocations, allocations))
            throw Error(ErrorCode::conflict, "A print that already started cannot be assigned to different spools");
        transaction.commit();
        return *existing;
    }

    const bool rebuild_allocations =
        !existing || !Impl::allocations_match(existing->allocations, allocations);
    std::map<std::string, Spool> spools;
    for (const auto &[spool_id, required] : required_by_spool) {
        const Spool spool = m_impl->get_spool_unlocked(spool_id);
        spools.emplace(spool_id, spool);
        if (spool.status == SpoolStatus::archived)
            throw Error(ErrorCode::conflict, "An archived spool cannot be reserved for printing");
        if (rebuild_allocations &&
            normalize_currency(spool.price_currency) != cost_currency)
            throw Error(
                ErrorCode::conflict,
                "Spool price currency does not match the print-job currency");
        const Milligrams available = checked_subtract(
            m_impl->physical_balance(spool_id),
            m_impl->active_reservations(spool_id, existing_id),
            "Available filament");
        if (available < required) {
            std::ostringstream message;
            message << "Spool '" << spool.name << "' has " << available
                    << " mg available but the print requires " << required << " mg";
            throw Error(ErrorCode::insufficient_stock, message.str());
        }
    }

    std::string job_id;
    if (existing) {
        job_id = existing->id;
        Statement update(m_impl->db, R"SQL(
            UPDATE print_jobs
            SET job_name = ?, project_path = ?, printer_id = ?,
                updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
            WHERE id = ?
        )SQL");
        update.bind(1, trim_copy(job.job_name));
        update.bind(2, job.project_path);
        update.bind(3, job.printer_id);
        update.bind(4, job_id);
        update.execute();

        if (!Impl::allocations_match(existing->allocations, allocations)) {
            Statement remove(m_impl->db, "DELETE FROM allocations WHERE job_id = ?");
            remove.bind(1, job_id);
            remove.execute();
        } else {
            transaction.commit();
            return m_impl->get_job_unlocked(job_id);
        }
    } else {
        job_id = make_uuid();
        const std::int64_t machine_power_watts =
            job.machine_power_watts > 0 ?
                job.machine_power_watts : settings.default_machine_power_watts;
        const MoneyMicros power_cost = electricity_cost(
            settings.electricity_price_per_kwh_micros,
            machine_power_watts,
            job.estimated_runtime_seconds);
        const MoneyMicros wear_cost = hourly_cost(
            settings.machine_wear_per_hour_micros, job.estimated_runtime_seconds,
            "Machine wear cost");
        const MoneyMicros maintenance_cost = hourly_cost(
            settings.maintenance_per_hour_micros, job.estimated_runtime_seconds,
            "Maintenance cost");
        const MoneyMicros repair_cost = hourly_cost(
            settings.repair_reserve_per_hour_micros, job.estimated_runtime_seconds,
            "Repair reserve cost");
        Statement insert(m_impl->db, R"SQL(
            INSERT INTO print_jobs (
                id, idempotency_key, job_name, project_path, printer_id,
                customer_order_id, state, cost_currency,
                electricity_price_per_kwh_micros, machine_power_watts,
                estimated_runtime_seconds, electricity_cost_micros,
                machine_wear_per_hour_micros, maintenance_per_hour_micros,
                repair_reserve_per_hour_micros, machine_wear_cost_micros,
                maintenance_cost_micros, repair_reserve_cost_micros
            ) VALUES (?, ?, ?, ?, ?, ?, 'reserved', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )SQL");
        insert.bind(1, job_id);
        insert.bind(2, normalized_job_key);
        insert.bind(3, trim_copy(job.job_name));
        insert.bind(4, job.project_path);
        insert.bind(5, job.printer_id);
        if (customer_order_id) insert.bind(6, *customer_order_id);
        else insert.bind_null(6);
        insert.bind(7, cost_currency);
        insert.bind(8, settings.electricity_price_per_kwh_micros);
        insert.bind(9, machine_power_watts);
        insert.bind(10, job.estimated_runtime_seconds);
        insert.bind(11, power_cost);
        insert.bind(12, settings.machine_wear_per_hour_micros);
        insert.bind(13, settings.maintenance_per_hour_micros);
        insert.bind(14, settings.repair_reserve_per_hour_micros);
        insert.bind(15, wear_cost);
        insert.bind(16, maintenance_cost);
        insert.bind(17, repair_cost);
        insert.execute();
    }

    for (const AllocationInput &allocation : allocations) {
        Statement row(m_impl->db, R"SQL(
            INSERT INTO allocations (
                id, job_id, spool_id, spool_name, manufacturer, material_type,
                filament_preset_id, color_hex,
                filament_index, estimated_weight_mg,
                material_price_per_kg_micros, cost_currency,
                estimated_material_cost_micros
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )SQL");
        const Spool &spool = spools.at(allocation.spool_id);
        const MoneyMicros estimated_cost = material_cost(
            spool.material_price_per_kg_micros, allocation.estimated_weight_mg);
        row.bind(1, make_uuid());
        row.bind(2, job_id);
        row.bind(3, allocation.spool_id);
        row.bind(4, spool.name);
        row.bind(5, spool.manufacturer);
        row.bind(6, spool.material_type);
        row.bind(7, spool.filament_preset_id);
        row.bind(8, spool.color_hex);
        row.bind(9, allocation.filament_index);
        row.bind(10, allocation.estimated_weight_mg);
        row.bind(11, spool.material_price_per_kg_micros);
        row.bind(12, cost_currency);
        row.bind(13, estimated_cost);
        row.execute();
    }

    transaction.commit();
    return m_impl->get_job_unlocked(job_id);
}

PrintJob Store::update_print_job(
    const std::string &job_id, const PrintJobUpdateInput &input)
{
    if (trim_copy(job_id).empty())
        throw Error(ErrorCode::validation, "Print job ID must not be empty");
    const std::string job_name = trim_copy(input.job_name);
    if (job_name.empty())
        throw Error(ErrorCode::validation, "Print job name must not be empty");
    if (input.estimated_runtime_seconds < 0)
        throw Error(
            ErrorCode::validation,
            "Estimated machine runtime must not be negative");
    if (input.machine_power_watts < 0)
        throw Error(ErrorCode::validation, "Machine power must not be negative");
    if (input.allocations.empty())
        throw Error(
            ErrorCode::validation,
            "At least one spool allocation is required");

    std::optional<std::string> customer_order_id;
    if (input.customer_order_id) {
        const std::string normalized = trim_copy(*input.customer_order_id);
        if (!normalized.empty())
            customer_order_id = normalized;
    }

    std::set<int> filament_indices;
    std::map<std::string, Milligrams> required_by_spool;
    for (const AllocationInput &allocation : input.allocations) {
        if (allocation.spool_id.empty())
            throw Error(
                ErrorCode::validation,
                "Every print filament must reference a spool");
        if (allocation.filament_index < 0)
            throw Error(
                ErrorCode::validation,
                "Filament index must not be negative");
        if (allocation.estimated_weight_mg <= 0)
            throw Error(
                ErrorCode::validation,
                "Estimated filament usage must be positive");
        if (!filament_indices.insert(allocation.filament_index).second)
            throw Error(
                ErrorCode::validation,
                "Each print filament may only be allocated once");

        Milligrams &sum = required_by_spool[allocation.spool_id];
        if (allocation.estimated_weight_mg >
            std::numeric_limits<Milligrams>::max() - sum)
            throw Error(
                ErrorCode::validation,
                "Estimated filament usage is too large");
        sum += allocation.estimated_weight_mg;
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    const PrintJob existing = m_impl->get_job_unlocked(job_id);
    const bool allocations_changed =
        !Impl::allocations_match(existing.allocations, input.allocations);
    const bool print_parameters_changed =
        existing.estimated_runtime_seconds !=
            input.estimated_runtime_seconds ||
        existing.machine_power_watts != input.machine_power_watts;
    const bool manually_changed =
        existing.job_name != job_name ||
        existing.project_path != input.project_path ||
        existing.printer_id != input.printer_id ||
        existing.customer_order_id != customer_order_id ||
        print_parameters_changed || allocations_changed;
    if (existing.state != JobState::reserved &&
        print_parameters_changed)
        throw Error(
            ErrorCode::conflict,
            "Print parameters can only be changed before printing starts");
    if (allocations_changed && existing.state != JobState::reserved)
        throw Error(
            ErrorCode::conflict,
            "Material assignments can only be changed before printing starts");
    if (!manually_changed) {
        transaction.commit();
        return existing;
    }

    if (customer_order_id) {
        const CustomerOrder order =
            m_impl->get_customer_order_unlocked(*customer_order_id);
        if (existing.customer_order_id != customer_order_id &&
            (order.status == CustomerOrderStatus::completed ||
             order.status == CustomerOrderStatus::cancelled))
            throw Error(
                ErrorCode::conflict,
                "A closed customer order cannot receive another print job");
        if (normalize_currency(order.currency) !=
            normalize_currency(existing.cost_currency))
            throw Error(
                ErrorCode::conflict,
                "Customer-order currency does not match the print-job currency");
    }

    std::map<std::string, Spool> spools;
    if (allocations_changed) {
        for (const auto &[spool_id, required] : required_by_spool) {
            const Spool spool = m_impl->get_spool_unlocked(spool_id);
            spools.emplace(spool_id, spool);
            if (spool.status == SpoolStatus::archived)
                throw Error(
                    ErrorCode::conflict,
                    "An archived spool cannot be assigned to a print job");
            if (normalize_currency(spool.price_currency) !=
                normalize_currency(existing.cost_currency))
                throw Error(
                    ErrorCode::conflict,
                    "Spool price currency does not match the print-job currency");
            const Milligrams available = checked_subtract(
                m_impl->physical_balance(spool_id),
                m_impl->active_reservations(spool_id, existing.id),
                "Available filament");
            if (available < required) {
                std::ostringstream message;
                message << "Spool '" << spool.name << "' has " << available
                        << " mg available but the print requires " << required
                        << " mg";
                throw Error(ErrorCode::insufficient_stock, message.str());
            }
        }
    }

    const MoneyMicros power_cost = electricity_cost(
        existing.electricity_price_per_kwh_micros,
        input.machine_power_watts,
        input.estimated_runtime_seconds);
    const MoneyMicros wear_cost = hourly_cost(
        existing.machine_wear_per_hour_micros, input.estimated_runtime_seconds,
        "Machine wear cost");
    const MoneyMicros maintenance_cost = hourly_cost(
        existing.maintenance_per_hour_micros, input.estimated_runtime_seconds,
        "Maintenance cost");
    const MoneyMicros repair_cost = hourly_cost(
        existing.repair_reserve_per_hour_micros, input.estimated_runtime_seconds,
        "Repair reserve cost");
    Statement update(m_impl->db, R"SQL(
        UPDATE print_jobs
        SET job_name = ?, project_path = ?, printer_id = ?,
            customer_order_id = ?, estimated_runtime_seconds = ?,
            machine_power_watts = ?, electricity_cost_micros = ?,
            machine_wear_cost_micros = ?, maintenance_cost_micros = ?,
            repair_reserve_cost_micros = ?,
            updated_at = CASE
                WHEN state = 'printing' THEN updated_at
                ELSE strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
            END
        WHERE id = ?
    )SQL");
    update.bind(1, job_name);
    update.bind(2, input.project_path);
    update.bind(3, input.printer_id);
    if (customer_order_id)
        update.bind(4, *customer_order_id);
    else
        update.bind_null(4);
    update.bind(5, input.estimated_runtime_seconds);
    update.bind(6, input.machine_power_watts);
    update.bind(7, power_cost);
    update.bind(8, wear_cost);
    update.bind(9, maintenance_cost);
    update.bind(10, repair_cost);
    update.bind(11, existing.id);
    update.execute();

    if (allocations_changed) {
        Statement remove(
            m_impl->db, "DELETE FROM allocations WHERE job_id = ?");
        remove.bind(1, existing.id);
        remove.execute();

        for (const AllocationInput &allocation : input.allocations) {
            const Spool &spool = spools.at(allocation.spool_id);
            const MoneyMicros estimated_cost = material_cost(
                spool.material_price_per_kg_micros,
                allocation.estimated_weight_mg);
            Statement row(m_impl->db, R"SQL(
                INSERT INTO allocations (
                    id, job_id, spool_id, spool_name, manufacturer,
                    material_type, filament_preset_id, color_hex,
                    filament_index, estimated_weight_mg,
                    material_price_per_kg_micros, cost_currency,
                    estimated_material_cost_micros
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            )SQL");
            row.bind(1, make_uuid());
            row.bind(2, existing.id);
            row.bind(3, allocation.spool_id);
            row.bind(4, spool.name);
            row.bind(5, spool.manufacturer);
            row.bind(6, spool.material_type);
            row.bind(7, spool.filament_preset_id);
            row.bind(8, spool.color_hex);
            row.bind(9, allocation.filament_index);
            row.bind(10, allocation.estimated_weight_mg);
            row.bind(11, spool.material_price_per_kg_micros);
            row.bind(12, existing.cost_currency);
            row.bind(13, estimated_cost);
            row.execute();
        }
    }

    Statement manual_override(m_impl->db, R"SQL(
        INSERT OR IGNORE INTO print_job_manual_overrides (job_id)
        VALUES (?)
    )SQL");
    manual_override.bind(1, existing.id);
    manual_override.execute();

    transaction.commit();
    return m_impl->get_job_unlocked(existing.id);
}

PrintJob Store::get_job(const std::string &job_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->get_job_unlocked(job_id);
}

std::vector<PrintJob> Store::list_open_jobs() const
{
    return list_jobs(false);
}

std::vector<PrintJob> Store::list_jobs(bool include_closed, std::size_t limit) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    std::string sql = R"SQL(
        SELECT id, idempotency_key, job_name, project_path, printer_id,
               customer_order_id, state, cost_currency,
               electricity_price_per_kwh_micros, machine_power_watts,
               estimated_runtime_seconds, electricity_cost_micros,
               machine_wear_per_hour_micros, maintenance_per_hour_micros,
               repair_reserve_per_hour_micros, machine_wear_cost_micros,
               maintenance_cost_micros, repair_reserve_cost_micros,
               created_at, updated_at, started_at, completed_at,
               actual_runtime_seconds
        FROM print_jobs
    )SQL";
    if (!include_closed)
        sql += " WHERE state IN ('reserved', 'printing', 'needs_review')";
    sql += " ORDER BY created_at DESC";
    if (limit != 0)
        sql += " LIMIT ?";

    Statement statement(m_impl->db, sql.c_str());
    if (limit != 0) {
        if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw Error(ErrorCode::validation, "Print-job history limit is too large");
        statement.bind(1, static_cast<std::int64_t>(limit));
    }
    std::vector<PrintJob> result;
    while (statement.step())
        result.emplace_back(m_impl->read_job(statement));
    return result;
}

std::vector<PrintJob> Store::list_customer_order_jobs(
    const std::string &order_id, bool include_discarded) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    (void) m_impl->get_customer_order_unlocked(order_id);
    std::string sql = R"SQL(
        SELECT id, idempotency_key, job_name, project_path, printer_id,
               customer_order_id, state, cost_currency,
               electricity_price_per_kwh_micros, machine_power_watts,
               estimated_runtime_seconds, electricity_cost_micros,
               machine_wear_per_hour_micros, maintenance_per_hour_micros,
               repair_reserve_per_hour_micros, machine_wear_cost_micros,
               maintenance_cost_micros, repair_reserve_cost_micros,
               created_at, updated_at, started_at, completed_at,
               actual_runtime_seconds
        FROM print_jobs
        WHERE customer_order_id = ?
    )SQL";
    if (!include_discarded)
        sql += " AND state <> 'discarded'";
    sql += " ORDER BY created_at DESC, id";

    Statement statement(m_impl->db, sql.c_str());
    statement.bind(1, order_id);
    std::vector<PrintJob> result;
    while (statement.step())
        result.emplace_back(m_impl->read_job(statement));
    return result;
}

void Store::bind_job_identifier(const std::string &job_id, const std::string &provider,
                                const std::string &kind, const std::string &value)
{
    const std::string normalized_provider = trim_copy(provider);
    const std::string normalized_kind     = trim_copy(kind);
    const std::string normalized_value    = trim_copy(value);
    if (normalized_provider.empty() || normalized_kind.empty() || normalized_value.empty())
        throw Error(ErrorCode::validation, "Print-job identifier fields must not be empty");

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    (void) m_impl->get_job_unlocked(job_id);
    Statement existing(m_impl->db, R"SQL(
        SELECT job_id FROM job_identifiers WHERE provider = ? AND kind = ? AND value = ?
    )SQL");
    existing.bind(1, normalized_provider);
    existing.bind(2, normalized_kind);
    existing.bind(3, normalized_value);
    if (existing.step()) {
        if (existing.text(0) != job_id)
            throw Error(ErrorCode::conflict, "This external print-job identifier already belongs to another job");
        transaction.commit();
        return;
    }

    Statement statement(m_impl->db, R"SQL(
        INSERT INTO job_identifiers (provider, kind, value, job_id) VALUES (?, ?, ?, ?)
    )SQL");
    statement.bind(1, normalized_provider);
    statement.bind(2, normalized_kind);
    statement.bind(3, normalized_value);
    statement.bind(4, job_id);
    statement.execute();
    transaction.commit();
}

std::optional<PrintJob> Store::find_job(const std::string &provider, const std::string &kind,
                                        const std::string &value) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Statement statement(m_impl->db, R"SQL(
        SELECT job_id FROM job_identifiers WHERE provider = ? AND kind = ? AND value = ?
    )SQL");
    statement.bind(1, trim_copy(provider));
    statement.bind(2, trim_copy(kind));
    statement.bind(3, trim_copy(value));
    if (!statement.step())
        return std::nullopt;
    return m_impl->get_job_unlocked(statement.text(0));
}

void Store::mark_printing(const std::string &job_id)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    const PrintJob job = m_impl->get_job_unlocked(job_id);
    if (job.state == JobState::printing && !job.started_at.empty()) {
        transaction.commit();
        return;
    }
    if (job.state == JobState::completed || job.state == JobState::discarded)
        throw Error(ErrorCode::conflict, "A closed print job cannot return to printing");

    // updated_at is also the start of the current observed printing segment.
    // Keeping started_at unchanged preserves the first start shown in history,
    // while review pauses can be excluded from the accumulated runtime.
    Statement statement(m_impl->db, R"SQL(
        UPDATE print_jobs
        SET state = 'printing',
            started_at = CASE
                WHEN started_at = '' THEN strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
                ELSE started_at
            END,
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, job_id);
    statement.execute();
    transaction.commit();
}

void Store::mark_needs_review(const std::string &job_id)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    const PrintJob job = m_impl->get_job_unlocked(job_id);
    if (job.state == JobState::needs_review) {
        transaction.commit();
        return;
    }
    if (job.state == JobState::completed || job.state == JobState::discarded)
        throw Error(ErrorCode::conflict, "A closed print job cannot be marked for review");

    Statement statement(m_impl->db, R"SQL(
        UPDATE print_jobs
        SET state = 'needs_review',
            actual_runtime_seconds = CASE
                WHEN state = 'printing' AND started_at <> '' THEN
                    COALESCE(actual_runtime_seconds, 0) +
                    MAX(
                        0,
                        CAST(
                            (julianday('now') - julianday(updated_at)) * 86400.0
                            AS INTEGER
                        )
                    )
                WHEN actual_runtime_seconds IS NOT NULL
                    THEN actual_runtime_seconds
                WHEN started_at = '' THEN NULL
                ELSE MAX(
                    0,
                    CAST(
                        (julianday('now') - julianday(started_at)) * 86400.0
                        AS INTEGER
                    )
                )
            END,
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, job_id);
    statement.execute();
    transaction.commit();
}

void Store::commit_job(const std::string &job_id,
                       const std::vector<ActualConsumption> &actual_consumption)
{
    std::map<int, Milligrams> actual_by_filament;
    for (const ActualConsumption &actual : actual_consumption) {
        if (actual.filament_index < 0 || actual.weight_mg < 0)
            throw Error(ErrorCode::validation, "Corrected filament consumption is invalid");
        if (!actual_by_filament.emplace(actual.filament_index, actual.weight_mg).second)
            throw Error(ErrorCode::validation, "Corrected filament consumption contains a duplicate index");
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    PrintJob job = m_impl->get_job_unlocked(job_id);

    std::set<int> known_indices;
    for (const Allocation &allocation : job.allocations)
        known_indices.insert(allocation.filament_index);
    for (const auto &[index, weight] : actual_by_filament) {
        (void) weight;
        if (known_indices.count(index) == 0)
            throw Error(ErrorCode::validation, "Corrected consumption references an unknown filament index");
    }

    if (job.state == JobState::completed) {
        for (const Allocation &allocation : job.allocations) {
            const auto corrected = actual_by_filament.find(allocation.filament_index);
            const Milligrams requested = corrected != actual_by_filament.end() ?
                                         corrected->second : allocation.estimated_weight_mg;
            if (!allocation.actual_weight_mg || *allocation.actual_weight_mg != requested)
                throw Error(ErrorCode::conflict,
                            "Completed print job was already committed with different consumption");
        }
        transaction.commit();
        return;
    }
    if (job.state == JobState::discarded)
        throw Error(ErrorCode::conflict, "A discarded print job cannot consume filament");

    for (const Allocation &allocation : job.allocations) {
        const auto actual = actual_by_filament.find(allocation.filament_index);
        const Milligrams consumed = actual != actual_by_filament.end() ?
                                    actual->second : allocation.estimated_weight_mg;
        const MoneyMicros consumed_cost = material_cost(
            allocation.material_price_per_kg_micros, consumed);
        m_impl->add_stock_event(
            allocation.spool_id,
            job.id,
            allocation.id,
            "consumption",
            -consumed,
            "consume:" + allocation.id,
            "Filament consumed by print job " + job.job_name
        );

        Statement update(m_impl->db, R"SQL(
            UPDATE allocations
            SET actual_weight_mg = ?, actual_material_cost_micros = ?
            WHERE id = ?
        )SQL");
        update.bind(1, consumed);
        update.bind(2, consumed_cost);
        update.bind(3, allocation.id);
        update.execute();
    }

    Statement close(m_impl->db, R"SQL(
        UPDATE print_jobs
        SET state = 'completed',
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
            completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
            actual_runtime_seconds = CASE
                WHEN state = 'printing' AND started_at <> '' THEN
                    COALESCE(actual_runtime_seconds, 0) +
                    MAX(
                        0,
                        CAST(
                            (julianday('now') - julianday(updated_at)) * 86400.0
                            AS INTEGER
                        )
                    )
                WHEN actual_runtime_seconds IS NOT NULL
                    THEN actual_runtime_seconds
                WHEN started_at = '' THEN NULL
                ELSE MAX(
                    0,
                    CAST(
                        (julianday('now') - julianday(started_at)) * 86400.0
                        AS INTEGER
                    )
                )
            END
        WHERE id = ?
    )SQL");
    close.bind(1, job_id);
    close.execute();
    transaction.commit();
}

void Store::discard_job(const std::string &job_id)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    Transaction transaction(m_impl->db);
    const PrintJob job = m_impl->get_job_unlocked(job_id);
    if (job.state == JobState::discarded) {
        transaction.commit();
        return;
    }
    if (job.state == JobState::completed)
        throw Error(ErrorCode::conflict, "A completed print job cannot be discarded");

    Statement statement(m_impl->db, R"SQL(
        UPDATE print_jobs
        SET state = 'discarded',
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
            completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?
    )SQL");
    statement.bind(1, job_id);
    statement.execute();
    transaction.commit();
}

namespace {

CostSummary summarize_jobs(const std::vector<PrintJob> &jobs, const std::string &currency)
{
    CostSummary summary;
    summary.currency = normalize_currency(currency);
    summary.actual_material_cost_micros = 0;
    for (const PrintJob &job : jobs) {
        if (job.state == JobState::discarded)
            continue;
        if (normalize_currency(job.cost_currency) != summary.currency)
            throw Error(ErrorCode::conflict, "Costs in different currencies cannot be combined");
        summary.electricity_cost_micros = checked_add(
            summary.electricity_cost_micros,
            job.electricity_cost_micros,
            "Electricity cost total");
        summary.machine_wear_cost_micros = checked_add(
            summary.machine_wear_cost_micros, job.machine_wear_cost_micros,
            "Machine wear cost total");
        summary.maintenance_cost_micros = checked_add(
            summary.maintenance_cost_micros, job.maintenance_cost_micros,
            "Maintenance cost total");
        summary.repair_reserve_cost_micros = checked_add(
            summary.repair_reserve_cost_micros, job.repair_reserve_cost_micros,
            "Repair reserve total");
        for (const Allocation &allocation : job.allocations) {
            if (normalize_currency(allocation.cost_currency) != summary.currency)
                throw Error(ErrorCode::database, "Allocation and print-job currencies do not match");
            summary.estimated_material_cost_micros = checked_add(
                summary.estimated_material_cost_micros,
                allocation.estimated_material_cost_micros,
                "Estimated material cost total");
            summary.material_cost_micros = checked_add(
                summary.material_cost_micros,
                allocation.actual_material_cost_micros.value_or(
                    allocation.estimated_material_cost_micros),
                "Material cost total");
            if (summary.actual_material_cost_micros) {
                if (allocation.actual_material_cost_micros) {
                    *summary.actual_material_cost_micros = checked_add(
                        *summary.actual_material_cost_micros,
                        *allocation.actual_material_cost_micros,
                        "Actual material cost total");
                } else {
                    summary.actual_material_cost_micros.reset();
                }
            }
        }
    }
    summary.total_cost_micros = checked_add(
        summary.material_cost_micros,
        summary.electricity_cost_micros,
        "Print cost total");
    summary.total_cost_micros = checked_add(
        summary.total_cost_micros, summary.machine_wear_cost_micros,
        "Print cost total");
    summary.total_cost_micros = checked_add(
        summary.total_cost_micros, summary.maintenance_cost_micros,
        "Print cost total");
    summary.total_cost_micros = checked_add(
        summary.total_cost_micros, summary.repair_reserve_cost_micros,
        "Print cost total");
    summary.billable_subtotal_micros = summary.total_cost_micros;
    summary.calculated_invoice_micros = summary.total_cost_micros;
    return summary;
}

void apply_order_costs(CostSummary &summary, const CustomerOrder &order)
{
    summary.design_cost_micros = hourly_cost(
        order.design_hourly_rate_micros, order.design_time_seconds,
        "Design cost");
    summary.other_cost_micros = order.other_cost_micros;
    summary.total_cost_micros = checked_add(
        summary.total_cost_micros, summary.design_cost_micros,
        "Order cost total");
    summary.total_cost_micros = checked_add(
        summary.total_cost_micros, summary.other_cost_micros,
        "Order cost total");

    MoneyMicros billable = 0;
    const auto include = [&billable](bool enabled, MoneyMicros value,
                                     const char *context) {
        if (enabled)
            billable = checked_add(billable, value, context);
    };
    include(order.bill_material, summary.material_cost_micros, "Billable total");
    include(order.bill_electricity, summary.electricity_cost_micros, "Billable total");
    include(order.bill_machine_wear, summary.machine_wear_cost_micros, "Billable total");
    include(order.bill_maintenance, summary.maintenance_cost_micros, "Billable total");
    include(order.bill_repair_reserve, summary.repair_reserve_cost_micros, "Billable total");
    include(order.bill_design, summary.design_cost_micros, "Billable total");
    include(order.bill_other, summary.other_cost_micros, "Billable total");
    summary.billable_subtotal_micros = billable;
    summary.discount_micros = scaled_cost(
        {billable, order.discount_basis_points}, 10'000, "Discount");
    summary.calculated_invoice_micros =
        billable - summary.discount_micros;
}

} // namespace

std::vector<MaterialUsageSummary> summarize_material_usage(
    const std::vector<PrintJob> &jobs)
{
    using MaterialKey = std::tuple<
        std::string, std::string, std::string, std::string, std::string,
        std::string>;
    std::map<MaterialKey, MaterialUsageSummary> summaries;
    for (const PrintJob &job : jobs) {
        if (job.state == JobState::discarded)
            continue;
        for (const Allocation &allocation : job.allocations) {
            const std::string currency =
                normalize_currency(allocation.cost_currency);
            const std::string fallback_name =
                allocation.manufacturer.empty() &&
                        allocation.material_type.empty() &&
                        allocation.filament_preset_id.empty() ?
                    allocation.spool_name :
                    std::string {};
            const MaterialKey key {
                allocation.manufacturer,
                allocation.material_type,
                allocation.filament_preset_id,
                allocation.color_hex,
                currency,
                fallback_name
            };
            auto [found, inserted] =
                summaries.try_emplace(key, MaterialUsageSummary {});
            MaterialUsageSummary &summary = found->second;
            if (inserted) {
                summary.spool_name        = fallback_name;
                summary.manufacturer      = allocation.manufacturer;
                summary.material_type     = allocation.material_type;
                summary.filament_preset_id = allocation.filament_preset_id;
                summary.color_hex         = allocation.color_hex;
                summary.cost_currency     = currency;
            }
            summary.estimated_weight_mg = checked_add(
                summary.estimated_weight_mg,
                allocation.estimated_weight_mg,
                "Estimated material weight total");
            summary.best_known_weight_mg = checked_add(
                summary.best_known_weight_mg,
                allocation.actual_weight_mg.value_or(
                    allocation.estimated_weight_mg),
                "Material weight total");
            summary.estimated_material_cost_micros = checked_add(
                summary.estimated_material_cost_micros,
                allocation.estimated_material_cost_micros,
                "Estimated material cost total");
            summary.best_known_material_cost_micros = checked_add(
                summary.best_known_material_cost_micros,
                allocation.actual_material_cost_micros.value_or(
                    allocation.estimated_material_cost_micros),
                "Material cost total");
            summary.weight_fully_confirmed &=
                allocation.actual_weight_mg.has_value();
            summary.cost_fully_confirmed &=
                allocation.actual_material_cost_micros.has_value();
        }
    }

    std::vector<MaterialUsageSummary> result;
    result.reserve(summaries.size());
    for (auto &entry : summaries)
        result.emplace_back(std::move(entry.second));
    return result;
}

CostSummary Store::job_cost_summary(const std::string &job_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const PrintJob job = m_impl->get_job_unlocked(job_id);
    return summarize_jobs({job}, job.cost_currency);
}

CostSummary Store::customer_order_cost_summary(const std::string &order_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const CustomerOrder order = m_impl->get_customer_order_unlocked(order_id);
    Statement statement(m_impl->db, R"SQL(
        SELECT id FROM print_jobs
        WHERE customer_order_id = ?
        ORDER BY created_at, id
    )SQL");
    statement.bind(1, order_id);
    std::vector<PrintJob> jobs;
    while (statement.step())
        jobs.emplace_back(m_impl->get_job_unlocked(statement.text(0)));
    CostSummary summary = summarize_jobs(jobs, order.currency);
    apply_order_costs(summary, order);
    summary.quoted_price_micros = order.quoted_price_micros;
    summary.invoice_amount_micros = order.invoice_amount_micros;
    return summary;
}

std::vector<InvoiceLine> Store::customer_order_invoice_lines(
    const std::string &order_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const CustomerOrder order = m_impl->get_customer_order_unlocked(order_id);
    Statement statement(m_impl->db, R"SQL(
        SELECT id FROM print_jobs
        WHERE customer_order_id = ?
        ORDER BY created_at, id
    )SQL");
    statement.bind(1, order_id);
    std::vector<PrintJob> jobs;
    while (statement.step())
        jobs.emplace_back(m_impl->get_job_unlocked(statement.text(0)));

    const CostSummary costs = [&] {
        CostSummary result = summarize_jobs(jobs, order.currency);
        apply_order_costs(result, order);
        return result;
    }();
    std::vector<InvoiceLine> lines;
    for (const MaterialUsageSummary &material : summarize_material_usage(jobs)) {
        std::string description;
        if (!material.manufacturer.empty())
            description = material.manufacturer;
        if (!material.material_type.empty()) {
            if (!description.empty()) description += " ";
            description += material.material_type;
        }
        if (!material.filament_preset_id.empty()) {
            if (!description.empty()) description += " - ";
            description += material.filament_preset_id;
        }
        if (description.empty())
            description = material.spool_name.empty() ? "Filament" : material.spool_name;
        std::ostringstream detail;
        detail.imbue(std::locale::classic());
        detail << std::fixed << std::setprecision(1)
               << material.best_known_weight_mg / 1'000.0 << " g";
        lines.push_back({
            InvoiceCostCategory::material, description, detail.str(),
            material.color_hex, material.best_known_material_cost_micros,
            order.bill_material ? material.best_known_material_cost_micros : 0,
            order.bill_material});
    }
    const auto add = [&lines](InvoiceCostCategory category,
                              const char *description, const char *detail,
                              MoneyMicros amount, bool included) {
        lines.push_back({category, description, detail, {}, amount,
                         included ? amount : 0, included});
    };
    add(InvoiceCostCategory::electricity, "Electricity", "Calculated from print runtime",
        costs.electricity_cost_micros, order.bill_electricity);
    add(InvoiceCostCategory::machine_wear, "Machine wear", "Runtime-based allowance",
        costs.machine_wear_cost_micros, order.bill_machine_wear);
    add(InvoiceCostCategory::maintenance, "Maintenance", "Runtime-based allowance",
        costs.maintenance_cost_micros, order.bill_maintenance);
    add(InvoiceCostCategory::repair_reserve, "Repair reserve", "Runtime-based allowance",
        costs.repair_reserve_cost_micros, order.bill_repair_reserve);
    std::ostringstream design_detail;
    design_detail.imbue(std::locale::classic());
    design_detail << std::fixed << std::setprecision(2)
                  << order.design_time_seconds / 3'600.0 << " h";
    lines.push_back({
        InvoiceCostCategory::design, "Design work", design_detail.str(), {},
        costs.design_cost_micros,
        order.bill_design ? costs.design_cost_micros : 0,
        order.bill_design});
    add(InvoiceCostCategory::other, "Other costs", "Order-specific costs",
        costs.other_cost_micros, order.bill_other);
    if (costs.discount_micros > 0) {
        std::ostringstream discount_detail;
        discount_detail.imbue(std::locale::classic());
        discount_detail << std::fixed << std::setprecision(2)
                        << order.discount_basis_points / 100.0 << " %";
        lines.push_back({
            InvoiceCostCategory::discount, "Discount", discount_detail.str(), {},
            0, -costs.discount_micros, true});
    }
    return lines;
}

CostSummary Store::customer_cost_summary(const std::string &customer_id) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    (void) m_impl->get_customer_unlocked(customer_id);
    Statement orders_statement(m_impl->db, R"SQL(
        SELECT id, customer_id, order_number, title, notes,
               quoted_price_micros, invoice_amount_micros, currency, status,
               design_time_seconds, design_hourly_rate_micros,
               other_cost_micros, discount_basis_points,
               bill_material, bill_electricity, bill_machine_wear,
               bill_maintenance, bill_repair_reserve, bill_design, bill_other,
               created_at, updated_at
        FROM customer_orders
        WHERE customer_id = ?
        ORDER BY created_at, id
    )SQL");
    orders_statement.bind(1, customer_id);

    std::vector<CustomerOrder> orders;
    while (orders_statement.step())
        orders.emplace_back(m_impl->read_customer_order(orders_statement));
    const std::string currency = orders.empty() ?
        m_impl->get_settings_unlocked().currency : orders.front().currency;

    std::optional<MoneyMicros> quoted_total;
    std::optional<MoneyMicros> invoice_total;
    CostSummary summary;
    summary.currency = currency;
    summary.actual_material_cost_micros = 0;
    for (const CustomerOrder &order : orders) {
        if (normalize_currency(order.currency) != normalize_currency(currency))
            throw Error(ErrorCode::conflict, "Customer orders in different currencies cannot be combined");
        if (order.quoted_price_micros) {
            quoted_total = checked_add(
                quoted_total.value_or(0),
                *order.quoted_price_micros,
                "Quoted-price total");
        }
        if (order.invoice_amount_micros) {
            invoice_total = checked_add(
                invoice_total.value_or(0),
                *order.invoice_amount_micros,
                "Invoice total");
        }
        Statement jobs_statement(m_impl->db, R"SQL(
            SELECT id FROM print_jobs
            WHERE customer_order_id = ?
            ORDER BY created_at, id
        )SQL");
        jobs_statement.bind(1, order.id);
        std::vector<PrintJob> order_jobs;
        while (jobs_statement.step())
            order_jobs.emplace_back(m_impl->get_job_unlocked(jobs_statement.text(0)));
        CostSummary order_summary = summarize_jobs(order_jobs, currency);
        apply_order_costs(order_summary, order);
        const auto add = [&summary](MoneyMicros &target, MoneyMicros value,
                                    const char *context) {
            target = checked_add(target, value, context);
        };
        add(summary.estimated_material_cost_micros, order_summary.estimated_material_cost_micros, "Customer cost total");
        add(summary.material_cost_micros, order_summary.material_cost_micros, "Customer cost total");
        add(summary.electricity_cost_micros, order_summary.electricity_cost_micros, "Customer cost total");
        add(summary.machine_wear_cost_micros, order_summary.machine_wear_cost_micros, "Customer cost total");
        add(summary.maintenance_cost_micros, order_summary.maintenance_cost_micros, "Customer cost total");
        add(summary.repair_reserve_cost_micros, order_summary.repair_reserve_cost_micros, "Customer cost total");
        add(summary.design_cost_micros, order_summary.design_cost_micros, "Customer cost total");
        add(summary.other_cost_micros, order_summary.other_cost_micros, "Customer cost total");
        add(summary.total_cost_micros, order_summary.total_cost_micros, "Customer cost total");
        add(summary.billable_subtotal_micros, order_summary.billable_subtotal_micros, "Customer billable total");
        add(summary.discount_micros, order_summary.discount_micros, "Customer discount total");
        add(summary.calculated_invoice_micros, order_summary.calculated_invoice_micros, "Customer invoice total");
        if (summary.actual_material_cost_micros) {
            if (order_summary.actual_material_cost_micros)
                *summary.actual_material_cost_micros = checked_add(
                    *summary.actual_material_cost_micros,
                    *order_summary.actual_material_cost_micros,
                    "Customer actual material total");
            else
                summary.actual_material_cost_micros.reset();
        }
    }
    summary.quoted_price_micros = quoted_total;
    summary.invoice_amount_micros = invoice_total;
    return summary;
}

} // namespace Slic3r::FilamentInventory
