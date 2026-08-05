#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "libslic3r/FilamentInventory.hpp"

class wxWindow;

namespace Slic3r::GUI {

struct InvoiceExportData {
    std::string seller_name;
    std::string seller_address;
    std::string seller_contact;
    std::string tax_identifier;
    std::string customer_name;
    std::string customer_address;
    std::string invoice_number;
    std::string invoice_date;
    std::string service_date;
    std::string due_date;
    std::string order_title;
    std::string currency {"EUR"};
    std::int64_t vat_basis_points {1'900};
    bool small_business {false};
    std::vector<FilamentInventory::InvoiceLine> lines;
};

std::string invoice_plain_text(const InvoiceExportData &data);
void write_invoice_pdf(const std::string &path, const InvoiceExportData &data);

void show_customer_invoice_dialog(
    wxWindow *parent, FilamentInventory::Store &store,
    const FilamentInventory::CustomerOrder &order,
    const FilamentInventory::Customer &customer);

} // namespace Slic3r::GUI
