#pragma once

#include <optional>
#include <vector>

#include "libslic3r/FilamentInventory.hpp"

class wxWindow;

namespace Slic3r::GUI {

// Shared by the manager and print-allocation dialog so spool validation and
// initial-fill handling stay identical in every creation flow.
std::optional<FilamentInventory::Spool> create_filament_spool_interactively(
    wxWindow *parent, FilamentInventory::Store &store,
    const FilamentInventory::SpoolInput *defaults = nullptr,
    const std::vector<FilamentInventory::SpoolIdentifierInput> *identifier_defaults = nullptr);

} // namespace Slic3r::GUI
