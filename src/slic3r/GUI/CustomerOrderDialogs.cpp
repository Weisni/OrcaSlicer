#include "CustomerOrderDialogs.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"

namespace Slic3r::GUI {

namespace {

using namespace FilamentInventory;

bool parse_number(wxString text, double &value)
{
    text.Trim(true).Trim(false);
    text.Replace(",", ".");
    return !text.empty() && text.ToDouble(&value) && std::isfinite(value);
}

bool currency_to_micros(const wxString &text, MoneyMicros &value)
{
    double amount = 0.0;
    if (!parse_number(text, amount) || amount < 0.0)
        return false;
    const long double micros = static_cast<long double>(amount) * 1'000'000.0L;
    if (micros > static_cast<long double>(std::numeric_limits<MoneyMicros>::max()))
        return false;
    value = static_cast<MoneyMicros>(std::llround(micros));
    return true;
}

wxTextCtrl *add_text_row(
    wxWindow *parent, wxFlexGridSizer *grid, const wxString &label,
    const wxString &value = {}, long style = 0)
{
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto *control = new wxTextCtrl(
        parent, wxID_ANY, value, wxDefaultPosition, wxSize(parent->FromDIP(320), -1), style);
    grid->Add(control, 1, wxEXPAND);
    return control;
}

class CustomerDialog final : public wxDialog
{
public:
    CustomerDialog(wxWindow *parent, const Customer *customer)
        : wxDialog(
              parent, wxID_ANY, customer ? _L("Edit customer") : _L("Add customer"),
              wxDefaultPosition, wxDefaultSize,
              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *details = new wxStaticBoxSizer(wxVERTICAL, this, _L("Customer details"));
        auto *grid = new wxFlexGridSizer(2, FromDIP(9), FromDIP(12));
        grid->AddGrowableCol(1, 1);
        m_name = add_text_row(this, grid, _L("Name"));
        m_contact = add_text_row(this, grid, _L("Contact person"));
        m_email = add_text_row(this, grid, _L("Email"));
        m_phone = add_text_row(this, grid, _L("Phone"));
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Notes")), 0, wxALIGN_TOP | wxTOP, FromDIP(4));
        m_notes = new wxTextCtrl(
            this, wxID_ANY, {}, wxDefaultPosition, wxSize(FromDIP(320), FromDIP(90)),
            wxTE_MULTILINE);
        grid->Add(m_notes, 1, wxEXPAND);
        details->Add(grid, 1, wxEXPAND | wxALL, FromDIP(12));
        root->Add(details, 1, wxEXPAND | wxALL, FromDIP(12));
        root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(12));

        if (customer != nullptr) {
            m_name->SetValue(from_u8(customer->name));
            m_contact->SetValue(from_u8(customer->contact_name));
            m_email->SetValue(from_u8(customer->email));
            m_phone->SetValue(from_u8(customer->phone));
            m_notes->SetValue(from_u8(customer->notes));
        }

        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(520), GetSize().y));
        CentreOnParent();
    }

    CustomerInput input() const
    {
        return {
            into_u8(m_name->GetValue()),
            into_u8(m_contact->GetValue()),
            into_u8(m_email->GetValue()),
            into_u8(m_phone->GetValue()),
            into_u8(m_notes->GetValue())
        };
    }

private:
    wxTextCtrl *m_name {nullptr};
    wxTextCtrl *m_contact {nullptr};
    wxTextCtrl *m_email {nullptr};
    wxTextCtrl *m_phone {nullptr};
    wxTextCtrl *m_notes {nullptr};
};

class CustomerOrderDialog final : public wxDialog
{
public:
    CustomerOrderDialog(
        wxWindow *parent, std::vector<Customer> customers,
        const CustomerOrder *order, const std::string &preferred_customer_id,
        std::string currency, const InventorySettings &settings)
        : wxDialog(
              parent, wxID_ANY,
              order ? _L("Edit customer order") : _L("Add customer order"),
              wxDefaultPosition, wxDefaultSize,
              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_customers(std::move(customers))
        , m_currency(std::move(currency))
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *details = new wxStaticBoxSizer(wxVERTICAL, this, _L("Order details"));
        auto *grid = new wxFlexGridSizer(2, FromDIP(9), FromDIP(12));
        grid->AddGrowableCol(1, 1);

        grid->Add(new wxStaticText(this, wxID_ANY, _L("Customer")), 0, wxALIGN_CENTER_VERTICAL);
        m_customer = new wxChoice(this, wxID_ANY);
        for (const Customer &item : m_customers)
            m_customer->Append(from_u8(item.name));
        grid->Add(m_customer, 1, wxEXPAND);
        m_number = add_text_row(this, grid, _L("Order number"));
        m_title = add_text_row(this, grid, _L("Title"));
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Notes")), 0, wxALIGN_TOP | wxTOP, FromDIP(4));
        m_notes = new wxTextCtrl(
            this, wxID_ANY, {}, wxDefaultPosition, wxSize(FromDIP(320), FromDIP(76)),
            wxTE_MULTILINE);
        grid->Add(m_notes, 1, wxEXPAND);
        details->Add(grid, 1, wxEXPAND | wxALL, FromDIP(12));
        root->Add(details, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto *billing = new wxStaticBoxSizer(wxVERTICAL, this, _L("Optional billing"));
        auto *billing_grid = new wxFlexGridSizer(2, FromDIP(9), FromDIP(12));
        billing_grid->AddGrowableCol(1, 1);
        m_quote = add_text_row(
            this, billing_grid,
            _L("Quoted price") + " (" + from_u8(m_currency) + ")");
        m_invoice = add_text_row(
            this, billing_grid,
            _L("Invoice amount") + " (" + from_u8(m_currency) + ")");
        m_design_hours = add_text_row(this, billing_grid, _L("Design time (hours)"), "0.00");
        m_design_rate = add_text_row(
            this, billing_grid,
            _L("Design hourly rate") + " (" + from_u8(m_currency) + "/h)",
            wxString::Format("%.2f", settings.design_per_hour_micros / 1'000'000.0));
        m_other_cost = add_text_row(
            this, billing_grid,
            _L("Other costs") + " (" + from_u8(m_currency) + ")", "0.00");
        m_discount = add_text_row(this, billing_grid, _L("Overall discount (%)"), "0.00");
        billing->Add(billing_grid, 1, wxEXPAND | wxALL, FromDIP(12));
        auto *included = new wxStaticBoxSizer(wxVERTICAL, this, _L("Invoice items"));
        auto *included_grid = new wxGridSizer(2, FromDIP(6), FromDIP(16));
        const auto add_item = [this, included_grid](const wxString &label, wxCheckBox *&box) {
            box = new wxCheckBox(this, wxID_ANY, label);
            box->SetValue(true);
            included_grid->Add(box);
        };
        add_item(_L("Material"), m_bill_material);
        add_item(_L("Electricity"), m_bill_electricity);
        add_item(_L("Machine wear"), m_bill_machine_wear);
        add_item(_L("Maintenance"), m_bill_maintenance);
        add_item(_L("Repair reserve"), m_bill_repair);
        add_item(_L("Design"), m_bill_design);
        add_item(_L("Other costs"), m_bill_other);
        included->Add(included_grid, 0, wxEXPAND | wxALL, FromDIP(10));
        root->Add(billing, 0, wxEXPAND | wxALL, FromDIP(12));
        root->Add(included, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(12));

        std::string customer_id = preferred_customer_id;
        if (order != nullptr) {
            customer_id = order->customer_id;
            m_number->SetValue(from_u8(order->order_number));
            m_title->SetValue(from_u8(order->title));
            m_notes->SetValue(from_u8(order->notes));
            if (order->quoted_price_micros)
                m_quote->SetValue(wxString::Format("%.2f", *order->quoted_price_micros / 1'000'000.0));
            if (order->invoice_amount_micros)
                m_invoice->SetValue(wxString::Format("%.2f", *order->invoice_amount_micros / 1'000'000.0));
            m_design_hours->SetValue(wxString::Format("%.2f", order->design_time_seconds / 3600.0));
            m_design_rate->SetValue(wxString::Format("%.2f", order->design_hourly_rate_micros / 1'000'000.0));
            m_other_cost->SetValue(wxString::Format("%.2f", order->other_cost_micros / 1'000'000.0));
            m_discount->SetValue(wxString::Format("%.2f", order->discount_basis_points / 100.0));
            m_bill_material->SetValue(order->bill_material);
            m_bill_electricity->SetValue(order->bill_electricity);
            m_bill_machine_wear->SetValue(order->bill_machine_wear);
            m_bill_maintenance->SetValue(order->bill_maintenance);
            m_bill_repair->SetValue(order->bill_repair_reserve);
            m_bill_design->SetValue(order->bill_design);
            m_bill_other->SetValue(order->bill_other);
        }
        for (std::size_t index = 0; index < m_customers.size(); ++index) {
            if (m_customers[index].id == customer_id) {
                m_customer->SetSelection(static_cast<int>(index));
                break;
            }
        }
        if (m_customer->GetSelection() == wxNOT_FOUND && !m_customers.empty())
            m_customer->SetSelection(0);

        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(540), GetSize().y));
        CentreOnParent();
    }

    bool input(CustomerOrderInput &result, wxString &error) const
    {
        const int customer = m_customer->GetSelection();
        if (customer == wxNOT_FOUND || static_cast<std::size_t>(customer) >= m_customers.size()) {
            error = _L("Please select a customer.");
            return false;
        }
        result.customer_id = m_customers[static_cast<std::size_t>(customer)].id;
        result.order_number = into_u8(m_number->GetValue());
        result.title = into_u8(m_title->GetValue());
        result.notes = into_u8(m_notes->GetValue());
        result.currency = m_currency;
        result.quoted_price_micros.reset();
        result.invoice_amount_micros.reset();

        double design_hours = 0.0;
        double discount_percent = 0.0;
        if (!parse_number(m_design_hours->GetValue(), design_hours) || design_hours < 0.0 ||
            design_hours > static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 3600.0) {
            error = _L("Design time must be a non-negative number of hours.");
            return false;
        }
        if (!currency_to_micros(m_design_rate->GetValue(), result.design_hourly_rate_micros) ||
            !currency_to_micros(m_other_cost->GetValue(), result.other_cost_micros)) {
            error = _L("Design rate and other costs must be non-negative amounts.");
            return false;
        }
        if (!parse_number(m_discount->GetValue(), discount_percent) ||
            discount_percent < 0.0 || discount_percent > 100.0) {
            error = _L("Discount must be between 0 and 100 percent.");
            return false;
        }
        result.design_time_seconds = static_cast<std::int64_t>(std::llround(design_hours * 3600.0));
        result.discount_basis_points = static_cast<std::int64_t>(std::llround(discount_percent * 100.0));
        result.bill_material = m_bill_material->GetValue();
        result.bill_electricity = m_bill_electricity->GetValue();
        result.bill_machine_wear = m_bill_machine_wear->GetValue();
        result.bill_maintenance = m_bill_maintenance->GetValue();
        result.bill_repair_reserve = m_bill_repair->GetValue();
        result.bill_design = m_bill_design->GetValue();
        result.bill_other = m_bill_other->GetValue();

        wxString quote = m_quote->GetValue();
        quote.Trim(true).Trim(false);
        if (!quote.empty()) {
            MoneyMicros value = 0;
            if (!currency_to_micros(quote, value)) {
                error = _L("Quoted price must be empty or a non-negative amount.");
                return false;
            }
            result.quoted_price_micros = value;
        }
        wxString invoice = m_invoice->GetValue();
        invoice.Trim(true).Trim(false);
        if (!invoice.empty()) {
            MoneyMicros value = 0;
            if (!currency_to_micros(invoice, value)) {
                error = _L("Invoice amount must be empty or a non-negative amount.");
                return false;
            }
            result.invoice_amount_micros = value;
        }
        return true;
    }

private:
    std::vector<Customer> m_customers;
    std::string           m_currency;
    wxChoice   *m_customer {nullptr};
    wxTextCtrl *m_number {nullptr};
    wxTextCtrl *m_title {nullptr};
    wxTextCtrl *m_notes {nullptr};
    wxTextCtrl *m_quote {nullptr};
    wxTextCtrl *m_invoice {nullptr};
    wxTextCtrl *m_design_hours {nullptr};
    wxTextCtrl *m_design_rate {nullptr};
    wxTextCtrl *m_other_cost {nullptr};
    wxTextCtrl *m_discount {nullptr};
    wxCheckBox *m_bill_material {nullptr};
    wxCheckBox *m_bill_electricity {nullptr};
    wxCheckBox *m_bill_machine_wear {nullptr};
    wxCheckBox *m_bill_maintenance {nullptr};
    wxCheckBox *m_bill_repair {nullptr};
    wxCheckBox *m_bill_design {nullptr};
    wxCheckBox *m_bill_other {nullptr};
};

class CostSettingsDialog final : public wxDialog
{
public:
    CostSettingsDialog(wxWindow *parent, const InventorySettings &settings)
        : wxDialog(
              parent, wxID_ANY, _L("Cost calculation settings"),
              wxDefaultPosition, wxDefaultSize,
              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(
                      this, wxID_ANY,
                      _L("Defaults are tailored for a household workshop in Viersen and a "
                         "Bambu P2S. They are planning assumptions and can be adjusted.\n"
                         "Changes apply to new print jobs. Use Recalculate costs to update "
                         "existing customer orders.")),
                  0, wxEXPAND | wxALL, FromDIP(12));
        auto *group = new wxStaticBoxSizer(wxVERTICAL, this, _L("Energy"));
        auto *grid = new wxFlexGridSizer(2, FromDIP(9), FromDIP(12));
        grid->AddGrowableCol(1, 1);
        m_electricity = add_text_row(
            this, grid, _L("Electricity price (EUR/kWh)"),
            wxString::Format("%.3f", settings.electricity_price_per_kwh_micros / 1'000'000.0));
        m_power = add_text_row(
            this, grid, _L("Average printer power (W)"),
            wxString::Format("%lld", static_cast<long long>(settings.default_machine_power_watts)));
        group->Add(grid, 1, wxEXPAND | wxALL, FromDIP(12));
        root->Add(group, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        auto *rates = new wxStaticBoxSizer(wxVERTICAL, this, _L("Hourly rates"));
        auto *rates_grid = new wxFlexGridSizer(2, FromDIP(9), FromDIP(12));
        rates_grid->AddGrowableCol(1, 1);
        m_wear = add_text_row(this, rates_grid, _L("Machine wear (EUR/h)"),
            wxString::Format("%.2f", settings.machine_wear_per_hour_micros / 1'000'000.0));
        m_maintenance = add_text_row(this, rates_grid, _L("Maintenance reserve (EUR/h)"),
            wxString::Format("%.2f", settings.maintenance_per_hour_micros / 1'000'000.0));
        m_repair = add_text_row(this, rates_grid, _L("Repair reserve (EUR/h)"),
            wxString::Format("%.2f", settings.repair_reserve_per_hour_micros / 1'000'000.0));
        m_design = add_text_row(this, rates_grid, _L("Design work (EUR/h)"),
            wxString::Format("%.2f", settings.design_per_hour_micros / 1'000'000.0));
        rates->Add(rates_grid, 1, wxEXPAND | wxALL, FromDIP(12));
        root->Add(rates, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(12));
        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(520), GetSize().y));
        CentreOnParent();
    }

    bool settings(InventorySettings &result, wxString &error) const
    {
        MoneyMicros electricity = 0;
        if (!currency_to_micros(m_electricity->GetValue(), electricity)) {
            error = _L("Electricity price must be a non-negative amount.");
            return false;
        }
        double watts = 0.0;
        if (!parse_number(m_power->GetValue(), watts) || watts <= 0.0 ||
            watts > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            error = _L("Average printer power must be a positive number.");
            return false;
        }
        result.currency = "EUR";
        result.electricity_price_per_kwh_micros = electricity;
        result.default_machine_power_watts = static_cast<std::int64_t>(std::llround(watts));
        if (!currency_to_micros(m_wear->GetValue(), result.machine_wear_per_hour_micros) ||
            !currency_to_micros(m_maintenance->GetValue(), result.maintenance_per_hour_micros) ||
            !currency_to_micros(m_repair->GetValue(), result.repair_reserve_per_hour_micros) ||
            !currency_to_micros(m_design->GetValue(), result.design_per_hour_micros)) {
            error = _L("Hourly rates must be non-negative amounts.");
            return false;
        }
        return true;
    }

private:
    wxTextCtrl *m_electricity {nullptr};
    wxTextCtrl *m_power {nullptr};
    wxTextCtrl *m_wear {nullptr};
    wxTextCtrl *m_maintenance {nullptr};
    wxTextCtrl *m_repair {nullptr};
    wxTextCtrl *m_design {nullptr};
};

} // namespace

std::optional<Customer> edit_customer_interactively(
    wxWindow *parent, Store &store, const Customer *customer)
{
    CustomerDialog dialog(parent, customer);
    while (dialog.ShowModal() == wxID_OK) {
        try {
            const CustomerInput input = dialog.input();
            return customer == nullptr ?
                       store.create_customer(input) :
                       store.update_customer(customer->id, input);
        } catch (const std::exception &error) {
            wxMessageBox(from_u8(error.what()), _L("Customer"), wxOK | wxICON_WARNING, parent);
        }
    }
    return std::nullopt;
}

std::optional<CustomerOrder> edit_customer_order_interactively(
    wxWindow *parent, Store &store, const CustomerOrder *order,
    const std::string &preferred_customer_id,
    std::optional<std::string> new_order_currency_override)
{
    std::vector<Customer> customers = store.list_customers();
    if (order != nullptr) {
        const Customer current_customer = store.get_customer(order->customer_id);
        const auto current = std::find_if(
            customers.begin(), customers.end(),
            [&current_customer](const Customer &customer) {
                return customer.id == current_customer.id;
            });
        if (current == customers.end())
            customers.emplace_back(current_customer);
    }
    if (customers.empty()) {
        wxMessageBox(
            _L("Create a customer before adding an order."),
            _L("Customer orders"), wxOK | wxICON_INFORMATION, parent);
        return std::nullopt;
    }

    const std::string currency =
        order != nullptr ?
            order->currency :
            (new_order_currency_override ?
                 *new_order_currency_override :
                 store.get_settings().currency);
    CustomerOrderDialog dialog(
        parent, customers, order, preferred_customer_id, currency,
        store.get_settings());
    while (dialog.ShowModal() == wxID_OK) {
        CustomerOrderInput input;
        wxString error;
        if (!dialog.input(input, error)) {
            wxMessageBox(error, _L("Customer orders"), wxOK | wxICON_WARNING, parent);
            continue;
        }
        try {
            return order == nullptr ?
                       store.create_customer_order(input) :
                       store.update_customer_order(order->id, input);
        } catch (const std::exception &exception) {
            wxMessageBox(
                from_u8(exception.what()), _L("Customer orders"),
                wxOK | wxICON_WARNING, parent);
        }
    }
    return std::nullopt;
}

bool edit_inventory_cost_settings_interactively(wxWindow *parent, Store &store)
{
    CostSettingsDialog dialog(parent, store.get_settings());
    while (dialog.ShowModal() == wxID_OK) {
        InventorySettings settings;
        wxString error;
        if (!dialog.settings(settings, error)) {
            wxMessageBox(error, _L("Cost settings"), wxOK | wxICON_WARNING, parent);
            continue;
        }
        try {
            store.update_settings(settings);
            return true;
        } catch (const std::exception &exception) {
            wxMessageBox(
                from_u8(exception.what()), _L("Cost settings"),
                wxOK | wxICON_WARNING, parent);
        }
    }
    return false;
}

} // namespace Slic3r::GUI
