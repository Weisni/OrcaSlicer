#pragma once

#include <optional>

#include "libslic3r/FilamentInventory.hpp"

class wxWindow;

namespace Slic3r::GUI {

std::optional<FilamentInventory::Customer> edit_customer_interactively(
    wxWindow *parent, FilamentInventory::Store &store,
    const FilamentInventory::Customer *customer = nullptr);

std::optional<FilamentInventory::CustomerOrder> edit_customer_order_interactively(
    wxWindow *parent, FilamentInventory::Store &store,
    const FilamentInventory::CustomerOrder *order = nullptr,
    const std::string &preferred_customer_id = {},
    std::optional<std::string> new_order_currency_override = std::nullopt);

bool edit_inventory_cost_settings_interactively(
    wxWindow *parent, FilamentInventory::Store &store);

} // namespace Slic3r::GUI
