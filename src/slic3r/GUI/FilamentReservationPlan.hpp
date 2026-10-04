#pragma once

#include <functional>
#include <cctype>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "libslic3r/FilamentInventory.hpp"

namespace Slic3r::GUI {

struct FilamentInventoryUsage {
    int         filament_index {0};
    std::string display_name;
    std::string manufacturer;
    std::string material_type;
    std::string filament_preset_id;
    std::string color_hex;
    double      diameter_mm {0.0};
    double      density_g_cm3 {0.0};
    FilamentInventory::Milligrams estimated_weight_mg {0};
    std::string suggested_bambu_tag_uid;
};

struct FilamentReservationContext {
    std::string job_name;
    std::string project_path;
    std::string printer_id;
    std::int64_t estimated_runtime_seconds {0};
    std::vector<FilamentInventoryUsage> usages;
    // Present only after an authoritative provider has validated the physical
    // UUID/slot mapping. The order chooser remains part of both workflows.
    std::optional<std::vector<FilamentInventory::AllocationInput>> fixed_allocations;
    // Reconcile acknowledged/pending authority writes before the next operation.
    std::function<void()> refresh_inventory;
};

struct FilamentReservationPlan {
    FilamentInventory::PrintJobInput job;
    std::vector<FilamentInventory::AllocationInput> allocations;
};

inline FilamentReservationPlan make_filament_reservation_plan(
    const FilamentReservationContext &context, const std::string &launch_key,
    std::optional<std::string> order_id,
    const std::vector<FilamentInventory::AllocationInput> &selected)
{
    if (context.fixed_allocations) {
        const auto &fixed = *context.fixed_allocations;
        if (fixed.empty() || fixed.size() != context.usages.size() || selected.size() != fixed.size())
            throw std::runtime_error("Every sliced material needs its prevalidated HA roll allocation");
        std::map<int, FilamentInventory::Milligrams> requirements;
        for (const auto &usage : context.usages) {
            if (usage.filament_index < 0 || usage.estimated_weight_mg <= 0 ||
                !requirements.emplace(usage.filament_index, usage.estimated_weight_mg).second)
                throw std::runtime_error("Invalid or duplicate sliced material usage");
        }
        std::map<int, FilamentInventory::AllocationInput> expected;
        for (const auto &allocation : fixed) {
            const auto &id = allocation.spool_id;
            bool uuid = id.size() == 36;
            for (std::size_t index = 0; uuid && index < id.size(); ++index)
                uuid = (index == 8 || index == 13 || index == 18 || index == 23) ?
                    id[index] == '-' : std::isxdigit(static_cast<unsigned char>(id[index])) != 0;
            const auto usage = requirements.find(allocation.filament_index);
            if (!uuid || usage == requirements.end() || usage->second != allocation.estimated_weight_mg ||
                !expected.emplace(allocation.filament_index, allocation).second)
                throw std::runtime_error("HA roll identity or quantity differs from the sliced material");
        }
        for (const auto &allocation : selected) {
            const auto found = expected.find(allocation.filament_index);
            if (found == expected.end() || found->second.spool_id != allocation.spool_id ||
                found->second.estimated_weight_mg != allocation.estimated_weight_mg)
                throw std::runtime_error("The prevalidated HA roll allocation cannot be replaced in this dialog");
            expected.erase(found);
        }
    }
    FilamentInventory::PrintJobInput job {
        launch_key, context.job_name, context.project_path, context.printer_id
    };
    job.customer_order_id = std::move(order_id);
    job.estimated_runtime_seconds = context.estimated_runtime_seconds;
    return {std::move(job), selected};
}

} // namespace Slic3r::GUI
