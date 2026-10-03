#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/FilamentReservationPlan.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::FilamentInventory;

namespace {
const std::string roll = "11111111-2222-4333-8444-555555555555";
const std::string other_roll = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

FilamentReservationContext context(bool fixed = true)
{
    FilamentReservationContext value;
    value.job_name = "Customer parts";
    value.project_path = "customer-parts.3mf";
    value.printer_id = "paired-printer";
    value.estimated_runtime_seconds = 7200;
    FilamentInventoryUsage usage;
    usage.filament_index = 2;
    usage.estimated_weight_mg = 3500;
    // A project override must not trigger spool reselection by profile/color.
    usage.filament_preset_id = "Local flow override";
    usage.color_hex = "#010203";
    value.usages.push_back(usage);
    if (fixed) value.fixed_allocations = {{roll, 2, 3500}};
    return value;
}
}

TEST_CASE("Authoritative roll reservations retain the chosen customer order and runtime", "[FilamentReservationPlan]")
{
    const auto value = context();
    const auto plan = make_filament_reservation_plan(value, "one-launch", "customer-order", *value.fixed_allocations);
    REQUIRE(plan.job.customer_order_id);
    CHECK(*plan.job.customer_order_id == "customer-order");
    CHECK(plan.job.estimated_runtime_seconds == 7200);
    CHECK(plan.job.project_path == "customer-parts.3mf");
    CHECK(plan.job.idempotency_key == "one-launch");
    REQUIRE(plan.allocations.size() == 1);
    CHECK(plan.allocations.front().spool_id == roll);
    CHECK(plan.allocations.front().filament_index == 2);
    CHECK(plan.allocations.front().estimated_weight_mg == 3500);
}

TEST_CASE("Authoritative reservations reject bypasses and changed physical allocations", "[FilamentReservationPlan]")
{
    const auto value = context();
    CHECK_THROWS(make_filament_reservation_plan(value, "launch", {}, {}));
    CHECK_THROWS(make_filament_reservation_plan(value, "launch", {}, {{other_roll, 2, 3500}}));
    CHECK_THROWS(make_filament_reservation_plan(value, "launch", {}, {{roll, 2, 3501}}));
    CHECK_THROWS(make_filament_reservation_plan(value, "launch", {}, {{roll, 1, 3500}}));
}

TEST_CASE("Malformed authoritative reservations cannot replace sliced usage", "[FilamentReservationPlan]")
{
    auto value = context();
    value.fixed_allocations = std::vector<AllocationInput>{};
    CHECK_THROWS(make_filament_reservation_plan(value, "launch", {}, {}));
    value = context();
    value.fixed_allocations->front().spool_id = "not-a-uuid";
    CHECK_THROWS(make_filament_reservation_plan(value, "launch", {}, *value.fixed_allocations));
    value = context();
    value.fixed_allocations->front().estimated_weight_mg++;
    CHECK_THROWS(make_filament_reservation_plan(value, "launch", {}, *value.fixed_allocations));
    value = context();
    value.usages.push_back(value.usages.front());
    value.fixed_allocations->push_back(value.fixed_allocations->front());
    CHECK_THROWS(make_filament_reservation_plan(value, "launch", {}, *value.fixed_allocations));
}

TEST_CASE("One authoritative roll can serve multiple distinct material indices", "[FilamentReservationPlan]")
{
    auto value = context();
    auto second = value.usages.front();
    second.filament_index = 0;
    second.estimated_weight_mg = 2000;
    value.usages.push_back(second);
    value.fixed_allocations->push_back({roll, 0, 2000});
    const auto plan = make_filament_reservation_plan(value, "launch", {}, {{roll, 0, 2000}, {roll, 2, 3500}});
    REQUIRE(plan.allocations.size() == 2);
    CHECK_FALSE(plan.job.customer_order_id);
}

TEST_CASE("Ordinary reservations preserve free spool selection and optional customer orders", "[FilamentReservationPlan]")
{
    const auto value = context(false);
    const auto plan = make_filament_reservation_plan(value, "launch", {}, {{other_roll, 2, 3500}});
    CHECK_FALSE(plan.job.customer_order_id);
    REQUIRE(plan.allocations.size() == 1);
    CHECK(plan.allocations.front().spool_id == other_roll);
}
