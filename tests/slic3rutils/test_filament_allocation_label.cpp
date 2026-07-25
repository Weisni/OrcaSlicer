#include <catch2/catch_all.hpp>

#include <string>

#include "slic3r/GUI/FilamentAllocationDialog.hpp"

using Slic3r::FilamentInventory::Spool;
using Slic3r::GUI::FilamentAllocationDetail::format_spool_choice_label;

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
