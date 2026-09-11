#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

#include "libslic3r/FilamentInventory.hpp"

class wxButton;
class wxDataViewListCtrl;
class wxSimplebook;
class wxStaticText;
class TabCtrl;

namespace Slic3r::GUI {

class FilamentManagerPanel : public wxPanel
{
public:
    FilamentManagerPanel(wxWindow *parent, wxWindowID id = wxID_ANY,
                         const wxPoint &position = wxDefaultPosition,
                         const wxSize &size = wxDefaultSize,
                         long style = wxTAB_TRAVERSAL);

    bool Show(bool show = true) override;
    void refresh();

private:
    using InventoryStore = FilamentInventory::Store;
    using Spool          = FilamentInventory::Spool;
    using PrintJob       = FilamentInventory::PrintJob;
    using Customer       = FilamentInventory::Customer;
    using CustomerOrder  = FilamentInventory::CustomerOrder;

    bool initialize_store();
    void refresh_spools();
    void refresh_jobs();
    void refresh_history();
    void refresh_job_history();
    void refresh_customers_and_orders();
    void update_button_state();

    int selected_spool_row() const;
    int selected_job_row() const;
    int selected_job_history_row() const;
    int selected_customer_row() const;
    int selected_order_row() const;

    void add_spool();
    void edit_spool();
    void set_remaining();
    void archive_spool();
    void manage_identifiers();
    void copy_nfc_link();
    void review_job();
    void confirm_job(bool correct_amounts);
    void discard_job();
    void edit_selected_job(bool history);
    void add_customer();
    void edit_customer();
    void archive_customer();
    void add_customer_order();
    void edit_customer_order();
    void show_material_breakdown();
    void show_invoice();
    void show_selected_job_materials(bool history);
    void set_customer_order_status(FilamentInventory::CustomerOrderStatus status);
    void delete_customer_order();
    void edit_cost_settings();
    void recalculate_costs();
    void show_error(const std::exception &error);

    InventoryStore                  *m_store {nullptr};
    bool                            m_store_initialization_attempted {false};
    bool                            m_store_error_reported {false};
    std::string                     m_store_error;
    std::vector<Spool>              m_spools;
    std::vector<PrintJob>           m_jobs;
    std::vector<PrintJob>           m_job_history;
    std::vector<Customer>           m_customers;
    std::vector<CustomerOrder>      m_customer_orders;
    std::map<std::string, std::size_t> m_order_job_counts;
    std::set<std::string>           m_orders_with_open_jobs;

    TabCtrl            *m_tabs {nullptr};
    wxSimplebook       *m_pages {nullptr};
    wxDataViewListCtrl *m_spool_list {nullptr};
    wxDataViewListCtrl *m_job_list {nullptr};
    wxDataViewListCtrl *m_history_list {nullptr};
    wxDataViewListCtrl *m_job_history_list {nullptr};
    wxDataViewListCtrl *m_customer_list {nullptr};
    wxDataViewListCtrl *m_order_list {nullptr};
    wxButton           *m_add_button {nullptr};
    wxButton           *m_edit_button {nullptr};
    wxButton           *m_remaining_button {nullptr};
    wxButton           *m_archive_button {nullptr};
    wxButton           *m_identifiers_button {nullptr};
    wxButton           *m_copy_nfc_button {nullptr};
    wxButton           *m_confirm_button {nullptr};
    wxButton           *m_correct_button {nullptr};
    wxButton           *m_review_button {nullptr};
    wxButton           *m_discard_button {nullptr};
    wxButton           *m_edit_job_button {nullptr};
    wxButton           *m_job_materials_button {nullptr};
    wxButton           *m_edit_job_history_button {nullptr};
    wxButton           *m_job_history_materials_button {nullptr};
    wxButton           *m_edit_customer_button {nullptr};
    wxButton           *m_archive_customer_button {nullptr};
    wxButton           *m_add_order_button {nullptr};
    wxButton           *m_edit_order_button {nullptr};
    wxButton           *m_material_breakdown_button {nullptr};
    wxButton           *m_invoice_button {nullptr};
    wxButton           *m_recalculate_costs_button {nullptr};
    wxButton           *m_activate_order_button {nullptr};
    wxButton           *m_complete_order_button {nullptr};
    wxButton           *m_cancel_order_button {nullptr};
    wxButton           *m_delete_order_button {nullptr};
    wxStaticText       *m_active_spools_value {nullptr};
    wxStaticText       *m_available_value {nullptr};
    wxStaticText       *m_reserved_value {nullptr};
    wxStaticText       *m_low_stock_value {nullptr};
    std::vector<wxButton *> m_refresh_buttons;
    wxTimer             m_refresh_timer;
    std::uint64_t       m_seen_service_revision {0};
};

} // namespace Slic3r::GUI
