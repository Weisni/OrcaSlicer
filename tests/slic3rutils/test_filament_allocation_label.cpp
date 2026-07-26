#include <catch2/catch_all.hpp>

#include <string>
#include <utility>
#include <vector>

#include "slic3r/GUI/FilamentAllocationDialog.hpp"

using Slic3r::FilamentInventory::Spool;
using Slic3r::FilamentInventory::SpoolStatus;
using Slic3r::GUI::FilamentInventoryUsage;
using Slic3r::GUI::FilamentAllocationDetail::find_unique_compatible_spool_id;
using Slic3r::GUI::FilamentAllocationDetail::format_spool_choice_label;

namespace {

Spool make_spool(
    std::string id, std::string color, std::int64_t available_weight_mg)
{
    Spool spool;
    spool.id                    = std::move(id);
    spool.manufacturer          = "Bambu Lab";
    spool.material_type         = "PETG";
    spool.filament_preset_id    = "GFG99";
    spool.color_hex             = std::move(color);
    spool.diameter_mm           = 1.75;
    spool.available_weight_mg   = available_weight_mg;
    spool.status                = SpoolStatus::active;
    return spool;
}

FilamentInventoryUsage make_usage()
{
    FilamentInventoryUsage usage;
    usage.manufacturer          = "Bambu Lab";
    usage.material_type         = "PETG";
    usage.filament_preset_id    = "GFG99";
    usage.color_hex             = "#FF144A";
    usage.diameter_mm           = 1.75;
    usage.estimated_weight_mg   = 100'000;
    return usage;
}

} // namespace

TEST_CASE("Filament allocation spool label preserves UTF-8 punctuation",
          "[FilamentAllocation][Encoding]")
{
    Spool spool;
    spool.name                = "Bambu Lab PETG";
    spool.material_type       = "PETG";
    spool.available_weight_mg = 462'700;

    const wxString label = format_spool_choice_label(spool);

    CHECK(std::string(label.utf8_str()) ==
          "Bambu Lab PETG \xE2\x80\x94 PETG, 462.7 g available");
}

TEST_CASE("Filament allocation automatically selects one compatible spool",
          "[FilamentAllocation][Matching]")
{
    const FilamentInventoryUsage usage = make_usage();
    const std::vector<Spool> spools {
        make_spool("red", "#FF144A", 500'000),
        make_spool("blue", "#0066FF", 500'000)
    };

    const auto match = find_unique_compatible_spool_id(usage, spools);
    REQUIRE(match);
    CHECK(*match == "red");
}

TEST_CASE("Filament allocation leaves ambiguous compatible spools unselected",
          "[FilamentAllocation][Matching]")
{
    const FilamentInventoryUsage usage = make_usage();
    const std::vector<Spool> spools {
        make_spool("first", "#FF144A", 500'000),
        make_spool("second", "#FF144A", 800'000)
    };

    CHECK_FALSE(find_unique_compatible_spool_id(usage, spools));
}

TEST_CASE("Filament allocation repeats a unique material match independently",
          "[FilamentAllocation][Matching]")
{
    const FilamentInventoryUsage usage = make_usage();
    std::vector<Spool> spools {make_spool("red", "#FF144A", 500'000)};

    const auto first_start = find_unique_compatible_spool_id(usage, spools);
    REQUIRE(first_start);
    spools.front().available_weight_mg = 50'000;
    const auto second_start = find_unique_compatible_spool_id(usage, spools);

    CHECK(second_start == first_start);
}

TEST_CASE("Filament allocation ignores conflicting and archived spools",
          "[FilamentAllocation][Matching]")
{
    FilamentInventoryUsage usage = make_usage();
    std::vector<Spool> spools {
        make_spool("wrong-color", "#0066FF", 500'000),
        make_spool("wrong-preset", "#FF144A", 500'000),
        make_spool("match", "#FF144A", 500'000)
    };
    spools[1].filament_preset_id = "OTHER";

    const auto match = find_unique_compatible_spool_id(usage, spools);
    REQUIRE(match);
    CHECK(*match == "match");

    spools.back().status = SpoolStatus::archived;
    CHECK_FALSE(find_unique_compatible_spool_id(usage, spools));
}

TEST_CASE("Filament allocation never uses stock level to resolve ambiguity",
          "[FilamentAllocation][Matching]")
{
    const FilamentInventoryUsage usage = make_usage();
    const std::vector<Spool> spools {
        make_spool("insufficient", "#FF144A", 50'000),
        make_spool("sufficient", "#FF144A", 500'000)
    };

    CHECK_FALSE(find_unique_compatible_spool_id(usage, spools));
}

TEST_CASE("Filament allocation tolerates missing legacy optional metadata",
          "[FilamentAllocation][Matching]")
{
    FilamentInventoryUsage usage = make_usage();
    Spool legacy = make_spool("legacy", "#FF144A", 500'000);
    legacy.manufacturer.clear();
    legacy.filament_preset_id.clear();

    const auto match = find_unique_compatible_spool_id(usage, {legacy});
    REQUIRE(match);
    CHECK(*match == "legacy");

    legacy.material_type = "PLA";
    CHECK_FALSE(find_unique_compatible_spool_id(usage, {legacy}));
}

TEST_CASE("Filament allocation keeps exact and legacy candidates ambiguous",
          "[FilamentAllocation][Matching]")
{
    const FilamentInventoryUsage usage = make_usage();
    Spool exact = make_spool("exact", "#FF144A", 500'000);
    Spool legacy = make_spool("legacy", "#FF144A", 500'000);
    legacy.manufacturer.clear();
    legacy.filament_preset_id.clear();

    CHECK_FALSE(find_unique_compatible_spool_id(usage, {exact, legacy}));
}

TEST_CASE("Filament allocation does not invent a color match",
          "[FilamentAllocation][Matching]")
{
    FilamentInventoryUsage usage = make_usage();
    usage.color_hex.clear();
    const std::vector<Spool> spools {
        make_spool("red", "#FF144A", 500'000),
        make_spool("blue", "#0066FF", 500'000)
    };

    CHECK_FALSE(find_unique_compatible_spool_id(usage, spools));
}
