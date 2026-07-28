#include "CustomerOrderDialogs.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <wx/choice.h>
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
        std::string currency)
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
        billing->Add(billing_grid, 1, wxEXPAND | wxALL, FromDIP(12));
        root->Add(billing, 0, wxEXPAND | wxALL, FromDIP(12));
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
                      _L("Electricity cost is calculated from the sliced machine runtime "
                         "and the average printer power below.")),
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
        return true;
    }

private:
    wxTextCtrl *m_electricity {nullptr};
    wxTextCtrl *m_power {nullptr};
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
        parent, customers, order, preferred_customer_id, currency);
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
