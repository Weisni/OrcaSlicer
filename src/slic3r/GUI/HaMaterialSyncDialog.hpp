#pragma once
#include "HaProjectMaterialDialog.hpp"
#include "HaMaterialProvider.hpp"
#include "libslic3r/HaProjectMaterialDifferences.hpp"
#include "Widgets/Button.hpp"
#include <wx/radiobut.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>

namespace Slic3r::GUI {
// Uses the same slot/color/mapping vocabulary and native controls as AMS sync.
class HaMaterialSyncDialog : public DPIDialog {
public:
    using Rows = std::vector<HaProjectMaterialSync::Row>;
    using Project = HaProjectMaterialSync::Project;
    using Load = std::function<std::optional<Project>(const std::vector<HaProjectMaterialSync::Selection> &, const Project &, const Rows &)>;
    using Save = std::function<bool(std::vector<HaProjectRollPublish::RollPublish>, const Project &, const Rows &)>;
    bool changed() const { return m_changed; }
    HaMaterialSyncDialog(wxWindow *parent, Rows rows, Project project, Load load, Save save)
        : DPIDialog(parent, wxID_ANY, _L("Synchronize filament information"), wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_rows(std::move(rows)), m_project(std::move(project))
    {
        auto *layout = new wxBoxSizer(wxVERTICAL);
        auto *directions = new wxBoxSizer(wxHORIZONTAL);
        auto *download = new wxRadioButton(this, wxID_ANY, _L("Home Assistant to project"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        auto *upload = new wxRadioButton(this, wxID_ANY, _L("Project to Home Assistant"));
        directions->Add(download, 0, wxRIGHT, FromDIP(24)); directions->Add(upload);
        layout->Add(directions, 0, wxALIGN_CENTER | wxALL, FromDIP(20));
        const auto bindings = HaMaterialBinding::read(wxGetApp().preset_bundle->project_config, m_project.presets.size());
        auto *slots = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const auto &a = m_rows[i].assignment;
            if (a.slot.empty()) continue;
            auto *panel = new wxPanel(this);
            auto *column = new wxBoxSizer(wxVERTICAL);
            auto *check = new wxCheckBox(panel, wxID_ANY, from_u8(a.slot));
            check->Enable(!a.spool_uuid.empty());
            column->Add(check, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(8));
            auto *color = new HaMaterialColourPreview(panel);
            color->set_color(a.color);
            column->Add(color, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(8));
            auto *name = new wxStaticText(panel, wxID_ANY, a.spool_uuid.empty() ? _L("Empty") : from_u8(a.product),
                                          wxDefaultPosition, FromDIP(wxSize(110, 44)), wxALIGN_CENTER);
            name->Wrap(FromDIP(110)); column->Add(name, 0, wxALIGN_CENTER);
            column->Add(new wxStaticText(panel, wxID_ANY, a.spool_uuid.empty() ? "" : wxString::Format("%.1f g", m_rows[i].remaining_mg / 1000.0)),
                        0, wxALIGN_CENTER | wxBOTTOM, FromDIP(12));
            auto *mapping = new wxChoice(panel, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(110, -1)));
            mapping->Append(_L("Unmapped"));
            for (size_t p = 0; p < m_project.presets.size(); ++p) mapping->Append(wxString::Format(_L("Filament %d"), int(p + 1)));
            mapping->Append(_L("Add filament")); mapping->SetSelection(0);
            for (size_t p = 0; p < bindings.size(); ++p)
                if (!a.spool_uuid.empty() && bindings[p].spool_uuid == a.spool_uuid && bindings[p].source == HaMaterialProvider::endpoint()) {
                    mapping->SetSelection(int(p + 1)); check->SetValue(true); break;
                }
            mapping->Enable(!a.spool_uuid.empty());
            mapping->SetToolTip(_L("Choose the project filament associated with this physical roll."));
            column->Add(mapping, 0, wxALIGN_CENTER); panel->SetSizer(column);
            slots->Add(panel, 1, wxLEFT | wxRIGHT, FromDIP(7));
            m_checks.push_back(check); m_mappings.push_back(mapping); m_indices.push_back(i);
        }
        layout->Add(slots, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        // Uploads follow physical project bindings, including rolls outside the AMS.
        // Keep their choices separate from the download slot mapping controls.
        m_differences = HaProjectMaterialDifferences::compare(m_project, m_rows, bindings,
            HaMaterialProvider::endpoint(), "duck-poop-demo");
        auto *uploads = new wxPanel(this);
        auto *upload_layout = new wxBoxSizer(wxVERTICAL);
        m_pending_transfer = wxFileExists(from_u8(data_dir() + "/ha-project-material-pending.json"));
        if (m_pending_transfer) {
            auto *pending = new wxStaticText(uploads, wxID_ANY,
                _L("A previous transfer is awaiting completion. Synchronize now first reviews that saved transfer; current selections are not sent with the retry."));
            pending->Wrap(FromDIP(690));
            upload_layout->Add(pending, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
        }
        auto *description = new wxStaticText(uploads, wxID_ANY,
            _L("Changes to Home Assistant are selected below. Uncheck any change you want to keep only in this project."));
        description->Wrap(FromDIP(690));
        upload_layout->Add(description, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
        auto *changes = new wxScrolledWindow(uploads, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(710, 270)), wxVSCROLL);
        changes->SetScrollRate(0, FromDIP(12));
        auto *change_layout = new wxBoxSizer(wxVERTICAL);
        for (const auto &difference : m_differences) {
            const auto &assignment = m_rows.at(difference.source_index).assignment;
            const auto field = difference.field == HaProjectMaterialDifferences::Field::Profile ? _L("Profile") : _L("Color");
            const auto location = assignment.slot.empty() ? _L("Inventory") : from_u8(assignment.slot);
            auto *check = new wxCheckBox(changes, wxID_ANY,
                wxString::Format(_L("Filament %d"), int(difference.project_index + 1)) + " / " + location + " / " +
                from_u8(assignment.product) + " - " + field);
            check->SetValue(difference.problem.empty());
            check->Enable(difference.problem.empty());
            check->SetToolTip(from_u8(assignment.spool_uuid));
            change_layout->Add(check, 0, wxEXPAND | wxTOP, FromDIP(8));
            const auto before = difference.before.empty() ? _L("No linked profile") : from_u8(difference.before);
            auto *values = new wxStaticText(changes, wxID_ANY,
                _L("Home Assistant:") + " " + before + "\n" + _L("Project:") + " " + from_u8(difference.after));
            values->Wrap(FromDIP(650));
            change_layout->Add(values, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, FromDIP(7));
            if (difference.field == HaProjectMaterialDifferences::Field::Profile && !difference.after_profile_sha256.empty()) {
                auto *settings = new wxStaticText(changes, wxID_ANY, difference.before_profile_sha256.empty()
                    ? _L("The complete material settings will be stored in Home Assistant.")
                    : _L("Material settings differ. The complete project profile will replace the HA profile for this roll."));
                settings->Wrap(FromDIP(650));
                change_layout->Add(settings, 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(7));
            }
            if (!difference.problem.empty()) {
                auto *problem = new wxStaticText(changes, wxID_ANY, from_u8(difference.problem));
                problem->Wrap(FromDIP(650));
                change_layout->Add(problem, 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(7));
            }
            m_difference_checks.push_back(check);
        }
        if (m_differences.empty())
            change_layout->Add(new wxStaticText(changes, wxID_ANY,
                _L("No profile or color differences for available HA rolls assigned to this project.")), 0, wxALL, FromDIP(8));
        changes->SetSizer(change_layout);
        changes->FitInside();
        upload_layout->Add(changes, 1, wxEXPAND);
        size_t unavailable = 0;
        for (const auto &binding : bindings) {
            if (binding.spool_uuid.empty() || binding.source != HaMaterialProvider::endpoint() || binding.printer_id != "duck-poop-demo" ||
                std::none_of(m_rows.begin(), m_rows.end(), [&](const auto &row) { return row.assignment.spool_uuid == binding.spool_uuid; }))
                ++unavailable;
        }
        if (unavailable) {
            auto *unbound = new wxStaticText(uploads, wxID_ANY,
                _L("Some project filaments have no available roll in this HA inventory. Select a roll in their filament dropdown first."));
            unbound->Wrap(FromDIP(690));
            upload_layout->Add(unbound, 0, wxEXPAND | wxTOP, FromDIP(12));
        }
        uploads->SetSizer(upload_layout);
        layout->Add(uploads, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
        uploads->Hide();
        auto *options = new wxBoxSizer(wxHORIZONTAL);
        auto *scope = new wxChoice(this, wxID_ANY);
        scope->Append(_L("Profile and color")); scope->Append(_L("Profile only")); scope->Append(_L("Color only")); scope->SetSelection(0);
        options->Add(new wxStaticText(this, wxID_ANY, _L("Synchronize:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        options->Add(scope); layout->Add(options, 0, wxALL, FromDIP(20));
        auto *note = new wxStaticText(this, wxID_ANY,
            _L("Choose slots and project filaments. Local profile overrides stay in the project until you send them to Home Assistant."));
        note->Wrap(FromDIP(680)); layout->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        auto *apply = new Button(this, _L("Synchronize now")); apply->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
        auto *cancel = new Button(this, _L("Cancel")); cancel->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        buttons->AddStretchSpacer(); buttons->Add(apply, 0, wxRIGHT, FromDIP(12)); buttons->Add(cancel);
        layout->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(20));
        const auto update_apply = [this, upload, apply]() {
            apply->Enable(!upload->GetValue() || m_pending_transfer || std::any_of(m_difference_checks.begin(), m_difference_checks.end(),
                [](const auto *check) { return check->IsEnabled() && check->GetValue(); }));
        };
        const auto direction_changed = [this, layout, slots, uploads, options, upload, note, update_apply](wxCommandEvent &) {
            const bool sending = upload->GetValue();
            layout->Show(slots, !sending, true);
            layout->Show(options, !sending, true);
            uploads->Show(sending);
            note->SetLabel(sending ?
                _L("Selected profiles include their current material settings, including project edits. Stock and slot assignments stay unchanged.") :
                _L("Choose slots and project filaments. Local profile overrides stay in the project until you send them to Home Assistant."));
            note->Wrap(FromDIP(690));
            update_apply();
            Layout(); Fit(); CentreOnParent();
        };
        upload->Bind(wxEVT_RADIOBUTTON, direction_changed);
        download->Bind(wxEVT_RADIOBUTTON, direction_changed);
        for (auto *check : m_difference_checks)
            check->Bind(wxEVT_CHECKBOX, [update_apply](wxCommandEvent &) { update_apply(); });
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        apply->Bind(wxEVT_BUTTON, [this, upload, scope, load, save](wxCommandEvent &) {
            try {
                if (upload->GetValue()) {
                    std::vector<bool> checked;
                    for (const auto *check : m_difference_checks) checked.push_back(check->IsEnabled() && check->GetValue());
                    const auto published = m_pending_transfer ? std::vector<HaProjectRollPublish::RollPublish>{} :
                        HaProjectMaterialDifferences::selected_rolls(m_differences, checked, m_rows);
                    if (published.empty() && !m_pending_transfer) throw std::runtime_error("Select at least one profile or color change");
                    m_changed = save(published, m_project, m_rows);
                    if (m_changed) EndModal(wxID_OK);
                    return;
                }
                std::vector<HaProjectMaterialSync::Selection> selected;
                size_t appended = m_project.presets.size();
                std::set<size_t> project_indices;
                for (size_t i = 0; i < m_checks.size(); ++i) {
                    if (!m_checks[i]->GetValue()) continue;
                    const int choice = m_mappings[i]->GetSelection();
                    if (choice <= 0) throw std::runtime_error("Map every selected slot to a project filament");
                    const bool adding = choice > int(m_project.presets.size());
                    const size_t target = adding ? appended++ : size_t(choice - 1);
                    if (!project_indices.insert(target).second) throw std::runtime_error("Map each selected slot to a different project filament");
                    const auto row = m_indices[i];
                    selected.push_back({row, target, scope->GetSelection() == 2, scope->GetSelection() == 1});
                }
                if (selected.empty()) throw std::runtime_error("Select at least one occupied slot");
                m_changed = load(selected, m_project, m_rows).has_value();
                if (m_changed) EndModal(wxID_OK);
            } catch (const std::exception &e) { wxMessageBox(from_u8(e.what()), _L("Synchronize filaments"), wxOK | wxICON_WARNING, this); }
        });
        SetSizerAndFit(layout); SetMinSize(GetSize()); wxGetApp().UpdateDlgDarkUI(this); CentreOnParent();
    }
private:
    void on_dpi_changed(const wxRect &) override
    {
        Layout();
        Fit();
        SetMinSize(GetSize());
    }
    Rows m_rows;
    Project m_project;
    std::vector<wxCheckBox *> m_checks;
    std::vector<wxChoice *> m_mappings;
    std::vector<size_t> m_indices;
    std::vector<HaProjectMaterialDifferences::Difference> m_differences;
    std::vector<wxCheckBox *> m_difference_checks;
    bool m_changed = false;
    bool m_pending_transfer = false;
};
}
