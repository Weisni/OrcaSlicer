#include "FilamentManagerPanel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <sstream>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/cmndata.h>
#include <wx/dataobj.h>
#include <wx/dataview.h>
#include <wx/dialog.h>
#include <wx/icon.h>
#include <wx/msgdlg.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tokenzr.h>

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "CustomerOrderDialogs.hpp"
#include "FilamentInventoryService.hpp"
#include "FilamentSpoolEditor.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TabCtrl.hpp"
#include "wxExtensions.hpp"

namespace Slic3r::GUI {

namespace {

using namespace FilamentInventory;

std::string make_operation_key(const char *prefix, const std::string &entity_id)
{
    static std::atomic<std::uint64_t> sequence {0};
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::string(prefix) + ":" + entity_id + ":" + std::to_string(now) + ":" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

bool parse_number(wxString text, double &value)
{
    text.Trim(true).Trim(false);
    text.Replace(",", ".");
    return !text.empty() && text.ToDouble(&value) && std::isfinite(value);
}

bool grams_to_milligrams(double grams, Milligrams &result)
{
    const long double milligrams = static_cast<long double>(grams) * 1'000.0L;
    if (!std::isfinite(grams) || milligrams < 0.0L ||
        milligrams > static_cast<long double>(std::numeric_limits<Milligrams>::max()))
        return false;
    result = static_cast<Milligrams>(std::llround(milligrams));
    return true;
}

bool currency_to_micros(wxString text, MoneyMicros &result)
{
    double amount = 0.0;
    if (!parse_number(text, amount) || amount < 0.0)
        return false;
    const long double micros = static_cast<long double>(amount) * 1'000'000.0L;
    if (micros > static_cast<long double>(std::numeric_limits<MoneyMicros>::max()))
        return false;
    result = static_cast<MoneyMicros>(std::llround(micros));
    return true;
}

wxString format_weight(Milligrams milligrams, bool show_sign = false)
{
    const double grams = static_cast<double>(milligrams) / 1'000.0;
    if (show_sign)
        return wxString::Format("%+.1f g", grams);
    return wxString::Format("%.1f g", grams);
}

wxString format_money(MoneyMicros micros, const std::string &currency = "EUR")
{
    return wxString::Format("%.2f %s", micros / 1'000'000.0, from_u8(currency));
}

wxString format_optional_money(
    const std::optional<MoneyMicros> &micros, const std::string &currency = "EUR")
{
    return micros ? format_money(*micros, currency) : wxString::FromUTF8("\xE2\x80\x94");
}

wxString format_duration(std::int64_t seconds)
{
    if (seconds <= 0)
        return wxString::FromUTF8("\xE2\x80\x94");
    const std::int64_t rounded_minutes = (seconds + 30) / 60;
    const std::int64_t hours = rounded_minutes / 60;
    const std::int64_t minutes = rounded_minutes % 60;
    return hours > 0 ?
               wxString::Format("%lld h %02lld min",
                                static_cast<long long>(hours),
                                static_cast<long long>(minutes)) :
               wxString::Format("%lld min", static_cast<long long>(minutes));
}

wxString em_dash()
{
    return wxString::FromUTF8("\xE2\x80\x94");
}

wxString em_dash_separator()
{
    return wxString::FromUTF8(" \xE2\x80\x94 ");
}

std::optional<Milligrams> warning_threshold(const Spool &spool)
{
    if (spool.warning_mode == WarningMode::none)
        return std::nullopt;
    if (spool.warning_mode == WarningMode::grams)
        return spool.warning_value;

    const long double value =
        static_cast<long double>(spool.nominal_capacity_mg) *
        static_cast<long double>(spool.warning_value) / 10'000.0L;
    return static_cast<Milligrams>(std::llround(value));
}

bool is_low_stock(const Spool &spool)
{
    const std::optional<Milligrams> threshold = warning_threshold(spool);
    return threshold && spool.available_weight_mg <= *threshold;
}

long fill_level_percent(const Spool &spool)
{
    if (spool.nominal_capacity_mg <= 0)
        return 0;

    const long double percentage =
        static_cast<long double>(spool.current_weight_mg) * 100.0L /
        static_cast<long double>(spool.nominal_capacity_mg);
    return static_cast<long>(std::llround(std::clamp(percentage, 0.0L, 100.0L)));
}

wxString warning_description(const Spool &spool)
{
    const std::optional<Milligrams> threshold = warning_threshold(spool);
    if (!threshold)
        return _L("Off");

    wxString threshold_text = spool.warning_mode == WarningMode::grams ?
                              format_weight(*threshold) :
                              wxString::Format("%.1f%%", static_cast<double>(spool.warning_value) / 100.0);
    return is_low_stock(spool) ? _L("Low") + " (" + threshold_text + ")" : threshold_text;
}

void append_row(wxDataViewListCtrl *list, std::initializer_list<wxString> cells)
{
    wxVector<wxVariant> values;
    values.reserve(cells.size());
    for (const wxString &cell : cells)
        values.emplace_back(cell);
    list->AppendItem(values);
}

void style_data_view(wxDataViewListCtrl *list)
{
    list->SetRowHeight(list->FromDIP(42));
    wxGetApp().UpdateDVCDarkUI(list);
    list->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    list->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    list->SetAlternateRowColour(StateColor::darkModeColorFor(wxColour("#F8F8F8")));
}

wxStaticText *add_summary_card(wxWindow *parent, wxBoxSizer *row, const wxString &caption)
{
    auto *card = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    const wxColour background = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    card->SetBackgroundColour(background);

    auto *card_sizer = new wxBoxSizer(wxVERTICAL);
    auto *caption_text = new wxStaticText(card, wxID_ANY, caption);
    caption_text->SetBackgroundColour(background);
    caption_text->SetForegroundColour(
        StateColor::darkModeColorFor(wxColour("#6B6B6B")));
    auto *value = new wxStaticText(card, wxID_ANY, "0");
    value->SetBackgroundColour(background);
    value->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    wxFont value_font = value->GetFont();
    value_font.SetWeight(wxFONTWEIGHT_BOLD);
    value_font.SetPointSize(value_font.GetPointSize() + 2);
    value->SetFont(value_font);

    card_sizer->Add(caption_text, 0, wxBOTTOM, parent->FromDIP(4));
    card_sizer->Add(value);
    card->SetSizer(card_sizer);
    card_sizer->SetSizeHints(card);

    row->Add(card, 1, wxEXPAND | wxALL, parent->FromDIP(6));
    return value;
}

class SpoolEditorDialog : public wxDialog
{
public:
    SpoolEditorDialog(
        wxWindow *parent, const Spool *spool, const SpoolInput *defaults = nullptr,
        const std::vector<SpoolIdentifierInput> *identifier_defaults = nullptr)
        : wxDialog(parent, wxID_ANY, spool ? _L("Edit filament spool") : _L("Add filament spool"),
                   wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_create(spool == nullptr)
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *description = new wxStaticText(
            this, wxID_ANY,
            _L("Describe the physical spool and its current stock. The selected sRGB colour is stored with the spool."));
        description->Wrap(FromDIP(720));
        root->Add(description, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        auto *columns = new wxBoxSizer(wxHORIZONTAL);
        auto *left_column = new wxBoxSizer(wxVERTICAL);
        auto *right_column = new wxBoxSizer(wxVERTICAL);

        const auto make_grid = [this]() {
            auto *grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(12));
            grid->AddGrowableCol(1, 1);
            return grid;
        };
        const auto add_text_row =
            [this](wxFlexGridSizer *grid, const wxString &label, wxTextCtrl *&control,
                   const wxString &value = {}) {
                grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
                control = new wxTextCtrl(
                    this, wxID_ANY, value, wxDefaultPosition, wxSize(FromDIP(250), -1));
                grid->Add(control, 1, wxEXPAND);
            };

        auto *basic_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Spool details"));
        auto *basic_grid = make_grid();
        add_text_row(basic_grid, _L("Name"), m_name);
        add_text_row(basic_grid, _L("Manufacturer"), m_manufacturer);
        add_text_row(basic_grid, _L("Material type"), m_material);
        basic_grid->Add(
            new wxStaticText(this, wxID_ANY, _L("sRGB colour")), 0, wxALIGN_CENTER_VERTICAL);
        auto *color_row = new wxBoxSizer(wxHORIZONTAL);
        m_color_swatch = new wxPanel(
            this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(38), FromDIP(24)),
            wxBORDER_SIMPLE);
        m_color_swatch->SetMinSize(wxSize(FromDIP(38), FromDIP(24)));
        m_color_swatch->SetToolTip(_L("Click to choose the spool colour"));
        m_color_text = new wxStaticText(this, wxID_ANY, "#FFFFFF");
        m_color_text->SetMinSize(wxSize(FromDIP(72), -1));
        m_color_button = new wxButton(this, wxID_ANY, _L("Choose..."));
        m_color_button->SetToolTip(_L("Open the system sRGB colour picker"));
        color_row->Add(m_color_swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        color_row->Add(m_color_text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        color_row->Add(m_color_button, 1, wxALIGN_CENTER_VERTICAL);
        basic_grid->Add(color_row, 1, wxEXPAND);
        basic_box->Add(basic_grid, 1, wxEXPAND | wxALL, FromDIP(10));
        left_column->Add(basic_box, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

        auto *stock_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Stock and properties"));
        auto *stock_grid = make_grid();
        add_text_row(stock_grid, _L("Nominal fill (g)"), m_capacity, "1000");
        add_text_row(stock_grid, _L("Current fill (g)"), m_remaining, "1000");
        add_text_row(stock_grid, _L("Diameter (mm)"), m_diameter, "1.75");
        add_text_row(stock_grid, _L("Density (g/cm³)"), m_density, "1.24");
        stock_box->Add(stock_grid, 1, wxEXPAND | wxALL, FromDIP(10));
        left_column->Add(stock_box, 0, wxEXPAND);

        auto *warning_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Low-stock warning"));
        auto *warning_grid = make_grid();
        warning_grid->Add(
            new wxStaticText(this, wxID_ANY, _L("Warning type")), 0, wxALIGN_CENTER_VERTICAL);
        m_warning_mode = new wxChoice(this, wxID_ANY);
        m_warning_mode->Append(_L("Off"));
        m_warning_mode->Append(_L("Grams"));
        m_warning_mode->Append(_L("Percent"));
        m_warning_mode->SetSelection(0);
        warning_grid->Add(m_warning_mode, 1, wxEXPAND);
        warning_grid->Add(
            new wxStaticText(this, wxID_ANY, _L("Warning threshold")),
            0, wxALIGN_CENTER_VERTICAL);
        auto *warning_value_row = new wxBoxSizer(wxHORIZONTAL);
        m_warning_value = new wxTextCtrl(
            this, wxID_ANY, "0", wxDefaultPosition, wxSize(FromDIP(180), -1));
        m_warning_unit = new wxStaticText(this, wxID_ANY, {});
        m_warning_unit->SetMinSize(wxSize(FromDIP(16), -1));
        warning_value_row->Add(m_warning_value, 1, wxRIGHT, FromDIP(8));
        warning_value_row->Add(m_warning_unit, 0, wxALIGN_CENTER_VERTICAL);
        warning_grid->Add(warning_value_row, 1, wxEXPAND);
        warning_box->Add(warning_grid, 0, wxEXPAND | wxALL, FromDIP(10));
        auto *warning_help = new wxStaticText(
            this, wxID_ANY,
            _L("Show a warning when the available material reaches this limit."));
        warning_help->Wrap(FromDIP(300));
        warning_box->Add(
            warning_help, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        right_column->Add(warning_box, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

        auto *advanced_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Advanced"));
        auto *advanced_grid = make_grid();
        add_text_row(advanced_grid, _L("Filament preset ID"), m_preset_id);
        add_text_row(
            advanced_grid, _L("Material price (EUR/kg)"), m_material_price, "0.00");
        advanced_box->Add(advanced_grid, 1, wxEXPAND | wxALL, FromDIP(10));
        right_column->Add(advanced_box, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

        if (m_create) {
            auto *tags_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Tags (optional)"));
            auto *tags_grid = make_grid();
            add_text_row(tags_grid, _L("NFC tag UID"), m_nfc_uid);
            add_text_row(tags_grid, _L("Bambu RFID UID"), m_bambu_uid);
            tags_box->Add(tags_grid, 0, wxEXPAND | wxALL, FromDIP(10));
            auto *tag_help = new wxStaticText(
                this, wxID_ANY,
                _L("Tags can also be assigned later from the spool overview."));
            tag_help->Wrap(FromDIP(300));
            tags_box->Add(
                tag_help, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
            right_column->Add(tags_box, 0, wxEXPAND);
            if (identifier_defaults != nullptr) {
                for (const SpoolIdentifierInput &identifier : *identifier_defaults) {
                    if (identifier.kind == IdentifierKind::nfc_uid)
                        m_nfc_uid->SetValue(from_u8(identifier.value));
                    else if (identifier.kind == IdentifierKind::bambu_tag_uid)
                        m_bambu_uid->SetValue(from_u8(identifier.value));
                }
            }
        }

        columns->Add(left_column, 1, wxEXPAND | wxRIGHT, FromDIP(10));
        columns->Add(right_column, 1, wxEXPAND);
        root->Add(columns, 1, wxEXPAND | wxALL, FromDIP(16));

        set_color(wxColour(255, 255, 255));
        m_color_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_color(); });
        m_color_swatch->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) { choose_color(); });
        m_warning_mode->Bind(
            wxEVT_CHOICE, [this](wxCommandEvent &) { update_warning_controls(); });

        if (defaults != nullptr) {
            m_manufacturer->SetValue(from_u8(defaults->manufacturer));
            m_material->SetValue(from_u8(defaults->material_type));
            m_name->SetValue(from_u8(defaults->name));
            m_preset_id->SetValue(from_u8(defaults->filament_preset_id));
            m_material_price->SetValue(wxString::Format(
                "%.2f", defaults->material_price_per_kg_micros / 1'000'000.0));
            m_diameter->SetValue(wxString::Format("%.3f", defaults->diameter_mm));
            m_density->SetValue(wxString::Format("%.3f", defaults->density_g_cm3));
            m_capacity->SetValue(
                wxString::Format("%.3f", defaults->nominal_capacity_mg / 1'000.0));
            m_remaining->SetValue(
                wxString::Format("%.3f", defaults->initial_weight_mg / 1'000.0));
            set_color_from_hex(from_u8(defaults->color_hex));
            m_warning_mode->SetSelection(
                defaults->warning_mode == WarningMode::none ? 0 :
                defaults->warning_mode == WarningMode::grams ? 1 : 2);
            if (defaults->warning_mode == WarningMode::grams)
                m_warning_value->SetValue(
                    wxString::Format("%.3f", defaults->warning_value / 1'000.0));
            else if (defaults->warning_mode == WarningMode::percent)
                m_warning_value->SetValue(
                    wxString::Format("%.2f", defaults->warning_value / 100.0));
        } else if (spool != nullptr) {
            m_manufacturer->SetValue(from_u8(spool->manufacturer));
            m_material->SetValue(from_u8(spool->material_type));
            m_name->SetValue(from_u8(spool->name));
            m_preset_id->SetValue(from_u8(spool->filament_preset_id));
            m_material_price->SetValue(wxString::Format(
                "%.2f", spool->material_price_per_kg_micros / 1'000'000.0));
            m_diameter->SetValue(wxString::Format("%.3f", spool->diameter_mm));
            m_density->SetValue(wxString::Format("%.3f", spool->density_g_cm3));
            m_capacity->SetValue(wxString::Format("%.3f", spool->nominal_capacity_mg / 1'000.0));
            m_remaining->SetValue(wxString::Format("%.3f", spool->current_weight_mg / 1'000.0));
            set_color_from_hex(from_u8(spool->color_hex));
            m_warning_mode->SetSelection(
                spool->warning_mode == WarningMode::none ? 0 :
                spool->warning_mode == WarningMode::grams ? 1 : 2);
            if (spool->warning_mode == WarningMode::grams)
                m_warning_value->SetValue(wxString::Format("%.3f", spool->warning_value / 1'000.0));
            else if (spool->warning_mode == WarningMode::percent)
                m_warning_value->SetValue(wxString::Format("%.2f", spool->warning_value / 100.0));
        }

        update_warning_controls();
        root->Add(
            CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0,
            wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(760), GetSize().y));
        wxGetApp().UpdateDarkUI(this);
        set_color(m_color_value);
        CentreOnParent();
    }

    bool read(SpoolInput &input, std::vector<SpoolIdentifierInput> &identifiers, wxString &error) const
    {
        try {
            input.manufacturer     = into_u8(m_manufacturer->GetValue());
            input.material_type    = into_u8(m_material->GetValue());
            input.name             = into_u8(m_name->GetValue());
            input.filament_preset_id = into_u8(m_preset_id->GetValue());

            double diameter = 0.0;
            double density = 0.0;
            double capacity = 0.0;
            double remaining = 0.0;
            if (!parse_number(m_diameter->GetValue(), diameter) ||
                !parse_number(m_density->GetValue(), density) ||
                !parse_number(m_capacity->GetValue(), capacity) ||
                !parse_number(m_remaining->GetValue(), remaining))
                throw Error(ErrorCode::validation, "Diameter, density and fill levels must be valid numbers");

            input.diameter_mm   = diameter;
            input.density_g_cm3 = density;
            if (!grams_to_milligrams(capacity, input.nominal_capacity_mg) ||
                input.nominal_capacity_mg <= 0 ||
                !grams_to_milligrams(remaining, input.initial_weight_mg))
                throw Error(ErrorCode::validation, "Fill levels are outside the supported range");

            input.color_hex = canonical_color(
                ColorModel::hex,
                into_u8(m_color_value.GetAsString(wxC2S_HTML_SYNTAX)));
            if (!currency_to_micros(
                    m_material_price->GetValue(),
                    input.material_price_per_kg_micros))
                throw Error(
                    ErrorCode::validation,
                    "Material price must be a non-negative amount");
            input.price_currency = "EUR";

            const int warning_selection = m_warning_mode->GetSelection();
            input.warning_mode = warning_selection == 1 ? WarningMode::grams :
                                 warning_selection == 2 ? WarningMode::percent :
                                                          WarningMode::none;
            input.warning_value = 0;
            if (input.warning_mode != WarningMode::none) {
                double warning = 0.0;
                if (!parse_number(m_warning_value->GetValue(), warning) || warning < 0.0)
                    throw Error(ErrorCode::validation, "Warning threshold must be a non-negative number");
                if (input.warning_mode == WarningMode::grams) {
                    if (!grams_to_milligrams(warning, input.warning_value))
                        throw Error(ErrorCode::validation, "Warning threshold is outside the supported range");
                } else {
                    if (warning > 100.0)
                        throw Error(ErrorCode::validation, "Percentage warning threshold must not exceed 100");
                    input.warning_value = static_cast<std::int64_t>(std::llround(warning * 100.0));
                }
            }

            identifiers.clear();
            if (m_create && m_nfc_uid != nullptr && !m_nfc_uid->GetValue().Trim().empty())
                identifiers.push_back({IdentifierKind::nfc_uid, into_u8(m_nfc_uid->GetValue())});
            if (m_create && m_bambu_uid != nullptr && !m_bambu_uid->GetValue().Trim().empty())
                identifiers.push_back({IdentifierKind::bambu_tag_uid, into_u8(m_bambu_uid->GetValue())});
            return true;
        } catch (const std::exception &exception) {
            error = from_u8(exception.what());
            return false;
        }
    }

private:
    void choose_color()
    {
        wxColourData data;
        data.SetChooseFull(true);
        data.SetColour(m_color_value);
        set_color(show_sys_picker_dialog(this, data).GetColour());
    }

    void set_color_from_hex(const wxString &hex)
    {
        const wxColour color(hex);
        if (color.IsOk())
            set_color(color);
    }

    void set_color(const wxColour &color)
    {
        if (!color.IsOk())
            return;

        m_color_value = color;
        if (m_color_text != nullptr)
            m_color_text->SetLabel(m_color_value.GetAsString(wxC2S_HTML_SYNTAX).Upper());
        if (m_color_swatch != nullptr) {
            m_color_swatch->SetBackgroundColour(m_color_value);
            m_color_swatch->Refresh();
        }
    }

    void update_warning_controls()
    {
        const int selection = m_warning_mode != nullptr ? m_warning_mode->GetSelection() : 0;
        if (m_warning_value != nullptr)
            m_warning_value->Enable(selection != 0);
        if (m_warning_unit != nullptr)
            m_warning_unit->SetLabel(selection == 1 ? "g" : selection == 2 ? "%" : wxString {});
    }

    bool        m_create {false};
    wxTextCtrl *m_manufacturer {nullptr};
    wxTextCtrl *m_material {nullptr};
    wxTextCtrl *m_name {nullptr};
    wxTextCtrl *m_preset_id {nullptr};
    wxTextCtrl *m_material_price {nullptr};
    wxTextCtrl *m_diameter {nullptr};
    wxTextCtrl *m_density {nullptr};
    wxTextCtrl *m_capacity {nullptr};
    wxTextCtrl *m_remaining {nullptr};
    wxColour    m_color_value {255, 255, 255};
    wxPanel    *m_color_swatch {nullptr};
    wxStaticText *m_color_text {nullptr};
    wxButton   *m_color_button {nullptr};
    wxChoice   *m_warning_mode {nullptr};
    wxTextCtrl *m_warning_value {nullptr};
    wxStaticText *m_warning_unit {nullptr};
    wxTextCtrl *m_nfc_uid {nullptr};
    wxTextCtrl *m_bambu_uid {nullptr};
};

class IdentifierEditorDialog : public wxDialog
{
public:
    IdentifierEditorDialog(
        wxWindow *parent, const Spool &spool, const std::vector<SpoolIdentifier> &identifiers)
        : wxDialog(parent, wxID_ANY, _L("Manage spool tags"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        wxString nfc_uids;
        wxString bambu_uids;
        for (const SpoolIdentifier &identifier : identifiers) {
            wxString *target = identifier.kind == IdentifierKind::nfc_uid ? &nfc_uids :
                               identifier.kind == IdentifierKind::bambu_tag_uid ? &bambu_uids :
                                                                                 nullptr;
            if (target == nullptr)
                continue;
            if (!target->empty())
                *target += "\n";
            *target += from_u8(identifier.value);
        }

        auto *root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(
                      this, wxID_ANY,
                      _L("Enter one hardware UID per line. Empty fields remove the corresponding physical tag assignments.")),
                  0, wxEXPAND | wxALL, FromDIP(12));

        auto *grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(12));
        grid->AddGrowableCol(1, 1);
        grid->Add(new wxStaticText(this, wxID_ANY, _L("NFC tag UIDs")),
                  0, wxALIGN_TOP | wxTOP, FromDIP(4));
        m_nfc_uids = new wxTextCtrl(
            this, wxID_ANY, nfc_uids, wxDefaultPosition, wxSize(FromDIP(320), FromDIP(72)),
            wxTE_MULTILINE);
        grid->Add(m_nfc_uids, 1, wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Bambu RFID UIDs")),
                  0, wxALIGN_TOP | wxTOP, FromDIP(4));
        m_bambu_uids = new wxTextCtrl(
            this, wxID_ANY, bambu_uids, wxDefaultPosition, wxSize(FromDIP(320), FromDIP(72)),
            wxTE_MULTILINE);
        grid->Add(m_bambu_uids, 1, wxEXPAND);
        root->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        root->Add(new wxStaticText(
                      this, wxID_ANY,
                      _L("NDEF payload for this spool:") + "\nquackslicer://spool/" +
                          from_u8(spool.id)),
                  0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL),
                  0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(520), GetSize().y));
        CentreOnParent();
    }

    std::vector<SpoolIdentifierInput> identifiers() const
    {
        std::vector<SpoolIdentifierInput> result;
        append_identifiers(result, IdentifierKind::nfc_uid, m_nfc_uids->GetValue());
        append_identifiers(result, IdentifierKind::bambu_tag_uid, m_bambu_uids->GetValue());
        return result;
    }

private:
    static void append_identifiers(
        std::vector<SpoolIdentifierInput> &target, IdentifierKind kind, wxString text)
    {
        text.Replace(";", "\n");
        wxStringTokenizer tokenizer(text, "\r\n", wxTOKEN_STRTOK);
        while (tokenizer.HasMoreTokens()) {
            wxString value = tokenizer.GetNextToken();
            value.Trim(true).Trim(false);
            if (!value.empty())
                target.push_back({kind, into_u8(value)});
        }
    }

    wxTextCtrl *m_nfc_uids {nullptr};
    wxTextCtrl *m_bambu_uids {nullptr};
};

} // namespace

std::optional<Spool> create_filament_spool_interactively(
    wxWindow *parent, FilamentInventory::Store &store, const SpoolInput *defaults,
    const std::vector<SpoolIdentifierInput> *identifier_defaults)
{
    SpoolEditorDialog dialog(parent, nullptr, defaults, identifier_defaults);
    while (dialog.ShowModal() == wxID_OK) {
        SpoolInput input;
        std::vector<SpoolIdentifierInput> identifiers;
        wxString error;
        if (!dialog.read(input, identifiers, error)) {
            wxMessageBox(error, _L("Filament Manager"), wxOK | wxICON_WARNING, parent);
            continue;
        }
        try {
            return store.create_spool(input, identifiers);
        } catch (const std::exception &exception) {
            wxMessageBox(
                from_u8(exception.what()), _L("Filament Manager"),
                wxOK | wxICON_ERROR, parent);
        }
    }
    return std::nullopt;
}

FilamentManagerPanel::FilamentManagerPanel(wxWindow *parent, wxWindowID id,
                                           const wxPoint &position, const wxSize &size,
                                           long style)
    : wxPanel(parent, id, position, size, style)
    , m_refresh_timer(this)
{
    auto *root = new wxBoxSizer(wxVERTICAL);
    const wxColour page_background =
        StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    const wxColour header_background =
        StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    SetBackgroundColour(page_background);

    auto *header = new wxPanel(this, wxID_ANY);
    header->SetBackgroundColour(header_background);
    auto *header_sizer = new wxBoxSizer(wxVERTICAL);
    auto *heading = new wxStaticText(header, wxID_ANY, _L("Filament Manager"));
    heading->SetBackgroundColour(header_background);
    heading->SetForegroundColour(
        StateColor::darkModeColorFor(wxColour("#009688")));
    wxFont heading_font = heading->GetFont();
    heading_font.SetWeight(wxFONTWEIGHT_BOLD);
    heading_font.SetPointSize(heading_font.GetPointSize() + 4);
    heading->SetFont(heading_font);
    header_sizer->Add(
        heading, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *description = new wxStaticText(
        header, wxID_ANY,
        _L("Track physical spools, reserve sliced material, and confirm completed print jobs."));
    description->SetBackgroundColour(header_background);
    description->SetForegroundColour(
        StateColor::darkModeColorFor(wxColour("#6B6B6B")));
    header_sizer->Add(
        description, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(16));
    header->SetSizer(header_sizer);
    root->Add(
        header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_tabs = new TabCtrl(
        this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES |
            wxBORDER_NONE | wxWANTS_CHARS | wxTR_FULL_ROW_HIGHLIGHT);
    m_tabs->SetBackgroundColour(page_background);
    root->Add(
        m_tabs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_pages = new wxSimplebook(
        this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_pages->SetBackgroundColour(page_background);

    auto *spools_page = new wxPanel(m_pages);
    spools_page->SetBackgroundColour(page_background);
    auto *spools_sizer = new wxBoxSizer(wxVERTICAL);
    auto *spools_heading = new wxStaticText(spools_page, wxID_ANY, _L("Physical spools"));
    wxFont spools_heading_font = spools_heading->GetFont();
    spools_heading_font.SetWeight(wxFONTWEIGHT_BOLD);
    spools_heading_font.SetPointSize(spools_heading_font.GetPointSize() + 1);
    spools_heading->SetFont(spools_heading_font);
    spools_sizer->Add(
        spools_heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto *spools_help = new wxStaticText(
        spools_page, wxID_ANY,
        _L("Double-click a spool to edit its properties, colour, stock level, and warning."));
    spools_sizer->Add(
        spools_help, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

    auto *summary = new wxBoxSizer(wxHORIZONTAL);
    m_active_spools_value =
        add_summary_card(spools_page, summary, _L("Active spools"));
    m_available_value =
        add_summary_card(spools_page, summary, _L("Available material"));
    m_reserved_value =
        add_summary_card(spools_page, summary, _L("Reserved material"));
    m_low_stock_value =
        add_summary_card(spools_page, summary, _L("Low stock"));
    spools_sizer->Add(
        summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));

    auto *spool_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_add_button = new wxButton(spools_page, wxID_ANY, _L("Add spool"));
    m_edit_button = new wxButton(spools_page, wxID_ANY, _L("Edit"));
    m_remaining_button = new wxButton(spools_page, wxID_ANY, _L("Set remaining"));
    m_archive_button = new wxButton(spools_page, wxID_ANY, _L("Archive"));
    m_identifiers_button = new wxButton(spools_page, wxID_ANY, _L("Manage tags"));
    m_copy_nfc_button = new wxButton(spools_page, wxID_ANY, _L("Copy NFC payload"));
    auto *refresh_spools_button = new wxButton(spools_page, wxID_ANY, _L("Refresh"));
    m_add_button->SetToolTip(_L("Register a physical filament spool"));
    m_edit_button->SetToolTip(_L("Edit the selected spool"));
    m_remaining_button->SetToolTip(_L("Correct the current stock level"));
    m_identifiers_button->SetToolTip(_L("Assign NFC or Bambu RFID identifiers"));
    m_copy_nfc_button->SetToolTip(_L("Copy the NFC link for the selected spool"));
    m_archive_button->SetToolTip(_L("Hide a spool that is no longer in use"));
    spool_buttons->Add(m_add_button, 0, wxRIGHT, FromDIP(12));
    for (wxButton *button : {
             m_edit_button, m_remaining_button, m_identifiers_button, m_copy_nfc_button})
        spool_buttons->Add(button, 0, wxRIGHT, FromDIP(8));
    spool_buttons->AddStretchSpacer();
    spool_buttons->Add(m_archive_button, 0, wxRIGHT, FromDIP(8));
    spool_buttons->Add(refresh_spools_button);
    spools_sizer->Add(spool_buttons, 0, wxEXPAND | wxALL, FromDIP(10));

    m_spool_list = new wxDataViewListCtrl(spools_page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxDV_ROW_LINES | wxBORDER_NONE);
    m_spool_list->AppendIconTextColumn(
        _L("Spool"), wxDATAVIEW_CELL_INERT, FromDIP(210));
    m_spool_list->AppendTextColumn(_L("Manufacturer"), wxDATAVIEW_CELL_INERT, FromDIP(140));
    m_spool_list->AppendTextColumn(_L("Material"), wxDATAVIEW_CELL_INERT, FromDIP(90));
    m_spool_list->AppendTextColumn(
        _L("Price/kg"), wxDATAVIEW_CELL_INERT, FromDIP(100), wxALIGN_RIGHT);
    m_spool_list->AppendProgressColumn(
        _L("Fill level"), wxDATAVIEW_CELL_INERT, FromDIP(125), wxALIGN_CENTER);
    m_spool_list->AppendTextColumn(
        _L("Remaining / capacity"), wxDATAVIEW_CELL_INERT, FromDIP(150), wxALIGN_RIGHT);
    m_spool_list->AppendTextColumn(
        _L("Reserved"), wxDATAVIEW_CELL_INERT, FromDIP(100), wxALIGN_RIGHT);
    m_spool_list->AppendTextColumn(
        _L("Available"), wxDATAVIEW_CELL_INERT, FromDIP(100), wxALIGN_RIGHT);
    m_spool_list->AppendTextColumn(_L("Warning"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_spool_list->AppendTextColumn(_L("Identifiers"), wxDATAVIEW_CELL_INERT, FromDIP(140));
    style_data_view(m_spool_list);
    spools_sizer->Add(m_spool_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    spools_page->SetSizer(spools_sizer);

    auto *jobs_page = new wxPanel(m_pages);
    jobs_page->SetBackgroundColour(page_background);
    auto *jobs_sizer = new wxBoxSizer(wxVERTICAL);
    auto *job_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_confirm_button = new wxButton(jobs_page, wxID_ANY, _L("Confirm estimated usage"));
    m_correct_button = new wxButton(jobs_page, wxID_ANY, _L("Correct and confirm"));
    m_review_button = new wxButton(jobs_page, wxID_ANY, _L("Review manually"));
    m_discard_button = new wxButton(jobs_page, wxID_ANY, _L("Discard"));
    auto *refresh_jobs_button = new wxButton(jobs_page, wxID_ANY, _L("Refresh"));
    for (wxButton *button : {
             m_confirm_button, m_correct_button, m_review_button,
             m_discard_button, refresh_jobs_button})
        job_buttons->Add(button, 0, wxRIGHT, FromDIP(8));
    jobs_sizer->Add(job_buttons, 0, wxEXPAND | wxALL, FromDIP(10));

    m_job_list = new wxDataViewListCtrl(jobs_page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxDV_ROW_LINES | wxBORDER_NONE);
    m_job_list->AppendTextColumn(_L("Job"), wxDATAVIEW_CELL_INERT, FromDIP(220));
    m_job_list->AppendTextColumn(_L("Customer order"), wxDATAVIEW_CELL_INERT, FromDIP(210));
    m_job_list->AppendTextColumn(_L("State"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_list->AppendTextColumn(_L("Printer"), wxDATAVIEW_CELL_INERT, FromDIP(160));
    m_job_list->AppendTextColumn(_L("Filaments"), wxDATAVIEW_CELL_INERT, FromDIP(90));
    m_job_list->AppendTextColumn(_L("Reserved"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_list->AppendTextColumn(_L("Runtime"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_list->AppendTextColumn(_L("Est. cost"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_list->AppendTextColumn(_L("Created"), wxDATAVIEW_CELL_INERT, FromDIP(190));
    style_data_view(m_job_list);
    jobs_sizer->Add(m_job_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    jobs_page->SetSizer(jobs_sizer);

    auto *history_page = new wxPanel(m_pages);
    history_page->SetBackgroundColour(page_background);
    auto *history_sizer = new wxBoxSizer(wxVERTICAL);
    auto *refresh_history_button = new wxButton(history_page, wxID_ANY, _L("Refresh"));
    history_sizer->Add(refresh_history_button, 0, wxALL, FromDIP(10));
    m_history_list = new wxDataViewListCtrl(history_page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                            wxDV_ROW_LINES | wxBORDER_NONE);
    m_history_list->AppendTextColumn(_L("Time"), wxDATAVIEW_CELL_INERT, FromDIP(190));
    m_history_list->AppendTextColumn(_L("Spool"), wxDATAVIEW_CELL_INERT, FromDIP(180));
    m_history_list->AppendTextColumn(_L("Type"), wxDATAVIEW_CELL_INERT, FromDIP(120));
    m_history_list->AppendTextColumn(_L("Change"), wxDATAVIEW_CELL_INERT, FromDIP(100));
    m_history_list->AppendTextColumn(_L("Balance"), wxDATAVIEW_CELL_INERT, FromDIP(100));
    m_history_list->AppendTextColumn(_L("Note"), wxDATAVIEW_CELL_INERT, FromDIP(320));
    style_data_view(m_history_list);
    history_sizer->Add(m_history_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    history_page->SetSizer(history_sizer);

    m_pages->AddPage(spools_page, wxEmptyString, true);
    m_pages->AddPage(jobs_page, wxEmptyString);
    m_pages->AddPage(history_page, wxEmptyString);

    auto *job_history_page = new wxPanel(m_pages);
    job_history_page->SetBackgroundColour(page_background);
    auto *job_history_sizer = new wxBoxSizer(wxVERTICAL);
    auto *refresh_job_history_button = new wxButton(
        job_history_page, wxID_ANY, _L("Refresh"));
    job_history_sizer->Add(refresh_job_history_button, 0, wxALL, FromDIP(10));
    m_job_history_list = new wxDataViewListCtrl(
        job_history_page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxDV_ROW_LINES | wxBORDER_NONE);
    m_job_history_list->AppendTextColumn(_L("Job"), wxDATAVIEW_CELL_INERT, FromDIP(220));
    m_job_history_list->AppendTextColumn(_L("Customer order"), wxDATAVIEW_CELL_INERT, FromDIP(210));
    m_job_history_list->AppendTextColumn(_L("State"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_history_list->AppendTextColumn(_L("Printer"), wxDATAVIEW_CELL_INERT, FromDIP(160));
    m_job_history_list->AppendTextColumn(_L("Estimated"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_history_list->AppendTextColumn(_L("Actual"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_history_list->AppendTextColumn(_L("Runtime"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_history_list->AppendTextColumn(_L("Total cost"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_job_history_list->AppendTextColumn(_L("Created"), wxDATAVIEW_CELL_INERT, FromDIP(190));
    m_job_history_list->AppendTextColumn(_L("Completed"), wxDATAVIEW_CELL_INERT, FromDIP(190));
    style_data_view(m_job_history_list);
    job_history_sizer->Add(
        m_job_history_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    job_history_page->SetSizer(job_history_sizer);
    m_pages->AddPage(job_history_page, wxEmptyString);

    auto *customers_page = new wxPanel(m_pages);
    customers_page->SetBackgroundColour(page_background);
    auto *customers_sizer = new wxBoxSizer(wxVERTICAL);

    auto *cost_bar = new wxBoxSizer(wxHORIZONTAL);
    auto *cost_text = new wxStaticText(
        customers_page, wxID_ANY,
        _L("Costs combine material usage with sliced machine runtime and electricity."));
    auto *cost_settings_button = new wxButton(
        customers_page, wxID_ANY, _L("Cost settings..."));
    cost_bar->Add(cost_text, 0, wxALIGN_CENTER_VERTICAL);
    cost_bar->AddStretchSpacer();
    cost_bar->Add(cost_settings_button);
    customers_sizer->Add(cost_bar, 0, wxEXPAND | wxALL, FromDIP(10));

    auto *customer_toolbar = new wxBoxSizer(wxHORIZONTAL);
    auto *add_customer_button = new wxButton(
        customers_page, wxID_ANY, _L("Add customer"));
    m_edit_customer_button = new wxButton(
        customers_page, wxID_ANY, _L("Edit customer"));
    m_archive_customer_button = new wxButton(
        customers_page, wxID_ANY, _L("Archive customer"));
    customer_toolbar->Add(add_customer_button, 0, wxRIGHT, FromDIP(8));
    customer_toolbar->Add(m_edit_customer_button, 0, wxRIGHT, FromDIP(8));
    customer_toolbar->Add(m_archive_customer_button, 0, wxRIGHT, FromDIP(8));
    customers_sizer->Add(
        customer_toolbar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_customer_list = new wxDataViewListCtrl(
        customers_page, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(170)),
        wxDV_ROW_LINES | wxBORDER_NONE);
    m_customer_list->AppendTextColumn(_L("Customer"), wxDATAVIEW_CELL_INERT, FromDIP(210));
    m_customer_list->AppendTextColumn(_L("Contact"), wxDATAVIEW_CELL_INERT, FromDIP(180));
    m_customer_list->AppendTextColumn(_L("Email"), wxDATAVIEW_CELL_INERT, FromDIP(220));
    m_customer_list->AppendTextColumn(_L("Phone"), wxDATAVIEW_CELL_INERT, FromDIP(140));
    m_customer_list->AppendTextColumn(_L("Accumulated cost"), wxDATAVIEW_CELL_INERT, FromDIP(140));
    style_data_view(m_customer_list);
    customers_sizer->Add(
        m_customer_list, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto *order_toolbar = new wxBoxSizer(wxHORIZONTAL);
    m_add_order_button = new wxButton(customers_page, wxID_ANY, _L("Add order"));
    m_edit_order_button = new wxButton(customers_page, wxID_ANY, _L("Edit order"));
    m_activate_order_button = new wxButton(customers_page, wxID_ANY, _L("Activate"));
    m_complete_order_button = new wxButton(customers_page, wxID_ANY, _L("Complete"));
    m_cancel_order_button = new wxButton(customers_page, wxID_ANY, _L("Cancel order"));
    m_delete_order_button = new wxButton(customers_page, wxID_ANY, _L("Delete draft"));
    auto *refresh_customers_button = new wxButton(customers_page, wxID_ANY, _L("Refresh"));
    for (wxButton *button : {
             m_add_order_button, m_edit_order_button, m_activate_order_button,
             m_complete_order_button, m_cancel_order_button, m_delete_order_button})
        order_toolbar->Add(button, 0, wxRIGHT, FromDIP(8));
    order_toolbar->AddStretchSpacer();
    order_toolbar->Add(refresh_customers_button);
    customers_sizer->Add(
        order_toolbar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_order_list = new wxDataViewListCtrl(
        customers_page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxDV_ROW_LINES | wxBORDER_NONE);
    m_order_list->AppendTextColumn(_L("Order"), wxDATAVIEW_CELL_INERT, FromDIP(210));
    m_order_list->AppendTextColumn(_L("Customer"), wxDATAVIEW_CELL_INERT, FromDIP(180));
    m_order_list->AppendTextColumn(_L("Status"), wxDATAVIEW_CELL_INERT, FromDIP(100));
    m_order_list->AppendTextColumn(_L("Runtime"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_order_list->AppendTextColumn(_L("Material"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_order_list->AppendTextColumn(_L("Electricity"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_order_list->AppendTextColumn(_L("Total cost"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_order_list->AppendTextColumn(_L("Quoted"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    m_order_list->AppendTextColumn(_L("Invoice"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    style_data_view(m_order_list);
    customers_sizer->Add(
        m_order_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    customers_page->SetSizer(customers_sizer);
    m_pages->AddPage(customers_page, wxEmptyString);

    m_tabs->AppendItem(_L("Spools"));
    m_tabs->AppendItem(_L("Open print jobs"));
    m_tabs->AppendItem(_L("Stock history"));
    m_tabs->AppendItem(_L("Job history"));
    m_tabs->AppendItem(_L("Customers & orders"));
    m_tabs->Bind(wxEVT_TAB_SEL_CHANGED, [this](wxCommandEvent &event) {
        const int selection = event.GetSelection();
        if (selection >= 0 && selection < static_cast<int>(m_pages->GetPageCount()))
            m_pages->SetSelection(selection);
        for (unsigned int index = 0; index < m_tabs->GetCount(); ++index)
            m_tabs->SetItemBold(index, static_cast<int>(index) == selection);
    });
    m_tabs->SelectItem(0);

    root->Add(
        m_pages, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    SetSizer(root);

    m_add_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_spool(); });
    m_edit_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { edit_spool(); });
    m_remaining_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { set_remaining(); });
    m_archive_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { archive_spool(); });
    m_identifiers_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        manage_identifiers();
    });
    m_copy_nfc_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { copy_nfc_link(); });
    refresh_spools_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh(); });
    refresh_jobs_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh(); });
    refresh_history_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh(); });
    refresh_job_history_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh(); });
    m_confirm_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { confirm_job(false); });
    m_correct_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { confirm_job(true); });
    m_review_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { review_job(); });
    m_discard_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { discard_job(); });
    add_customer_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_customer(); });
    m_edit_customer_button->Bind(
        wxEVT_BUTTON, [this](wxCommandEvent &) { edit_customer(); });
    m_archive_customer_button->Bind(
        wxEVT_BUTTON, [this](wxCommandEvent &) { archive_customer(); });
    m_add_order_button->Bind(
        wxEVT_BUTTON, [this](wxCommandEvent &) { add_customer_order(); });
    m_edit_order_button->Bind(
        wxEVT_BUTTON, [this](wxCommandEvent &) { edit_customer_order(); });
    m_activate_order_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        set_customer_order_status(CustomerOrderStatus::active);
    });
    m_complete_order_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        set_customer_order_status(CustomerOrderStatus::completed);
    });
    m_cancel_order_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        set_customer_order_status(CustomerOrderStatus::cancelled);
    });
    m_delete_order_button->Bind(
        wxEVT_BUTTON, [this](wxCommandEvent &) { delete_customer_order(); });
    cost_settings_button->Bind(
        wxEVT_BUTTON, [this](wxCommandEvent &) { edit_cost_settings(); });
    refresh_customers_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh(); });
    m_spool_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
        [this](wxDataViewEvent &) { update_button_state(); });
    m_job_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
        [this](wxDataViewEvent &) { update_button_state(); });
    m_customer_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
        [this](wxDataViewEvent &) { update_button_state(); });
    m_order_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
        [this](wxDataViewEvent &) { update_button_state(); });
    m_spool_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
        [this](wxDataViewEvent &) { edit_spool(); });
    m_customer_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
        [this](wxDataViewEvent &) { edit_customer(); });
    m_order_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
        [this](wxDataViewEvent &) { edit_customer_order(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        if (m_store != nullptr && IsShownOnScreen() &&
            wxGetApp().filament_inventory().revision() != m_seen_service_revision)
            refresh();
    }, m_refresh_timer.GetId());
    m_refresh_timer.Start(1'000);

    m_refresh_buttons = {
        refresh_spools_button, refresh_jobs_button, refresh_history_button,
        refresh_job_history_button, refresh_customers_button
    };
    update_button_state();
}

bool FilamentManagerPanel::initialize_store()
{
    if (m_store)
        return true;
    if (m_store_initialization_attempted)
        return false;
    m_store_initialization_attempted = true;
    try {
        m_store = &wxGetApp().filament_inventory().store();
        return true;
    } catch (const std::exception &error) {
        m_store_error = error.what();
        if (!m_store_error_reported) {
            m_store_error_reported = true;
            show_error(error);
        }
        update_button_state();
        return false;
    }
}

bool FilamentManagerPanel::Show(bool show)
{
    const bool result = wxPanel::Show(show);
    if (show)
        refresh();
    return result;
}

void FilamentManagerPanel::refresh()
{
    if (!initialize_store())
        return;
    try {
        refresh_spools();
        refresh_jobs();
        refresh_history();
        refresh_job_history();
        refresh_customers_and_orders();
        update_button_state();
        m_seen_service_revision = wxGetApp().filament_inventory().revision();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::refresh_spools()
{
    std::string selected_spool_id;
    const int selected_row = selected_spool_row();
    if (selected_row >= 0 && selected_row < static_cast<int>(m_spools.size()))
        selected_spool_id = m_spools[static_cast<std::size_t>(selected_row)].id;

    m_spools = m_store->list_spools();
    m_spool_list->DeleteAllItems();
    std::map<std::string, std::pair<bool, bool>> physical_tags;
    for (const SpoolIdentifier &identifier : m_store->list_identifiers()) {
        auto &tags = physical_tags[identifier.spool_id];
        tags.first  = tags.first || identifier.kind == IdentifierKind::nfc_uid;
        tags.second = tags.second || identifier.kind == IdentifierKind::bambu_tag_uid;
    }

    long double available_total_mg = 0.0L;
    long double reserved_total_mg = 0.0L;
    std::size_t low_stock_count = 0;
    int restored_row = wxNOT_FOUND;
    for (std::size_t row = 0; row < m_spools.size(); ++row) {
        const Spool &spool = m_spools[row];
        const auto found = physical_tags.find(spool.id);
        const bool has_nfc = found != physical_tags.end() && found->second.first;
        const bool has_bambu = found != physical_tags.end() && found->second.second;
        wxString identifiers;
        if (has_nfc) identifiers += "NFC";
        if (has_bambu) {
            if (!identifiers.empty())
                identifiers += ", ";
            identifiers += "Bambu";
        }

        wxBitmap spool_bitmap =
            create_scaled_bitmap("filament_green", m_spool_list, 30, false, spool.color_hex);
        wxIcon spool_icon;
        spool_icon.CopyFromBitmap(spool_bitmap);
        wxVariant spool_cell;
        spool_cell << wxDataViewIconText(from_u8(spool.name), spool_icon);

        wxVector<wxVariant> values;
        values.reserve(10);
        values.emplace_back(spool_cell);
        values.emplace_back(from_u8(spool.manufacturer));
        values.emplace_back(from_u8(spool.material_type));
        values.emplace_back(
            format_money(spool.material_price_per_kg_micros, spool.price_currency));
        values.emplace_back(fill_level_percent(spool));
        values.emplace_back(
            format_weight(spool.current_weight_mg) + " / " +
            format_weight(spool.nominal_capacity_mg));
        values.emplace_back(format_weight(spool.reserved_weight_mg));
        values.emplace_back(format_weight(spool.available_weight_mg));
        values.emplace_back(warning_description(spool));
        values.emplace_back(identifiers);
        m_spool_list->AppendItem(values);

        available_total_mg += static_cast<long double>(spool.available_weight_mg);
        reserved_total_mg += static_cast<long double>(spool.reserved_weight_mg);
        if (is_low_stock(spool))
            ++low_stock_count;
        if (!selected_spool_id.empty() && spool.id == selected_spool_id)
            restored_row = static_cast<int>(row);
    }

    if (restored_row != wxNOT_FOUND)
        m_spool_list->SelectRow(static_cast<unsigned int>(restored_row));

    const auto format_total = [](long double milligrams) {
        if (std::abs(milligrams) >= 1'000'000.0L)
            return wxString::Format("%.2f kg", static_cast<double>(milligrams / 1'000'000.0L));
        return wxString::Format("%.1f g", static_cast<double>(milligrams / 1'000.0L));
    };
    m_active_spools_value->SetLabel(wxString::Format("%zu", m_spools.size()));
    m_available_value->SetLabel(format_total(available_total_mg));
    m_reserved_value->SetLabel(format_total(reserved_total_mg));
    m_low_stock_value->SetLabel(wxString::Format("%zu", low_stock_count));
    m_low_stock_value->SetForegroundColour(StateColor::darkModeColorFor(
        wxColour(low_stock_count > 0 ? "#FF6F00" : "#262E30")));
}

void FilamentManagerPanel::refresh_jobs()
{
    m_jobs = m_store->list_open_jobs();
    m_job_list->DeleteAllItems();

    std::map<std::string, CustomerOrder> orders;
    for (const CustomerOrder &order : m_store->list_customer_orders({}, true))
        orders.emplace(order.id, order);
    std::map<std::string, Customer> customers;
    for (const Customer &customer : m_store->list_customers(true))
        customers.emplace(customer.id, customer);

    for (const PrintJob &job : m_jobs) {
        long double total_mg = 0.0L;
        for (const Allocation &allocation : job.allocations)
            total_mg += static_cast<long double>(allocation.estimated_weight_mg);

        wxString order_label = wxString::FromUTF8("\xE2\x80\x94");
        if (job.customer_order_id) {
            const auto order = orders.find(*job.customer_order_id);
            if (order != orders.end()) {
                const auto customer = customers.find(order->second.customer_id);
                order_label = customer != customers.end() ?
                                  from_u8(customer->second.name) +
                                      em_dash_separator() :
                                  wxString {};
                order_label += from_u8(
                    order->second.order_number.empty() ?
                        order->second.title : order->second.order_number);
            }
        }
        const CostSummary costs = m_store->job_cost_summary(job.id);
        append_row(m_job_list, {
            from_u8(job.job_name),
            order_label,
            from_u8(to_string(job.state)),
            from_u8(job.printer_id),
            wxString::Format("%zu", job.allocations.size()),
            wxString::Format("%.1f g", static_cast<double>(total_mg / 1'000.0L)),
            format_duration(job.estimated_runtime_seconds),
            format_money(costs.total_cost_micros, costs.currency),
            from_u8(job.created_at)
        });
    }
}

void FilamentManagerPanel::refresh_history()
{
    m_history_list->DeleteAllItems();
    std::map<std::string, std::string> spool_names;
    for (const Spool &spool : m_store->list_spools(true))
        spool_names.emplace(spool.id, spool.name);

    for (const StockEvent &event : m_store->list_stock_events({}, 1'000)) {
        const auto name = spool_names.find(event.spool_id);
        append_row(m_history_list, {
            from_u8(event.created_at),
            name != spool_names.end() ? from_u8(name->second) : from_u8(event.spool_id),
            from_u8(event.event_type),
            format_weight(event.delta_mg, true),
            format_weight(event.balance_after_mg),
            from_u8(event.note)
        });
    }
}

void FilamentManagerPanel::refresh_job_history()
{
    m_job_history_list->DeleteAllItems();
    std::map<std::string, CustomerOrder> orders;
    for (const CustomerOrder &order : m_store->list_customer_orders({}, true))
        orders.emplace(order.id, order);
    std::map<std::string, Customer> customers;
    for (const Customer &customer : m_store->list_customers(true))
        customers.emplace(customer.id, customer);

    for (const PrintJob &job : m_store->list_jobs(true, 1'000)) {
        Milligrams estimated = 0;
        Milligrams actual = 0;
        bool has_actual = true;
        for (const Allocation &allocation : job.allocations) {
            estimated += allocation.estimated_weight_mg;
            if (allocation.actual_weight_mg)
                actual += *allocation.actual_weight_mg;
            else
                has_actual = false;
        }
        wxString order_label = wxString::FromUTF8("\xE2\x80\x94");
        if (job.customer_order_id) {
            const auto order = orders.find(*job.customer_order_id);
            if (order != orders.end()) {
                const auto customer = customers.find(order->second.customer_id);
                order_label = customer != customers.end() ?
                                  from_u8(customer->second.name) +
                                      em_dash_separator() :
                                  wxString {};
                order_label += from_u8(
                    order->second.order_number.empty() ?
                        order->second.title : order->second.order_number);
            }
        }
        const CostSummary costs = m_store->job_cost_summary(job.id);
        append_row(m_job_history_list, {
            from_u8(job.job_name),
            order_label,
            from_u8(to_string(job.state)),
            from_u8(job.printer_id),
            format_weight(estimated),
            has_actual ? format_weight(actual) : em_dash(),
            format_duration(job.estimated_runtime_seconds),
            format_money(costs.total_cost_micros, costs.currency),
            from_u8(job.created_at),
            job.completed_at.empty() ? em_dash() : from_u8(job.completed_at)
        });
    }
}

void FilamentManagerPanel::refresh_customers_and_orders()
{
    m_customers = m_store->list_customers();
    m_customer_orders = m_store->list_customer_orders({}, true);
    m_customer_list->DeleteAllItems();
    m_order_list->DeleteAllItems();

    std::map<std::string, Customer> customers;
    for (const Customer &customer : m_store->list_customers(true))
        customers.emplace(customer.id, customer);
    for (const Customer &customer : m_customers) {
        const CostSummary costs = m_store->customer_cost_summary(customer.id);
        append_row(m_customer_list, {
            from_u8(customer.name),
            from_u8(customer.contact_name),
            from_u8(customer.email),
            from_u8(customer.phone),
            format_money(costs.total_cost_micros, costs.currency)
        });
    }

    std::map<std::string, std::int64_t> runtime_by_order;
    for (const PrintJob &job : m_store->list_jobs(true, 0)) {
        if (job.state != JobState::discarded && job.customer_order_id)
            runtime_by_order[*job.customer_order_id] += job.estimated_runtime_seconds;
    }

    for (const CustomerOrder &order : m_customer_orders) {
        const auto customer = customers.find(order.customer_id);
        const CostSummary costs = m_store->customer_order_cost_summary(order.id);
        wxString order_label = from_u8(order.order_number);
        if (!order.title.empty()) {
            if (!order_label.empty())
                order_label += em_dash_separator();
            order_label += from_u8(order.title);
        }
        append_row(m_order_list, {
            order_label,
            customer != customers.end() ?
                from_u8(customer->second.name) : from_u8(order.customer_id),
            from_u8(to_string(order.status)),
            format_duration(runtime_by_order[order.id]),
            format_money(costs.material_cost_micros, costs.currency),
            format_money(costs.electricity_cost_micros, costs.currency),
            format_money(costs.total_cost_micros, costs.currency),
            format_optional_money(order.quoted_price_micros, order.currency),
            format_optional_money(order.invoice_amount_micros, order.currency)
        });
    }
}

int FilamentManagerPanel::selected_spool_row() const
{
    return m_spool_list != nullptr ? m_spool_list->GetSelectedRow() : wxNOT_FOUND;
}

int FilamentManagerPanel::selected_job_row() const
{
    return m_job_list != nullptr ? m_job_list->GetSelectedRow() : wxNOT_FOUND;
}

int FilamentManagerPanel::selected_customer_row() const
{
    return m_customer_list != nullptr ? m_customer_list->GetSelectedRow() : wxNOT_FOUND;
}

int FilamentManagerPanel::selected_order_row() const
{
    return m_order_list != nullptr ? m_order_list->GetSelectedRow() : wxNOT_FOUND;
}

void FilamentManagerPanel::update_button_state()
{
    const bool store_ready = m_store != nullptr;
    const bool spool_selected = selected_spool_row() >= 0 &&
                                static_cast<size_t>(selected_spool_row()) < m_spools.size();
    const bool job_selected = selected_job_row() >= 0 &&
                              static_cast<size_t>(selected_job_row()) < m_jobs.size();
    const bool customer_selected =
        selected_customer_row() >= 0 &&
        static_cast<size_t>(selected_customer_row()) < m_customers.size();
    const bool order_selected =
        selected_order_row() >= 0 &&
        static_cast<size_t>(selected_order_row()) < m_customer_orders.size();
    m_add_button->Enable(store_ready);
    for (wxButton *button : m_refresh_buttons)
        button->Enable(store_ready);
    for (wxButton *button : {m_edit_button, m_remaining_button, m_archive_button,
                             m_identifiers_button, m_copy_nfc_button})
        button->Enable(store_ready && spool_selected);

    const JobState selected_state =
        job_selected ? m_jobs[selected_job_row()].state : JobState::completed;
    const bool needs_review = job_selected && selected_state == JobState::needs_review;
    m_confirm_button->Enable(store_ready && needs_review);
    m_correct_button->Enable(store_ready && needs_review);
    m_review_button->Enable(
        store_ready && job_selected &&
        (selected_state == JobState::reserved || selected_state == JobState::printing));
    m_discard_button->Enable(
        store_ready && job_selected &&
        (selected_state == JobState::reserved || selected_state == JobState::needs_review));

    m_edit_customer_button->Enable(store_ready && customer_selected);
    m_archive_customer_button->Enable(store_ready && customer_selected);
    m_add_order_button->Enable(store_ready && !m_customers.empty());
    m_edit_order_button->Enable(store_ready && order_selected);
    m_delete_order_button->Enable(
        store_ready && order_selected &&
        m_customer_orders[selected_order_row()].status == CustomerOrderStatus::draft);
    m_activate_order_button->Enable(
        store_ready && order_selected &&
        m_customer_orders[selected_order_row()].status == CustomerOrderStatus::draft);
    m_complete_order_button->Enable(
        store_ready && order_selected &&
        m_customer_orders[selected_order_row()].status == CustomerOrderStatus::active);
    m_cancel_order_button->Enable(
        store_ready && order_selected &&
        (m_customer_orders[selected_order_row()].status == CustomerOrderStatus::draft ||
         m_customer_orders[selected_order_row()].status == CustomerOrderStatus::active));
}

void FilamentManagerPanel::add_spool()
{
    if (!initialize_store())
        return;
    if (create_filament_spool_interactively(this, *m_store))
        refresh();
}

void FilamentManagerPanel::edit_spool()
{
    if (!initialize_store())
        return;
    const int row = selected_spool_row();
    if (row < 0 || static_cast<size_t>(row) >= m_spools.size())
        return;
    const Spool spool = m_spools[row];
    SpoolEditorDialog dialog(this, &spool);
    while (dialog.ShowModal() == wxID_OK) {
        SpoolInput input;
        std::vector<SpoolIdentifierInput> ignored_identifiers;
        wxString error;
        if (!dialog.read(input, ignored_identifiers, error)) {
            wxMessageBox(error, _L("Filament Manager"), wxOK | wxICON_WARNING, this);
            continue;
        }
        try {
            m_store->update_spool(spool.id, input, make_operation_key("ui-edit", spool.id));
            refresh();
            return;
        } catch (const std::exception &exception) {
            show_error(exception);
        }
    }
}

void FilamentManagerPanel::set_remaining()
{
    if (!initialize_store())
        return;
    const int row = selected_spool_row();
    if (row < 0 || static_cast<size_t>(row) >= m_spools.size())
        return;
    const Spool &spool = m_spools[row];
    wxTextEntryDialog dialog(
        this, _L("Enter the measured remaining filament weight in grams."),
        _L("Set remaining filament"),
        wxString::Format("%.3f", spool.current_weight_mg / 1'000.0));
    if (dialog.ShowModal() != wxID_OK)
        return;

    double grams = 0.0;
    Milligrams milligrams = 0;
    if (!parse_number(dialog.GetValue(), grams) || !grams_to_milligrams(grams, milligrams)) {
        wxMessageBox(_L("Please enter a valid non-negative weight."), _L("Filament Manager"),
                     wxOK | wxICON_WARNING, this);
        return;
    }
    try {
        m_store->set_remaining(spool.id, milligrams, make_operation_key("ui-scale", spool.id),
                               "Manually measured fill level");
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::archive_spool()
{
    if (!initialize_store())
        return;
    const int row = selected_spool_row();
    if (row < 0 || static_cast<size_t>(row) >= m_spools.size())
        return;
    const Spool &spool = m_spools[row];
    if (wxMessageBox(
            _L("Archive spool") + " \"" + from_u8(spool.name) + "\"?",
            _L("Filament Manager"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
        return;
    try {
        m_store->archive_spool(spool.id);
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::manage_identifiers()
{
    if (!initialize_store())
        return;
    const int row = selected_spool_row();
    if (row < 0 || static_cast<size_t>(row) >= m_spools.size())
        return;
    const Spool &spool = m_spools[row];
    try {
        IdentifierEditorDialog dialog(this, spool, m_store->list_identifiers(spool.id));
        if (dialog.ShowModal() != wxID_OK)
            return;
        m_store->replace_physical_identifiers(spool.id, dialog.identifiers());
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::copy_nfc_link()
{
    if (!initialize_store())
        return;
    const int row = selected_spool_row();
    if (row < 0 || static_cast<size_t>(row) >= m_spools.size())
        return;
    const wxString link = "quackslicer://spool/" + from_u8(m_spools[row].id);
    if (!wxTheClipboard->Open()) {
        wxMessageBox(_L("Could not open the clipboard."), _L("Filament Manager"),
                     wxOK | wxICON_WARNING, this);
        return;
    }
    wxTheClipboard->SetData(new wxTextDataObject(link));
    wxTheClipboard->Close();
}

void FilamentManagerPanel::review_job()
{
    if (!initialize_store())
        return;
    const int row = selected_job_row();
    if (row < 0 || static_cast<size_t>(row) >= m_jobs.size())
        return;
    const PrintJob &job = m_jobs[row];
    if (job.state != JobState::reserved && job.state != JobState::printing)
        return;
    if (wxMessageBox(
            _L("Review this job's filament usage manually? Use this when the print finished while QuackSlicer was disconnected or could not detect the printer state."),
            _L("Review print job"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
            this) != wxYES)
        return;
    try {
        m_store->mark_needs_review(job.id);
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::confirm_job(bool correct_amounts)
{
    if (!initialize_store())
        return;
    const int row = selected_job_row();
    if (row < 0 || static_cast<size_t>(row) >= m_jobs.size())
        return;
    const PrintJob &job = m_jobs[row];
    if (job.state != JobState::needs_review)
        return;
    std::vector<ActualConsumption> actual;

    if (correct_amounts) {
        for (const Allocation &allocation : job.allocations) {
            double grams = 0.0;
            Milligrams milligrams = 0;
            while (true) {
                wxTextEntryDialog dialog(
                    this,
                    wxString::Format(_L("Actual consumption for filament %d in grams."),
                                     allocation.filament_index + 1),
                    _L("Correct filament consumption"),
                    wxString::Format("%.3f", allocation.estimated_weight_mg / 1'000.0));
                if (dialog.ShowModal() != wxID_OK)
                    return;
                if (parse_number(dialog.GetValue(), grams) && grams_to_milligrams(grams, milligrams))
                    break;
                wxMessageBox(_L("Please enter a valid non-negative weight."), _L("Filament Manager"),
                             wxOK | wxICON_WARNING, this);
            }
            actual.push_back({allocation.filament_index, milligrams});
        }
    } else if (wxMessageBox(
                   _L("Confirm the sliced filament estimates and deduct them from the assigned spools?"),
                   _L("Confirm print job"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
        return;
    }

    try {
        m_store->commit_job(job.id, actual);
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::discard_job()
{
    if (!initialize_store())
        return;
    const int row = selected_job_row();
    if (row < 0 || static_cast<size_t>(row) >= m_jobs.size())
        return;
    const PrintJob &job = m_jobs[row];
    if (job.state != JobState::reserved && job.state != JobState::needs_review)
        return;
    if (wxMessageBox(
            _L("Discard this pending print calculation without deducting filament?"),
            _L("Discard print job"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
        return;
    try {
        m_store->discard_job(job.id);
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::add_customer()
{
    if (!initialize_store())
        return;
    if (edit_customer_interactively(this, *m_store))
        refresh();
}

void FilamentManagerPanel::edit_customer()
{
    if (!initialize_store())
        return;
    const int row = selected_customer_row();
    if (row < 0 || static_cast<std::size_t>(row) >= m_customers.size())
        return;
    const Customer customer = m_customers[static_cast<std::size_t>(row)];
    if (edit_customer_interactively(this, *m_store, &customer))
        refresh();
}

void FilamentManagerPanel::archive_customer()
{
    if (!initialize_store())
        return;
    const int row = selected_customer_row();
    if (row < 0 || static_cast<std::size_t>(row) >= m_customers.size())
        return;
    const Customer &customer = m_customers[static_cast<std::size_t>(row)];
    if (wxMessageBox(
            wxString::Format(
                _L("Archive customer \"%s\"? Existing orders and costs remain available."),
                from_u8(customer.name)),
            _L("Archive customer"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
            this) != wxYES)
        return;
    try {
        m_store->archive_customer(customer.id);
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::add_customer_order()
{
    if (!initialize_store())
        return;
    std::string preferred_customer_id;
    const int row = selected_customer_row();
    if (row >= 0 && static_cast<std::size_t>(row) < m_customers.size())
        preferred_customer_id = m_customers[static_cast<std::size_t>(row)].id;
    if (edit_customer_order_interactively(
            this, *m_store, nullptr, preferred_customer_id))
        refresh();
}

void FilamentManagerPanel::edit_customer_order()
{
    if (!initialize_store())
        return;
    const int row = selected_order_row();
    if (row < 0 || static_cast<std::size_t>(row) >= m_customer_orders.size())
        return;
    const CustomerOrder order = m_customer_orders[static_cast<std::size_t>(row)];
    if (edit_customer_order_interactively(this, *m_store, &order))
        refresh();
}

void FilamentManagerPanel::set_customer_order_status(CustomerOrderStatus status)
{
    if (!initialize_store())
        return;
    const int row = selected_order_row();
    if (row < 0 || static_cast<std::size_t>(row) >= m_customer_orders.size())
        return;
    const CustomerOrder &order = m_customer_orders[static_cast<std::size_t>(row)];
    if (wxMessageBox(
            wxString::Format(
                _L("Change order \"%s\" to status \"%s\"?"),
                from_u8(order.order_number.empty() ? order.title : order.order_number),
                from_u8(to_string(status))),
            _L("Customer order"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
            this) != wxYES)
        return;
    try {
        m_store->set_customer_order_status(order.id, status);
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::delete_customer_order()
{
    if (!initialize_store())
        return;
    const int row = selected_order_row();
    if (row < 0 || static_cast<std::size_t>(row) >= m_customer_orders.size())
        return;
    const CustomerOrder &order = m_customer_orders[static_cast<std::size_t>(row)];
    if (wxMessageBox(
            wxString::Format(
                _L("Delete draft order \"%s\"?"),
                from_u8(order.order_number.empty() ? order.title : order.order_number)),
            _L("Delete customer order"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
        return;
    try {
        m_store->delete_customer_order(order.id);
        refresh();
    } catch (const std::exception &error) {
        show_error(error);
    }
}

void FilamentManagerPanel::edit_cost_settings()
{
    if (!initialize_store())
        return;
    if (edit_inventory_cost_settings_interactively(this, *m_store))
        refresh();
}

void FilamentManagerPanel::show_error(const std::exception &error)
{
    wxMessageBox(from_u8(error.what()), _L("Filament Manager"), wxOK | wxICON_ERROR, this);
}

} // namespace Slic3r::GUI
