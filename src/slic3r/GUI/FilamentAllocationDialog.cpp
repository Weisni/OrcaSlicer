#include "FilamentAllocationDialog.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include "BitmapComboBox.hpp"
#include "CustomerOrderDialogs.hpp"
#include "FilamentSpoolEditor.hpp"
#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "wxExtensions.hpp"

namespace Slic3r::GUI {

namespace {

using namespace FilamentInventory;

constexpr int ID_CONTINUE_WITHOUT_TRACKING = wxID_HIGHEST + 1;

std::string make_launch_key()
{
    static std::atomic<std::uint64_t> sequence {0};
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "print-launch:" + std::to_string(now) + ":" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

wxString format_weight(Milligrams milligrams)
{
    return wxString::Format("%.1f g", static_cast<double>(milligrams) / 1'000.0);
}

wxString em_dash_separator()
{
    return wxString::FromUTF8(" \xE2\x80\x94 ");
}

std::string trim_copy(const std::string &value)
{
    const auto first = std::find_if_not(
        value.begin(), value.end(),
        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(),
        [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    return first < last ? std::string(first, last) : std::string {};
}

std::string normalized_metadata(std::string value)
{
    value = trim_copy(value);
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool optional_metadata_agrees(
    const std::string &usage_value, const std::string &spool_value)
{
    const std::string normalized_usage = normalized_metadata(usage_value);
    const std::string normalized_spool = normalized_metadata(spool_value);
    return normalized_usage.empty() || normalized_spool.empty() ||
           normalized_usage == normalized_spool;
}

bool spool_is_compatible(
    const FilamentInventoryUsage &usage, const Spool &spool)
{
    if (spool.id.empty() || spool.status == SpoolStatus::archived)
        return false;

    const std::string usage_material = normalized_metadata(usage.material_type);
    if (usage_material.empty() ||
        usage_material != normalized_metadata(spool.material_type))
        return false;

    if (!optional_metadata_agrees(usage.manufacturer, spool.manufacturer))
        return false;

    const std::string usage_preset = trim_copy(usage.filament_preset_id);
    const std::string spool_preset = trim_copy(spool.filament_preset_id);
    if (!usage_preset.empty() && !spool_preset.empty() &&
        usage_preset != spool_preset)
        return false;

    const std::string usage_color = normalized_metadata(usage.color_hex);
    if (!usage_color.empty() &&
        usage_color != normalized_metadata(spool.color_hex))
        return false;

    if (std::isfinite(usage.diameter_mm) && usage.diameter_mm > 0.0) {
        if (!std::isfinite(spool.diameter_mm) || spool.diameter_mm <= 0.0 ||
            std::abs(usage.diameter_mm - spool.diameter_mm) > 0.01)
            return false;
    }
    // Density affects the weight estimate but does not identify which physical
    // spool is loaded; the sliced usage is already expressed in milligrams.
    return true;
}

class FilamentAllocationDialog : public wxDialog
{
public:
    FilamentAllocationDialog(
        wxWindow *parent, Store &store, const FilamentReservationContext &context)
        : wxDialog(parent, wxID_ANY, _L("Assign filament spools"), wxDefaultPosition,
                   wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_store(store)
        , m_context(context)
    {
        auto *root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(
                      this, wxID_ANY,
                      _L("Assign each sliced project filament to the physical spool that will be used.")),
                  0, wxEXPAND | wxALL, FromDIP(12));

        auto *tracking_grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
        tracking_grid->AddGrowableCol(1, 1);
        tracking_grid->Add(
            new wxStaticText(this, wxID_ANY, _L("Customer order")),
            0, wxALIGN_TOP | wxTOP, FromDIP(4));
        m_customer_order = new wxChoice(this, wxID_ANY);
        auto *order_controls = new wxBoxSizer(wxVERTICAL);
        order_controls->Add(m_customer_order, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        auto *order_actions = new wxBoxSizer(wxHORIZONTAL);
        auto *new_customer = new wxButton(
            this, wxID_ANY, _L("New customer..."));
        auto *new_order = new wxButton(this, wxID_ANY, _L("New order..."));
        order_actions->Add(
            new_customer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
            FromDIP(8));
        order_actions->Add(new_order, 0, wxALIGN_CENTER_VERTICAL);
        order_controls->Add(order_actions, 0, wxALIGN_LEFT);
        tracking_grid->Add(order_controls, 1, wxEXPAND);
        new_customer->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            create_customer();
        });
        new_order->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            create_customer_order();
        });
        m_customer_order->Bind(wxEVT_CHOICE, [this](wxCommandEvent &event) {
            m_preferred_customer_id.clear();
            event.Skip();
        });
        refresh_customer_orders();
        tracking_grid->Add(
            new wxStaticText(this, wxID_ANY, _L("Estimated machine runtime")),
            0, wxALIGN_CENTER_VERTICAL);
        tracking_grid->Add(
            new wxStaticText(
                this, wxID_ANY,
                context.estimated_runtime_seconds > 0 ?
                    wxString::Format(
                        "%.1f h",
                        context.estimated_runtime_seconds / 3'600.0) :
                    _L("Not available")),
            0, wxALIGN_CENTER_VERTICAL);
        root->Add(
            tracking_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        auto *scroll = new wxScrolledWindow(
            this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        scroll->SetScrollRate(0, FromDIP(10));
        m_grid = new wxFlexGridSizer(4, FromDIP(8), FromDIP(10));
        m_grid->AddGrowableCol(2, 1);
        for (const wxString &heading : {
                 _L("Project filament"), _L("Required"), _L("Physical spool"), wxString()}) {
            auto *label = new wxStaticText(scroll, wxID_ANY, heading);
            wxFont font = label->GetFont();
            font.SetWeight(wxFONTWEIGHT_BOLD);
            label->SetFont(font);
            m_grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        }

        for (std::size_t index = 0; index < context.usages.size(); ++index) {
            const FilamentInventoryUsage &usage = context.usages[index];
            wxString description = from_u8(
                usage.display_name.empty() ? usage.material_type : usage.display_name);
            if (!usage.suggested_bambu_tag_uid.empty())
                description += "  " + wxString::Format(
                    _L("Bambu RFID: %s"), from_u8(usage.suggested_bambu_tag_uid));

            auto *project_filament = new wxBoxSizer(wxHORIZONTAL);
            const wxColour filament_color(from_u8(usage.color_hex));
            const std::string swatch_color =
                filament_color.IsOk() ? usage.color_hex : "#636363";
            const int swatch_size = FromDIP(18);
            const wxBitmap *color_swatch = get_extruder_color_icon(
                std::vector<std::string> {swatch_color}, false, "",
                swatch_size, swatch_size);
            if (color_swatch != nullptr)
                project_filament->Add(
                    new wxStaticBitmap(scroll, wxID_ANY, *color_swatch),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
            project_filament->Add(
                new wxStaticText(scroll, wxID_ANY, description),
                0, wxALIGN_CENTER_VERTICAL);
            m_grid->Add(project_filament, 0, wxALIGN_CENTER_VERTICAL);
            m_grid->Add(new wxStaticText(
                            scroll, wxID_ANY, format_weight(usage.estimated_weight_mg)),
                        0, wxALIGN_CENTER_VERTICAL);

            auto *choice = new BitmapComboBox(
                scroll, wxID_ANY, wxEmptyString, wxDefaultPosition,
                wxSize(FromDIP(330), -1), 0, nullptr, wxCB_READONLY);
            m_choices.push_back(choice);
            m_grid->Add(choice, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

            auto *create = new wxButton(scroll, wxID_ANY, _L("Create spool"));
            create->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent &) {
                create_spool(index);
            });
            m_grid->Add(create, 0, wxALIGN_CENTER_VERTICAL);
        }
        scroll->SetSizer(m_grid);
        root->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        auto *button_row = new wxBoxSizer(wxHORIZONTAL);
        auto *without_tracking = new wxButton(
            this, ID_CONTINUE_WITHOUT_TRACKING, _L("Continue without tracking"));
        without_tracking->SetToolTip(
            _L("Start the print without reserving or deducting filament."));
        without_tracking->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            EndModal(ID_CONTINUE_WITHOUT_TRACKING);
        });
        button_row->Add(without_tracking, 0, wxALIGN_CENTER_VERTICAL);
        button_row->AddStretchSpacer();

        auto *standard_buttons = new wxStdDialogButtonSizer();
        standard_buttons->AddButton(new wxButton(this, wxID_OK));
        standard_buttons->AddButton(new wxButton(this, wxID_CANCEL));
        standard_buttons->Realize();
        button_row->Add(standard_buttons, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(button_row, 0, wxEXPAND | wxALL, FromDIP(12));
        SetSizer(root);
        SetSize(wxSize(FromDIP(850), FromDIP(500)));
        SetMinSize(wxSize(FromDIP(650), FromDIP(330)));
        CentreOnParent();

        refresh_spools();
    }

    bool allocations(std::vector<AllocationInput> &result, wxString &error) const
    {
        result.clear();
        std::map<std::string, Milligrams> required_by_spool;
        for (std::size_t index = 0; index < m_context.usages.size(); ++index) {
            const int selection = m_choices[index]->GetSelection();
            if (selection <= 0 || static_cast<std::size_t>(selection) > m_spools.size()) {
                error = _L("Please assign a physical spool to every project filament.");
                return false;
            }
            const Spool &spool = m_spools[static_cast<std::size_t>(selection - 1)];
            const FilamentInventoryUsage &usage = m_context.usages[index];
            Milligrams &required = required_by_spool[spool.id];
            if (usage.estimated_weight_mg >
                std::numeric_limits<Milligrams>::max() - required) {
                error = _L("The combined filament requirement is too large.");
                return false;
            }
            required += usage.estimated_weight_mg;
            result.push_back({spool.id, usage.filament_index, usage.estimated_weight_mg});
        }

        for (const auto &[spool_id, required] : required_by_spool) {
            const auto found = std::find_if(
                m_spools.begin(), m_spools.end(),
                [&spool_id](const Spool &spool) { return spool.id == spool_id; });
            if (found != m_spools.end() && found->available_weight_mg < required) {
                error = wxString::Format(
                    _L("Spool \"%s\" has %s available, but the assigned filaments require %s."),
                    from_u8(found->name), format_weight(found->available_weight_mg),
                    format_weight(required));
                return false;
            }
        }
        return true;
    }

    wxString low_stock_warning(const std::vector<AllocationInput> &allocations) const
    {
        std::map<std::string, Milligrams> required_by_spool;
        for (const AllocationInput &allocation : allocations)
            required_by_spool[allocation.spool_id] += allocation.estimated_weight_mg;

        wxString warning;
        for (const Spool &spool : m_spools) {
            const auto required = required_by_spool.find(spool.id);
            if (required == required_by_spool.end() ||
                spool.warning_mode == WarningMode::none)
                continue;

            Milligrams threshold = spool.warning_value;
            wxString threshold_text = format_weight(threshold);
            if (spool.warning_mode == WarningMode::percent) {
                const long double value =
                    static_cast<long double>(spool.nominal_capacity_mg) *
                    static_cast<long double>(spool.warning_value) / 10'000.0L;
                threshold = static_cast<Milligrams>(std::llround(value));
                threshold_text = wxString::Format(
                    "%.1f%%", static_cast<double>(spool.warning_value) / 100.0);
            }

            const Milligrams projected_available =
                spool.available_weight_mg - required->second;
            if (projected_available > threshold)
                continue;

            if (!warning.empty())
                warning += "\n";
            warning += wxString::Format(
                _L("Spool \"%s\" will have %s available after this reservation "
                   "(warning threshold: %s)."),
                from_u8(spool.name), format_weight(projected_available), threshold_text);
        }
        return warning;
    }

    std::optional<std::string> selected_customer_order_id() const
    {
        const int selection = m_customer_order != nullptr ?
                                  m_customer_order->GetSelection() : 0;
        if (selection <= 0 ||
            static_cast<std::size_t>(selection) > m_orders.size())
            return std::nullopt;
        return m_orders[static_cast<std::size_t>(selection - 1)].id;
    }

private:
    void refresh_customer_orders(const std::string &preferred_order_id = {})
    {
        std::string selected_order_id = preferred_order_id;
        if (selected_order_id.empty() && m_customer_order != nullptr) {
            const int selection = m_customer_order->GetSelection();
            if (selection > 0 &&
                static_cast<std::size_t>(selection) <= m_orders.size())
                selected_order_id =
                    m_orders[static_cast<std::size_t>(selection - 1)].id;
        }

        m_orders = m_store.list_customer_orders({}, false);
        std::map<std::string, std::string> customer_names;
        for (const Customer &customer : m_store.list_customers(true))
            customer_names.emplace(customer.id, customer.name);

        m_customer_order->Clear();
        m_customer_order->Append(
            _L("No customer order (personal or family print)"));
        int selected_row = 0;
        for (std::size_t index = 0; index < m_orders.size(); ++index) {
            const CustomerOrder &order = m_orders[index];
            const auto customer = customer_names.find(order.customer_id);
            wxString label = customer != customer_names.end() ?
                                 from_u8(customer->second) +
                                     em_dash_separator() :
                                 wxString {};
            label += from_u8(
                order.order_number.empty() ? order.title : order.order_number);
            if (!order.title.empty() && !order.order_number.empty())
                label += em_dash_separator() + from_u8(order.title);
            m_customer_order->Append(label);
            if (order.id == selected_order_id)
                selected_row = static_cast<int>(index) + 1;
        }
        m_customer_order->SetSelection(selected_row);
        Layout();
    }

    void create_customer()
    {
        const auto customer =
            edit_customer_interactively(this, m_store);
        if (!customer)
            return;
        m_preferred_customer_id = customer->id;
        refresh_customer_orders();
    }

    void create_customer_order()
    {
        if (m_store.list_customers().empty()) {
            wxMessageBox(
                _L("Create the customer first, then add the order for this print."),
                _L("Customer order"), wxOK | wxICON_INFORMATION, this);
            return;
        }

        std::string preferred_customer_id = m_preferred_customer_id;
        const int selection = m_customer_order->GetSelection();
        if (preferred_customer_id.empty() && selection > 0 &&
            static_cast<std::size_t>(selection) <= m_orders.size())
            preferred_customer_id =
                m_orders[static_cast<std::size_t>(selection - 1)].customer_id;
        const auto order = edit_customer_order_interactively(
            this, m_store, nullptr, preferred_customer_id);
        if (order) {
            m_preferred_customer_id.clear();
            refresh_customer_orders(order->id);
        }
    }

    int best_selection(const FilamentInventoryUsage &usage) const
    {
        if (!usage.suggested_bambu_tag_uid.empty()) {
            try {
                const auto matched = m_store.find_spool(
                    IdentifierKind::bambu_tag_uid, usage.suggested_bambu_tag_uid);
                if (matched) {
                    const auto found = std::find_if(
                        m_spools.begin(), m_spools.end(),
                        [&matched](const Spool &spool) { return spool.id == matched->id; });
                    if (found != m_spools.end())
                        return static_cast<int>(std::distance(m_spools.begin(), found)) + 1;
                    // A linked but archived spool remains the authoritative
                    // identity. Do not silently charge a different spool.
                    return wxNOT_FOUND;
                }
            } catch (const std::exception &) {
                return wxNOT_FOUND;
            }
            // An unlinked tag does not identify an inventory entry. The
            // conservative metadata matcher below may still find one unique
            // physical spool, making repeated print starts deterministic.
        }

        const auto matched =
            FilamentAllocationDetail::find_unique_compatible_spool_id(
                usage, m_spools);
        if (matched) {
            const auto found = std::find_if(
                m_spools.begin(), m_spools.end(),
                [&matched](const Spool &spool) { return spool.id == *matched; });
            if (found != m_spools.end())
                return static_cast<int>(
                           std::distance(m_spools.begin(), found)) + 1;
        }
        return wxNOT_FOUND;
    }

    void refresh_spools(
        std::optional<std::size_t> preferred_row = std::nullopt,
        const std::string &preferred_spool_id = {})
    {
        std::vector<std::string> previous(m_choices.size());
        for (std::size_t index = 0; index < m_choices.size(); ++index) {
            const int selection = m_choices[index]->GetSelection();
            if (selection > 0 && static_cast<std::size_t>(selection) <= m_spools.size())
                previous[index] = m_spools[static_cast<std::size_t>(selection - 1)].id;
        }

        m_spools = m_store.list_spools();
        for (std::size_t row = 0; row < m_choices.size(); ++row) {
            BitmapComboBox *choice = m_choices[row];
            choice->Clear();
            const int swatch_size = FromDIP(18);
            choice->Append(
                m_context.usages[row].suggested_bambu_tag_uid.empty() ?
                    _L("Select a spool...") :
                    _L("Bambu RFID is not linked — select or create a spool..."),
                wxNullBitmap);
            for (const Spool &spool : m_spools) {
                const wxColour spool_color(from_u8(spool.color_hex));
                const std::string swatch_color =
                    spool_color.IsOk() ? spool.color_hex : "#636363";
                const wxBitmap *color_swatch = get_extruder_color_icon(
                    std::vector<std::string> {swatch_color}, false, "",
                    swatch_size, swatch_size);
                choice->Append(
                    FilamentAllocationDetail::format_spool_choice_label(spool),
                    color_swatch != nullptr ? *color_swatch : wxNullBitmap);
            }

            std::string desired = previous[row];
            if (preferred_row && *preferred_row == row)
                desired = preferred_spool_id;
            int selection = wxNOT_FOUND;
            if (!desired.empty()) {
                const auto found = std::find_if(
                    m_spools.begin(), m_spools.end(),
                    [&desired](const Spool &spool) { return spool.id == desired; });
                if (found != m_spools.end())
                    selection = static_cast<int>(std::distance(m_spools.begin(), found)) + 1;
            }
            if (selection == wxNOT_FOUND)
                selection = best_selection(m_context.usages[row]);
            choice->SetSelection(selection == wxNOT_FOUND ? 0 : selection);
        }
        Layout();
    }

    void create_spool(std::size_t row)
    {
        const FilamentInventoryUsage &usage = m_context.usages[row];
        SpoolInput defaults;
        defaults.manufacturer = usage.manufacturer;
        defaults.material_type = usage.material_type.empty() ? "Unknown" : usage.material_type;
        defaults.name = usage.display_name.empty() ?
                        defaults.material_type + " " + usage.color_hex : usage.display_name;
        defaults.filament_preset_id = usage.filament_preset_id;
        defaults.color_hex = usage.color_hex.empty() ? "#FFFFFF" : usage.color_hex;
        defaults.diameter_mm =
            std::isfinite(usage.diameter_mm) && usage.diameter_mm > 0.0 ?
                usage.diameter_mm : 1.75;
        defaults.density_g_cm3 =
            std::isfinite(usage.density_g_cm3) && usage.density_g_cm3 > 0.0 ?
                usage.density_g_cm3 : 1.24;
        std::vector<SpoolIdentifierInput> identifiers;
        if (!usage.suggested_bambu_tag_uid.empty())
            identifiers.push_back({
                IdentifierKind::bambu_tag_uid, usage.suggested_bambu_tag_uid
            });
        const auto created = create_filament_spool_interactively(
            this, m_store, &defaults, identifiers.empty() ? nullptr : &identifiers);
        if (created)
            refresh_spools(row, created->id);
    }

    Store                              &m_store;
    const FilamentReservationContext  &m_context;
    wxFlexGridSizer                    *m_grid {nullptr};
    wxChoice                           *m_customer_order {nullptr};
    std::vector<BitmapComboBox *>       m_choices;
    std::vector<Spool>                  m_spools;
    std::vector<CustomerOrder>          m_orders;
    std::string                         m_preferred_customer_id;
};

} // namespace

wxString FilamentAllocationDetail::format_spool_choice_label(
    const FilamentInventory::Spool &spool)
{
    const wxString name         = from_u8(spool.name);
    const wxString material     = from_u8(spool.material_type);
    const wxString weight       = format_weight(spool.available_weight_mg);
    return wxString::Format(
        _L("%s — %s, %s available"),
        name.c_str(), material.c_str(), weight.c_str());
}

std::optional<std::string>
FilamentAllocationDetail::find_unique_compatible_spool_id(
    const FilamentInventoryUsage &usage,
    const std::vector<FilamentInventory::Spool> &spools)
{
    const Spool *match = nullptr;
    for (const Spool &spool : spools) {
        if (!spool_is_compatible(usage, spool))
            continue;
        if (match != nullptr)
            return std::nullopt;
        match = &spool;
    }
    return match != nullptr ? std::optional<std::string>(match->id) :
                              std::nullopt;
}

FilamentReservationResult reserve_filament_for_print(
    wxWindow *parent, Store &store, const FilamentReservationContext &context)
{
    if (context.usages.empty())
        throw Error(ErrorCode::validation, "The sliced plate contains no measurable filament usage");

    if (store.list_spools().empty()) {
        wxMessageBox(
            _L("No physical filament spool exists yet. Create a spool for each material "
               "and enter its current fill level before starting the print."),
            _L("Filament inventory"), wxOK | wxICON_INFORMATION, parent);
    }

    const std::string launch_key = make_launch_key();
    FilamentAllocationDialog dialog(parent, store, context);
    for (;;) {
        const int modal_result = dialog.ShowModal();
        if (modal_result == ID_CONTINUE_WITHOUT_TRACKING)
            return {FilamentReservationDecision::without_tracking, std::nullopt};
        if (modal_result != wxID_OK)
            return {FilamentReservationDecision::cancelled, std::nullopt};

        std::vector<AllocationInput> allocations;
        wxString error;
        if (!dialog.allocations(allocations, error)) {
            wxMessageBox(error, _L("Assign filament spools"), wxOK | wxICON_WARNING, parent);
            continue;
        }
        const wxString warning = dialog.low_stock_warning(allocations);
        if (!warning.empty() &&
            wxMessageBox(
                warning + "\n\n" + _L("Continue with these spool assignments?"),
                _L("Low filament warning"),
                wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, parent) != wxYES)
            continue;
        try {
            PrintJobInput job_input {
                launch_key,
                context.job_name,
                context.project_path,
                context.printer_id
            };
            job_input.customer_order_id = dialog.selected_customer_order_id();
            job_input.estimated_runtime_seconds =
                context.estimated_runtime_seconds;
            return {
                FilamentReservationDecision::reserved,
                store.reserve_job(job_input, allocations)
            };
        } catch (const std::exception &exception) {
            wxMessageBox(
                from_u8(exception.what()), _L("Assign filament spools"),
                wxOK | wxICON_ERROR, parent);
        }
    }
}

} // namespace Slic3r::GUI
