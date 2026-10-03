#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <wx/string.h>

#include "libslic3r/FilamentInventory.hpp"
#include "FilamentReservationPlan.hpp"

class wxWindow;

namespace Slic3r::GUI {

enum class FilamentReservationDecision {
    reserved,
    without_tracking,
    cancelled
};

struct FilamentReservationResult {
    FilamentReservationDecision decision {FilamentReservationDecision::cancelled};
    std::optional<FilamentInventory::PrintJob> job;
};

namespace FilamentAllocationDetail {

wxString format_spool_choice_label(const FilamentInventory::Spool &spool);

// Returns a spool only when exactly one listed spool agrees with all known
// material-identity metadata. Stock is deliberately validated later and never
// used to choose between otherwise identical physical spools.
std::optional<std::string> find_unique_compatible_spool_id(
    const FilamentInventoryUsage &usage,
    const std::vector<FilamentInventory::Spool> &spools);

} // namespace FilamentAllocationDetail

// A reserved result contains a job which has already been reserved atomically,
// so the caller may start dispatch immediately. Continuing without tracking
// deliberately does not create an inventory job.
FilamentReservationResult reserve_filament_for_print(
    wxWindow *parent, FilamentInventory::Store &store,
    const FilamentReservationContext &context);

} // namespace Slic3r::GUI
