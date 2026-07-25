#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <wx/string.h>

#include "libslic3r/FilamentInventory.hpp"

class wxWindow;

namespace Slic3r::GUI {

struct FilamentInventoryUsage {
    int         filament_index {0};
    std::string display_name;
    std::string manufacturer;
    std::string material_type;
    std::string filament_preset_id;
    std::string color_hex {"#FFFFFF"};
    double      diameter_mm {1.75};
    double      density_g_cm3 {1.24};
    FilamentInventory::Milligrams estimated_weight_mg {0};
    std::string suggested_bambu_tag_uid;
};

struct FilamentReservationContext {
    std::string job_name;
    std::string project_path;
    std::string printer_id;
    std::int64_t estimated_runtime_seconds {0};
    std::vector<FilamentInventoryUsage> usages;
};

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

} // namespace FilamentAllocationDetail

// A reserved result contains a job which has already been reserved atomically,
// so the caller may start dispatch immediately. Continuing without tracking
// deliberately does not create an inventory job.
FilamentReservationResult reserve_filament_for_print(
    wxWindow *parent, FilamentInventory::Store &store,
    const FilamentReservationContext &context);

} // namespace Slic3r::GUI
