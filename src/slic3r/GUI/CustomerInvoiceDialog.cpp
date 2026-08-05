#include "CustomerInvoiceDialog.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <wx/bitmap.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/choice.h>
#include <wx/dataview.h>
#include <wx/datetime.h>
#include <wx/dcmemory.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r::GUI {

namespace {

using namespace FilamentInventory;

MoneyMicros checked_invoice_add(MoneyMicros lhs, MoneyMicros rhs)
{
    if ((rhs > 0 && lhs > std::numeric_limits<MoneyMicros>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<MoneyMicros>::min() - rhs))
        throw std::runtime_error("Invoice total exceeds the supported money range");
    return lhs + rhs;
}

MoneyMicros invoice_net(const InvoiceExportData &data)
{
    MoneyMicros result = 0;
    for (const InvoiceLine &line : data.lines)
        result = checked_invoice_add(result, line.invoice_amount_micros);
    return result;
}

MoneyMicros invoice_internal_total(const InvoiceExportData &data)
{
    MoneyMicros result = 0;
    for (const InvoiceLine &line : data.lines)
        result = checked_invoice_add(result, line.internal_amount_micros);
    return result;
}

MoneyMicros invoice_tax(const InvoiceExportData &data)
{
    if (data.small_business)
        return 0;
    const long double value =
        static_cast<long double>(invoice_net(data)) * data.vat_basis_points / 10'000.0L;
    if (value > static_cast<long double>(std::numeric_limits<MoneyMicros>::max()))
        throw std::runtime_error("Invoice tax exceeds the supported money range");
    return static_cast<MoneyMicros>(std::llround(value));
}

std::string money(MoneyMicros value, const std::string &currency)
{
    const bool negative = value < 0;
    const std::uint64_t magnitude = negative ?
        static_cast<std::uint64_t>(-(value + 1)) + 1 :
        static_cast<std::uint64_t>(value);
    const std::uint64_t cents = (magnitude + 5'000) / 10'000;
    std::ostringstream out;
    if (negative) out << '-';
    out << cents / 100 << ',' << std::setw(2) << std::setfill('0')
        << cents % 100 << ' ' << currency;
    return out.str();
}

std::string line_label(InvoiceCostCategory category)
{
    switch (category) {
    case InvoiceCostCategory::material:       return "Filament";
    case InvoiceCostCategory::electricity:    return "Strom";
    case InvoiceCostCategory::machine_wear:   return "Maschinenverschleiß";
    case InvoiceCostCategory::maintenance:    return "Wartung";
    case InvoiceCostCategory::repair_reserve: return "Instandhaltungsrücklage";
    case InvoiceCostCategory::design:         return "Designleistung";
    case InvoiceCostCategory::other:          return "Sonstige Kosten";
    case InvoiceCostCategory::discount:       return "Rabatt";
    }
    return "Kostenposition";
}

std::string replace_line_breaks(std::string value, const std::string &replacement)
{
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\r')
            continue;
        if (value[index] == '\n')
            result += replacement;
        else
            result.push_back(value[index]);
    }
    return result;
}

std::vector<std::string> split_lines(const std::string &value)
{
    std::vector<std::string> lines;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    if (lines.empty())
        lines.emplace_back();
    return lines;
}

std::string truncate_text(const std::string &value, std::size_t maximum)
{
    if (value.size() <= maximum)
        return value;
    return value.substr(0, maximum > 3 ? maximum - 3 : 0) + "...";
}

std::string pdf_escape(const std::string &utf8)
{
    const wxString text = wxString::FromUTF8(utf8);
    const wxCharBuffer encoded = text.mb_str(wxCSConv("windows-1252"));
    const std::string bytes(encoded.data(), encoded.length());
    std::string result;
    result.reserve(bytes.size());
    for (const unsigned char ch : bytes) {
        if (ch == '(' || ch == ')' || ch == '\\')
            result.push_back('\\');
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

void pdf_text(std::ostringstream &page, double x, double y,
              const std::string &text, double size = 9.0, bool bold = false,
              bool white = false)
{
    page << (white ? "1 1 1 rg " : "0.04 0.23 0.34 rg ")
         << "BT /" << (bold ? "F2" : "F1") << ' ' << size
         << " Tf " << x << ' ' << y << " Td (" << pdf_escape(text)
         << ") Tj ET\n";
}

void pdf_line(std::ostringstream &page, double x1, double y1,
              double x2, double y2, double gray = 0.75)
{
    page << gray << " G 0.6 w " << x1 << ' ' << y1 << " m "
         << x2 << ' ' << y2 << " l S\n";
}

void pdf_rect(std::ostringstream &page, double x, double y, double width,
              double height, double red, double green, double blue)
{
    page << red << ' ' << green << ' ' << blue << " rg "
         << x << ' ' << y << ' ' << width << ' ' << height << " re f\n";
}

void pdf_spool(std::ostringstream &page, double x, double y,
               const std::string &color_hex)
{
    if (color_hex.size() != 7 || color_hex.front() != '#')
        return;
    const auto channel = [&color_hex](std::size_t offset) {
        return std::stoi(color_hex.substr(offset, 2), nullptr, 16) / 255.0;
    };
    double red = 0.5, green = 0.5, blue = 0.5;
    try {
        red = channel(1); green = channel(3); blue = channel(5);
    } catch (...) {
        return;
    }
    constexpr double k = 0.5522847498;
    const auto circle = [&page, k](double cx, double cy, double radius) {
        page << cx + radius << ' ' << cy << " m "
             << cx + radius << ' ' << cy + k * radius << ' '
             << cx + k * radius << ' ' << cy + radius << ' '
             << cx << ' ' << cy + radius << " c "
             << cx - k * radius << ' ' << cy + radius << ' '
             << cx - radius << ' ' << cy + k * radius << ' '
             << cx - radius << ' ' << cy << " c "
             << cx - radius << ' ' << cy - k * radius << ' '
             << cx - k * radius << ' ' << cy - radius << ' '
             << cx << ' ' << cy - radius << " c "
             << cx + k * radius << ' ' << cy - radius << ' '
             << cx + radius << ' ' << cy - k * radius << ' '
             << cx + radius << ' ' << cy << " c ";
    };
    page << red << ' ' << green << ' ' << blue << " rg 0.25 G 0.5 w ";
    circle(x, y, 8.0);
    page << "B\n1 1 1 rg ";
    circle(x, y, 3.0);
    page << "f\n";
}

void invoice_table_header(std::ostringstream &page, double y)
{
    pdf_rect(page, 46, y - 5, 503, 21, 0.91, 0.94, 0.97);
    pdf_text(page, 55, y + 2, "Pos.", 8, true);
    pdf_text(page, 82, y + 2, "Beschreibung", 8, true);
    pdf_text(page, 388, y + 2, "Interne Kosten", 8, true);
    pdf_text(page, 488, y + 2, "Rechnung", 8, true);
}

std::vector<std::string> build_pdf_pages(const InvoiceExportData &data)
{
    std::vector<std::ostringstream> pages;
    pages.emplace_back();
    std::ostringstream *page = &pages.back();
    pdf_rect(*page, 0, 812, 595, 30, 0.06, 0.29, 0.43);
    pdf_text(*page, 48, 785, "RECHNUNG", 24, true);
    pdf_text(*page, 48, 762, data.seller_name.empty() ? "Absender" : data.seller_name, 11, true);
    double seller_y = 747;
    for (const std::string &line : split_lines(data.seller_address)) {
        pdf_text(*page, 48, seller_y, line, 8.5);
        seller_y -= 12;
    }
    if (!data.seller_contact.empty())
        pdf_text(*page, 48, seller_y, data.seller_contact, 8.5);
    if (!data.tax_identifier.empty())
        pdf_text(*page, 48, seller_y - 12, data.tax_identifier, 8.5);

    pdf_text(*page, 330, 762, "Rechnungsnummer", 8, true);
    pdf_text(*page, 455, 762, data.invoice_number, 8.5);
    pdf_text(*page, 330, 747, "Rechnungsdatum", 8, true);
    pdf_text(*page, 455, 747, data.invoice_date, 8.5);
    pdf_text(*page, 330, 732, "Leistungsdatum", 8, true);
    pdf_text(*page, 455, 732, data.service_date, 8.5);
    pdf_text(*page, 330, 717, "Fällig am", 8, true);
    pdf_text(*page, 455, 717, data.due_date, 8.5);

    pdf_text(*page, 48, 675, "Rechnung an", 8, true);
    pdf_text(*page, 48, 658, data.customer_name, 10, true);
    double customer_y = 643;
    for (const std::string &line : split_lines(data.customer_address)) {
        pdf_text(*page, 48, customer_y, line, 8.5);
        customer_y -= 12;
    }
    pdf_text(*page, 330, 675, "Auftrag", 8, true);
    pdf_text(*page, 330, 658, truncate_text(data.order_title, 42), 10, true);
    pdf_line(*page, 46, 610, 549, 610, 0.55);
    invoice_table_header(*page, 582);
    double y = 552;
    std::size_t position = 1;
    for (const InvoiceLine &line : data.lines) {
        if (y < 175) {
            pages.emplace_back();
            page = &pages.back();
            pdf_text(*page, 48, 800, "RECHNUNG " + data.invoice_number, 13, true);
            invoice_table_header(*page, 770);
            y = 740;
        }
        if (position % 2 == 0)
            pdf_rect(*page, 46, y - 10, 503, 29, 0.975, 0.98, 0.985);
        pdf_text(*page, 55, y + 3, std::to_string(position), 8.5);
        if (!line.color_hex.empty())
            pdf_spool(*page, 72, y + 4, line.color_hex);
        const std::string description =
            line.category == InvoiceCostCategory::material ?
                line.description : line_label(line.category);
        pdf_text(*page, 84, y + 6, truncate_text(description, 48), 8.5, true);
        std::string detail = line.detail;
        if (!line.included)
            detail += detail.empty() ? "nicht berechnet" : " - nicht berechnet";
        pdf_text(*page, 84, y - 5, truncate_text(detail, 58), 7.3);
        pdf_text(*page, 388, y + 2,
                 money(line.internal_amount_micros, data.currency), 8.2);
        pdf_text(*page, 488, y + 2,
                 money(line.invoice_amount_micros, data.currency), 8.2,
                 line.category == InvoiceCostCategory::discount);
        pdf_line(*page, 46, y - 12, 549, y - 12, 0.9);
        y -= 32;
        ++position;
    }
    if (y < 225) {
        pages.emplace_back();
        page = &pages.back();
        pdf_text(*page, 48, 800, "RECHNUNG " + data.invoice_number, 13, true);
        y = 735;
    }
    const MoneyMicros internal = invoice_internal_total(data);
    const MoneyMicros net = invoice_net(data);
    const MoneyMicros tax = invoice_tax(data);
    const MoneyMicros gross = checked_invoice_add(net, tax);
    pdf_line(*page, 320, y + 8, 549, y + 8, 0.45);
    pdf_text(*page, 330, y - 8, "Interne Gesamtkosten", 8.5);
    pdf_text(*page, 470, y - 8, money(internal, data.currency), 8.5);
    pdf_text(*page, 330, y - 27, "Nettobetrag", 9, true);
    pdf_text(*page, 470, y - 27, money(net, data.currency), 9, true);
    if (data.small_business) {
        pdf_text(*page, 330, y - 46, "Umsatzsteuer", 8.5);
        pdf_text(*page, 470, y - 46, money(0, data.currency), 8.5);
        pdf_text(*page, 48, y - 82,
                 "Gemäß § 19 UStG wird keine Umsatzsteuer berechnet.", 8.2);
    } else {
        std::ostringstream vat;
        vat << "Umsatzsteuer " << std::fixed << std::setprecision(2)
            << data.vat_basis_points / 100.0 << " %";
        pdf_text(*page, 330, y - 46, vat.str(), 8.5);
        pdf_text(*page, 470, y - 46, money(tax, data.currency), 8.5);
    }
    pdf_rect(*page, 320, y - 82, 229, 25, 0.06, 0.29, 0.43);
    pdf_text(*page, 330, y - 74, "Gesamtbetrag", 10, true, true);
    pdf_text(*page, 470, y - 74, money(gross, data.currency), 10, true, true);

    std::vector<std::string> result;
    result.reserve(pages.size());
    for (std::size_t index = 0; index < pages.size(); ++index) {
        pdf_line(pages[index], 46, 45, 549, 45, 0.82);
        pdf_text(pages[index], 48, 30, "Erstellt mit QuackSlicer", 7.5);
        pdf_text(pages[index], 505, 30,
                 "Seite " + std::to_string(index + 1) + " / " +
                     std::to_string(pages.size()), 7.5);
        result.emplace_back(pages[index].str());
    }
    return result;
}

wxBitmap spool_bitmap(const std::string &color_hex)
{
    wxBitmap bitmap(22, 22, 32);
    wxMemoryDC dc(bitmap);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    wxColour color(from_u8(color_hex));
    if (!color.IsOk()) color = wxColour(128, 128, 128);
    dc.SetPen(wxPen(wxColour(70, 70, 70), 1));
    dc.SetBrush(wxBrush(color));
    dc.DrawCircle(wxPoint(11, 11), 9);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(*wxWHITE_BRUSH);
    dc.DrawCircle(wxPoint(11, 11), 3);
    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

wxTextCtrl *add_invoice_text(wxWindow *parent, wxFlexGridSizer *grid,
                             const wxString &label, const wxString &value = {},
                             long style = 0)
{
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0,
              style & wxTE_MULTILINE ? wxALIGN_TOP : wxALIGN_CENTER_VERTICAL);
    auto *control = new wxTextCtrl(
        parent, wxID_ANY, value, wxDefaultPosition,
        wxSize(parent->FromDIP(310), style & wxTE_MULTILINE ? parent->FromDIP(64) : -1),
        style);
    grid->Add(control, 1, wxEXPAND);
    return control;
}

std::string config_value(const char *key)
{
    return wxGetApp().app_config->get("invoice_export", key);
}

class InvoiceDialog final : public wxDialog
{
public:
    InvoiceDialog(wxWindow *parent, const CustomerOrder &order,
                  const Customer &customer, std::vector<InvoiceLine> lines)
        : wxDialog(parent, wxID_ANY, _L("Create invoice"), wxDefaultPosition,
                   wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_order(order), m_customer(customer), m_lines(std::move(lines))
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *columns = new wxBoxSizer(wxHORIZONTAL);
        auto *sender = new wxStaticBoxSizer(wxVERTICAL, this, _L("Invoice issuer"));
        auto *sender_grid = new wxFlexGridSizer(2, FromDIP(7), FromDIP(10));
        sender_grid->AddGrowableCol(1, 1);
        m_seller_name = add_invoice_text(this, sender_grid, _L("Company/name"),
            from_u8(config_value("seller_name")));
        m_seller_address = add_invoice_text(this, sender_grid, _L("Address"),
            from_u8(config_value("seller_address")), wxTE_MULTILINE);
        m_seller_contact = add_invoice_text(this, sender_grid, _L("Email / phone"),
            from_u8(config_value("seller_contact")));
        m_tax_identifier = add_invoice_text(this, sender_grid, _L("Tax number / VAT ID"),
            from_u8(config_value("tax_identifier")));
        sender->Add(sender_grid, 1, wxEXPAND | wxALL, FromDIP(10));

        auto *recipient = new wxStaticBoxSizer(wxVERTICAL, this, _L("Invoice details"));
        auto *recipient_grid = new wxFlexGridSizer(2, FromDIP(7), FromDIP(10));
        recipient_grid->AddGrowableCol(1, 1);
        m_customer_name = add_invoice_text(this, recipient_grid, _L("Customer"),
            from_u8(customer.name));
        m_customer_address = add_invoice_text(this, recipient_grid, _L("Billing address"), {},
            wxTE_MULTILINE);
        const wxString today = wxDateTime::Today().FormatISODate();
        const wxString number = !order.order_number.empty() ?
            from_u8(order.order_number) : "RE-" + wxDateTime::Now().Format("%Y%m%d-%H%M");
        m_invoice_number = add_invoice_text(this, recipient_grid, _L("Invoice number"), number);
        m_invoice_date = add_invoice_text(this, recipient_grid, _L("Invoice date"), today);
        m_service_date = add_invoice_text(this, recipient_grid, _L("Service date"), today);
        m_due_date = add_invoice_text(this, recipient_grid, _L("Due date"),
            (wxDateTime::Today() + wxDateSpan::Days(14)).FormatISODate());
        recipient->Add(recipient_grid, 1, wxEXPAND | wxALL, FromDIP(10));
        columns->Add(sender, 1, wxEXPAND | wxRIGHT, FromDIP(8));
        columns->Add(recipient, 1, wxEXPAND);
        root->Add(columns, 0, wxEXPAND | wxALL, FromDIP(12));

        auto *tax_row = new wxBoxSizer(wxHORIZONTAL);
        m_small_business = new wxCheckBox(this, wxID_ANY,
            _L("Small-business regulation (Section 19 UStG, no VAT)"));
        m_small_business->SetValue(config_value("small_business") == "true");
        tax_row->Add(m_small_business, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
        tax_row->Add(new wxStaticText(this, wxID_ANY, _L("VAT rate")), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_vat = new wxChoice(this, wxID_ANY);
        m_vat->Append("19 %");
        m_vat->Append("7 %");
        m_vat->Append("0 %");
        const std::string configured_vat = config_value("vat_rate");
        m_vat->SetSelection(configured_vat == "7" ? 1 : configured_vat == "0" ? 2 : 0);
        tax_row->Add(m_vat);
        root->Add(tax_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        m_list = new wxDataViewListCtrl(
            this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(280)),
            wxDV_ROW_LINES | wxBORDER_SIMPLE);
        m_list->AppendIconTextColumn(_L("Position"), wxDATAVIEW_CELL_INERT, FromDIP(300));
        m_list->AppendTextColumn(_L("Details"), wxDATAVIEW_CELL_INERT, FromDIP(190));
        m_list->AppendTextColumn(_L("Internal cost"), wxDATAVIEW_CELL_INERT, FromDIP(130), wxALIGN_RIGHT);
        m_list->AppendTextColumn(_L("Invoice"), wxDATAVIEW_CELL_INERT, FromDIP(130), wxALIGN_RIGHT);
        for (const InvoiceLine &line : m_lines) {
            wxIcon icon;
            if (!line.color_hex.empty())
                icon.CopyFromBitmap(spool_bitmap(line.color_hex));
            wxVariant first;
            first << wxDataViewIconText(
                from_u8(line.category == InvoiceCostCategory::material ?
                    line.description : line_label(line.category)), icon);
            wxVector<wxVariant> row;
            row.emplace_back(first);
            row.emplace_back(from_u8(
                line.included ? line.detail : line.detail + " - not charged"));
            row.emplace_back(from_u8(money(line.internal_amount_micros, order.currency)));
            row.emplace_back(from_u8(money(line.invoice_amount_micros, order.currency)));
            m_list->AppendItem(row);
        }
        root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        m_totals = new wxStaticText(this, wxID_ANY, {});
        wxFont totals_font = m_totals->GetFont();
        totals_font.SetWeight(wxFONTWEIGHT_BOLD);
        totals_font.SetPointSize(totals_font.GetPointSize() + 1);
        m_totals->SetFont(totals_font);
        root->Add(m_totals, 0, wxALIGN_RIGHT | wxALL, FromDIP(12));

        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        auto *copy = new wxButton(this, wxID_ANY, _L("Copy invoice"));
        auto *pdf = new wxButton(this, wxID_ANY, _L("Export PDF..."));
        auto *close = new wxButton(this, wxID_CANCEL, _L("Close"));
        buttons->Add(copy, 0, wxRIGHT, FromDIP(8));
        buttons->Add(pdf, 0, wxRIGHT, FromDIP(8));
        buttons->AddStretchSpacer();
        buttons->Add(close);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(980), FromDIP(700)));
        SetSize(GetMinSize());
        CentreOnParent();
        update_totals();
        m_small_business->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { update_totals(); });
        m_vat->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { update_totals(); });
        copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { copy_invoice(); });
        pdf->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { export_pdf(); });
    }

private:
    InvoiceExportData data() const
    {
        InvoiceExportData result;
        result.seller_name = into_u8(m_seller_name->GetValue());
        result.seller_address = into_u8(m_seller_address->GetValue());
        result.seller_contact = into_u8(m_seller_contact->GetValue());
        result.tax_identifier = into_u8(m_tax_identifier->GetValue());
        result.customer_name = into_u8(m_customer_name->GetValue());
        result.customer_address = into_u8(m_customer_address->GetValue());
        result.invoice_number = into_u8(m_invoice_number->GetValue());
        result.invoice_date = into_u8(m_invoice_date->GetValue());
        result.service_date = into_u8(m_service_date->GetValue());
        result.due_date = into_u8(m_due_date->GetValue());
        result.order_title = m_order.order_number.empty() ? m_order.title :
            m_order.order_number + " - " + m_order.title;
        result.currency = m_order.currency;
        result.small_business = m_small_business->GetValue();
        result.vat_basis_points = m_vat->GetSelection() == 1 ? 700 :
                                  m_vat->GetSelection() == 2 ? 0 : 1'900;
        result.lines = m_lines;
        return result;
    }

    void save_defaults() const
    {
        AppConfig *config = wxGetApp().app_config;
        config->set("invoice_export", "seller_name", into_u8(m_seller_name->GetValue()));
        config->set("invoice_export", "seller_address", into_u8(m_seller_address->GetValue()));
        config->set("invoice_export", "seller_contact", into_u8(m_seller_contact->GetValue()));
        config->set("invoice_export", "tax_identifier", into_u8(m_tax_identifier->GetValue()));
        config->set("invoice_export", "small_business", m_small_business->GetValue() ? "true" : "false");
        config->set("invoice_export", "vat_rate",
            m_vat->GetSelection() == 1 ? "7" : m_vat->GetSelection() == 2 ? "0" : "19");
    }

    bool validate_metadata() const
    {
        if (m_seller_name->GetValue().Strip().empty() ||
            m_seller_address->GetValue().Strip().empty() ||
            m_customer_name->GetValue().Strip().empty() ||
            m_customer_address->GetValue().Strip().empty() ||
            m_invoice_number->GetValue().Strip().empty()) {
            wxMessageBox(
                _L("Issuer, issuer address, customer, billing address, and invoice number are required."),
                _L("Invoice"), wxOK | wxICON_WARNING, const_cast<InvoiceDialog *>(this));
            return false;
        }
        return true;
    }

    void update_totals()
    {
        const InvoiceExportData current = data();
        const MoneyMicros net = invoice_net(current);
        const MoneyMicros tax = invoice_tax(current);
        m_totals->SetLabel(
            _L("Internal costs") + ": " + from_u8(money(invoice_internal_total(current), current.currency)) +
            "    " + _L("Net") + ": " + from_u8(money(net, current.currency)) +
            "    " + _L("VAT") + ": " + from_u8(money(tax, current.currency)) +
            "    " + _L("Total") + ": " + from_u8(money(checked_invoice_add(net, tax), current.currency)));
    }

    void copy_invoice()
    {
        if (!validate_metadata()) return;
        save_defaults();
        if (!wxTheClipboard->Open()) {
            wxMessageBox(_L("Could not open the clipboard."), _L("Invoice"),
                         wxOK | wxICON_ERROR, this);
            return;
        }
        wxTheClipboard->SetData(new wxTextDataObject(from_u8(invoice_plain_text(data()))));
        wxTheClipboard->Close();
        wxMessageBox(_L("The invoice was copied to the clipboard."), _L("Invoice"),
                     wxOK | wxICON_INFORMATION, this);
    }

    void export_pdf()
    {
        if (!validate_metadata()) return;
        wxString filename = from_u8(data().invoice_number);
        for (const wxUniChar character : wxFileName::GetForbiddenChars())
            filename.Replace(wxString(character), "_");
        wxFileDialog dialog(
            this, _L("Export invoice as PDF"), {}, filename + ".pdf",
            _L("PDF files (*.pdf)|*.pdf"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK)
            return;
        try {
            save_defaults();
            write_invoice_pdf(into_u8(dialog.GetPath()), data());
            wxMessageBox(_L("The invoice PDF was created successfully."),
                         _L("Invoice"), wxOK | wxICON_INFORMATION, this);
        } catch (const std::exception &error) {
            wxMessageBox(from_u8(error.what()), _L("Invoice"),
                         wxOK | wxICON_ERROR, this);
        }
    }

    CustomerOrder m_order;
    Customer m_customer;
    std::vector<InvoiceLine> m_lines;
    wxTextCtrl *m_seller_name {nullptr};
    wxTextCtrl *m_seller_address {nullptr};
    wxTextCtrl *m_seller_contact {nullptr};
    wxTextCtrl *m_tax_identifier {nullptr};
    wxTextCtrl *m_customer_name {nullptr};
    wxTextCtrl *m_customer_address {nullptr};
    wxTextCtrl *m_invoice_number {nullptr};
    wxTextCtrl *m_invoice_date {nullptr};
    wxTextCtrl *m_service_date {nullptr};
    wxTextCtrl *m_due_date {nullptr};
    wxCheckBox *m_small_business {nullptr};
    wxChoice *m_vat {nullptr};
    wxDataViewListCtrl *m_list {nullptr};
    wxStaticText *m_totals {nullptr};
};

} // namespace

std::string invoice_plain_text(const InvoiceExportData &data)
{
    std::ostringstream out;
    out << "RECHNUNG\n\n"
        << data.seller_name << '\n' << data.seller_address << '\n';
    if (!data.seller_contact.empty()) out << data.seller_contact << '\n';
    if (!data.tax_identifier.empty()) out << data.tax_identifier << '\n';
    out << "\nRechnung an:\n" << data.customer_name << '\n'
        << data.customer_address << "\n\n"
        << "Rechnungsnummer: " << data.invoice_number << '\n'
        << "Rechnungsdatum: " << data.invoice_date << '\n'
        << "Leistungsdatum: " << data.service_date << '\n'
        << "Fällig am: " << data.due_date << '\n'
        << "Auftrag: " << data.order_title << "\n\n"
        << "Pos.\tBeschreibung\tDetails\tInterne Kosten\tRechnung\n";
    std::size_t position = 1;
    for (const InvoiceLine &line : data.lines) {
        out << position++ << '\t'
            << (line.category == InvoiceCostCategory::material ?
                    line.description : line_label(line.category)) << '\t'
            << replace_line_breaks(line.detail, " / ");
        if (!line.included) out << " - nicht berechnet";
        out << '\t' << money(line.internal_amount_micros, data.currency)
            << '\t' << money(line.invoice_amount_micros, data.currency) << '\n';
    }
    const MoneyMicros net = invoice_net(data);
    const MoneyMicros tax = invoice_tax(data);
    out << "\nInterne Gesamtkosten:\t" << money(invoice_internal_total(data), data.currency)
        << "\nNettobetrag:\t" << money(net, data.currency)
        << "\nUmsatzsteuer:\t" << money(tax, data.currency)
        << "\nGesamtbetrag:\t" << money(checked_invoice_add(net, tax), data.currency) << '\n';
    if (data.small_business)
        out << "\nGemäß § 19 UStG wird keine Umsatzsteuer berechnet.\n";
    return out.str();
}

void write_invoice_pdf(const std::string &path, const InvoiceExportData &data)
{
    if (path.empty())
        throw std::runtime_error("PDF path must not be empty");
    const std::vector<std::string> page_contents = build_pdf_pages(data);
    std::vector<std::string> objects(5 + page_contents.size() * 2);
    objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    std::ostringstream kids;
    for (std::size_t index = 0; index < page_contents.size(); ++index)
        kids << 6 + index * 2 << " 0 R ";
    objects[2] = "<< /Type /Pages /Count " + std::to_string(page_contents.size()) +
                 " /Kids [" + kids.str() + "] >>";
    objects[3] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
    objects[4] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>";
    for (std::size_t index = 0; index < page_contents.size(); ++index) {
        const std::size_t content_id = 5 + index * 2;
        const std::size_t page_id = content_id + 1;
        objects[content_id] = "<< /Length " + std::to_string(page_contents[index].size()) +
            " >>\nstream\n" + page_contents[index] + "endstream";
        objects[page_id] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
            "/Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " +
            std::to_string(content_id) + " 0 R >>";
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Could not create the invoice PDF");
    output << "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::streamoff> offsets(objects.size(), 0);
    for (std::size_t id = 1; id < objects.size(); ++id) {
        offsets[id] = output.tellp();
        output << id << " 0 obj\n" << objects[id] << "\nendobj\n";
    }
    const std::streamoff xref = output.tellp();
    output << "xref\n0 " << objects.size() << "\n"
           << "0000000000 65535 f \n";
    for (std::size_t id = 1; id < objects.size(); ++id)
        output << std::setw(10) << std::setfill('0') << offsets[id]
               << " 00000 n \n";
    output << "trailer\n<< /Size " << objects.size()
           << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    if (!output)
        throw std::runtime_error("Could not finish writing the invoice PDF");
}

void show_customer_invoice_dialog(wxWindow *parent, Store &store,
                                  const CustomerOrder &order,
                                  const Customer &customer)
{
    InvoiceDialog dialog(
        parent, order, customer, store.customer_order_invoice_lines(order.id));
    dialog.ShowModal();
}

} // namespace Slic3r::GUI
