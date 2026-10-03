#pragma once

#include <functional>
#include <cmath>
#include <optional>
#include <wx/bmpbuttn.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "libslic3r/HaProjectRollPublish.hpp"
#include <wx/dialog.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include "libslic3r/HaProjectMaterialSync.hpp"
#include "GUI_Utils.hpp"

namespace Slic3r::GUI {

// Paint the actual filament color locally; no generated images or color names
// are needed. A neutral crossed outline represents an unselected HA roll.
class HaMaterialColourPreview : public wxPanel {
public:
    explicit HaMaterialColourPreview(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(50, 50)))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC buffer(this);
            buffer.SetBackground(wxBrush(GetBackgroundColour()));
            buffer.Clear();
            wxGCDC dc(buffer);
            const wxPoint center(GetClientSize().x / 2, GetClientSize().y / 2);
            const int radius = FromDIP(21);
            dc.SetPen(wxPen(wxColour(140, 140, 140), FromDIP(1)));
            dc.SetBrush(m_color.IsOk() ? wxBrush(m_color) : *wxTRANSPARENT_BRUSH);
            dc.DrawCircle(center, radius);
            if (!m_color.IsOk()) {
                const int offset = FromDIP(12);
                dc.DrawLine(center.x - offset, center.y + offset, center.x + offset, center.y - offset);
                return;
            }
            const bool light = int(m_color.Red()) + m_color.Green() + m_color.Blue() > 420;
            dc.SetPen(wxPen(light ? wxColour(95, 95, 95) : wxColour(210, 210, 210), FromDIP(1)));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawCircle(center, FromDIP(17));
            dc.SetBrush(wxBrush(GetBackgroundColour()));
            dc.DrawCircle(center, FromDIP(6));
            const int offset = FromDIP(12), hole = FromDIP(2);
            dc.DrawCircle(wxPoint(center.x - offset, center.y), hole);
            dc.DrawCircle(wxPoint(center.x + offset, center.y), hole);
            dc.DrawCircle(wxPoint(center.x, center.y - offset), hole);
            dc.DrawCircle(wxPoint(center.x, center.y + offset), hole);
        });
    }

    void set_color(const std::string &color)
    {
        m_color = color.empty() ? wxColour() : wxColour(from_u8(color));
        SetToolTip(m_color.IsOk() ? from_u8(color) : _L("No HA roll selected"));
        Refresh();
    }

private:
    wxColour m_color;
};

// Show HA slots independently from project size. Transfers are explicit and reviewed.
class HaProjectMaterialDialog : public wxDialog {
    using Row = HaProjectMaterialSync::Row;
    using Project = HaProjectMaterialSync::Project;
    using Selection = HaProjectMaterialSync::Selection;
public:
    using Action = std::function<std::optional<Project>(const std::vector<Selection> &, const Project &, const std::vector<Row> &)>;
    using Refresh = std::function<std::vector<Row>()>;
    using LocalStock = std::function<std::optional<int64_t>(const std::string &)>;
    using Save = std::function<bool(const std::vector<HaProjectRollPublish::RollPublish> &, const Project &, const std::vector<Row> &)>;

    HaProjectMaterialDialog(wxWindow *parent, std::vector<Row> rows, Project project,
                            Action load, Save save, Refresh refresh, LocalStock local_stock)
        : wxDialog(wxGetTopLevelParent(parent), wxID_ANY, _L("Home Assistant project materials"), wxDefaultPosition,
                   wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_rows(std::move(rows)), m_project(std::move(project)),
          m_load(std::move(load)), m_save(std::move(save)), m_refresh(std::move(refresh)), m_local_stock(std::move(local_stock))
    {
        auto *layout = new wxBoxSizer(wxVERTICAL);
        auto *note = new wxStaticText(this, wxID_ANY, _L("All HA slots are shown below. Map individual project materials, or load every occupied slot. Up arrows send the selected profile, color and explicit stock to an existing or new HA roll. No transfer occurs until you confirm."));
        note->Wrap(FromDIP(930));
        layout->Add(note, 0, wxALL, FromDIP(12));
        auto *connection = new wxBoxSizer(wxHORIZONTAL);
        connection->Add(new wxStaticText(this,wxID_ANY,_L("Home Assistant slots - current server snapshot")),1,wxALIGN_CENTER_VERTICAL);
        auto *refresh_button=new wxButton(this,wxID_ANY,_L("Refresh HA"));
        refresh_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent &){try{m_rows=m_refresh(); update_rows(true); for(size_t i=0;i<m_controls.size();++i)reset_stock(i);m_status->SetLabel(_L("HA refreshed. Select rolls again before sending."));}catch(const std::exception&e){wxMessageBox(from_u8(e.what()),GetTitle(),wxOK|wxICON_ERROR,this);}});
        connection->Add(refresh_button,0);layout->Add(connection,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(12));
        m_slots = new wxPanel(this);
        layout->Add(m_slots, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        auto *all_slots = new wxButton(this, wxID_ANY, _L("Load all occupied HA slots into project"));
        all_slots->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { load_occupied(); });
        layout->Add(all_slots, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        const int list_height = 380;
        auto *scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(1040, list_height)), wxVSCROLL);
        m_scroll = scroll;
        scroll->SetScrollRate(0, FromDIP(20));
        auto *list = new wxBoxSizer(wxVERTICAL);
        m_list = list;
        build_controls();
        scroll->SetSizer(list);
        scroll->FitInside();
        layout->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        m_status = new wxStaticText(this, wxID_ANY, _L("No changes transferred. Select a roll to enable its actions."));
        layout->Add(m_status, 0, wxEXPAND | wxALL, FromDIP(12));
        auto *footer = new wxBoxSizer(wxHORIZONTAL);
        m_load_all = icon_button(this, "ha_material_load_all", _L("Load all project materials from their selected HA rolls"));
        m_save_all = icon_button(this, "ha_material_save_all", _L("Save all project materials to their selected HA rolls"));
        footer->Add(m_load_all, 0, wxRIGHT, FromDIP(8)); footer->Add(m_save_all, 0);
        footer->AddStretchSpacer();
        auto *close = icon_button(this, "topbar_close", _L("Close"));
        footer->Add(close, 0);
        layout->Add(footer, 0, wxEXPAND | wxALL, FromDIP(12));
        m_load_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { perform(false, size_t(-1)); });
        m_save_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { perform(true, size_t(-1)); });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
        SetSizerAndFit(layout);
        update_rows(true);
        layout->Fit(this);
        SetMinSize(wxSize(GetSize().x, FromDIP(360)));
        CentreOnParent();
    }

    bool changed() const { return m_changed; }

private:
    struct Controls {
        HaMaterialColourPreview *swatch;
        wxStaticText *current;
        wxChoice *roll;
        HaMaterialColourPreview *ha_swatch;
        wxStaticText *ha_material;
        wxStaticText *ha;
        wxChoice *mode;
        wxBitmapButton *load, *save;
        wxSpinCtrlDouble *stock;
        wxButton *local_stock;
        wxTextCtrl *new_name, *manufacturer;
        wxSpinCtrlDouble *capacity;
        std::string new_uuid;
    };
    std::vector<Row> m_rows;
    Project m_project;
    Action m_load;
    Save m_save;
    Refresh m_refresh;
    LocalStock m_local_stock;
    std::vector<Controls> m_controls;
    wxBitmapButton *m_load_all{}, *m_save_all{};
    wxStaticText *m_status{};
    wxScrolledWindow *m_scroll{};
    bool m_changed = false;
    wxPanel *m_slots{};
    wxBoxSizer *m_list{};

    wxBitmapButton *icon_button(wxWindow *parent, const std::string &icon, const wxString &name)
    {
        auto *button = new wxBitmapButton(parent, wxID_ANY, create_scaled_bitmap(icon, this, 22),
                                          wxDefaultPosition, FromDIP(wxSize(36, 36)));
        button->SetName(name); button->SetToolTip(name); button->SetHelpText(name);
        return button;
    }

    void build_controls()
    {
        auto *list = m_list;
        for (size_t i = 0; i < m_project.presets.size(); ++i) {
            auto *card = new wxPanel(m_scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
            auto *box = new wxBoxSizer(wxHORIZONTAL);
            auto *swatch = new HaMaterialColourPreview(card);
            box->Add(swatch, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
            auto *project_details = new wxBoxSizer(wxVERTICAL);
            auto *project_label = new wxStaticText(card, wxID_ANY, _L("Project") + " " + from_u8(std::to_string(i + 1)));
            project_label->SetFont(project_label->GetFont().Bold());
            project_details->Add(project_label, 0, wxBOTTOM, FromDIP(6));
            auto *current = new wxStaticText(card, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(215, -1)));
            project_details->Add(current, 0, wxEXPAND);
            box->Add(project_details, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(18));
            auto *mapping = new wxBoxSizer(wxVERTICAL);
            auto *roll = new wxChoice(card, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(360, -1)));
            mapping->Add(roll, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
            auto *ha_details = new wxBoxSizer(wxHORIZONTAL);
            auto *ha_swatch = new HaMaterialColourPreview(card);
            ha_details->Add(ha_swatch, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
            auto *ha_text = new wxBoxSizer(wxVERTICAL);
            auto *ha_material = new wxStaticText(card, wxID_ANY, wxEmptyString);
            ha_material->SetFont(ha_material->GetFont().Bold());
            ha_text->Add(ha_material, 0, wxEXPAND | wxBOTTOM, FromDIP(3));
            auto *ha = new wxStaticText(card, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(300, -1)));
            ha_text->Add(ha, 0, wxEXPAND);
            ha_details->Add(ha_text, 1, wxALIGN_CENTER_VERTICAL);
            mapping->Add(ha_details, 0, wxEXPAND);
            box->Add(mapping, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(12));
            wxArrayString modes;
            modes.Add(_L("Profile and color")); modes.Add(_L("Profile only")); modes.Add(_L("Color only"));
            auto *mode = new wxChoice(card, wxID_ANY, wxDefaultPosition, wxDefaultSize, modes);
            mode->SetSelection(0);
            box->Add(mode, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
            auto *load_button = icon_button(card, "ha_material_load", _L("Load this material from HA"));
            auto *save_button = icon_button(card, "ha_material_save", _L("Save this material to its HA roll"));
            box->Add(load_button, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(4));
            box->Add(save_button, 0, wxALIGN_CENTER_VERTICAL);
            auto *card_layout = new wxBoxSizer(wxVERTICAL);
            card_layout->Add(box, 1, wxEXPAND | wxALL, FromDIP(12));
            card->SetSizer(card_layout);
            list->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
            auto *stock_line = new wxBoxSizer(wxHORIZONTAL);
            stock_line->Add(new wxStaticText(card, wxID_ANY, _L("Stock to send (g)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            auto *stock = new wxSpinCtrlDouble(card, wxID_ANY, "0", wxDefaultPosition, FromDIP(wxSize(110,-1)), wxSP_ARROW_KEYS, 0, 100000, 0, 1);
            stock->SetDigits(3); stock->SetToolTip(_L("Current HA stock is the default. Enter a correction explicitly; Quack sends it as estimated stock."));
            stock_line->Add(stock, 0, wxRIGHT, FromDIP(8));
            auto *local_stock=new wxButton(card,wxID_ANY,_L("Use Quack stock")); stock_line->Add(local_stock,0,wxRIGHT,FromDIP(12));
            local_stock->Bind(wxEVT_BUTTON,[this,i](wxCommandEvent &){auto &c=m_controls.at(i);int selected=c.roll->GetSelection()-1;if(selected>=0 && size_t(selected)<m_rows.size()) if(auto amount=m_local_stock(m_rows[selected].assignment.spool_uuid)) c.stock->SetValue(*amount/1000.0);});
            auto *new_name = new wxTextCtrl(card, wxID_ANY, from_u8(m_project.presets[i]), wxDefaultPosition, FromDIP(wxSize(280,-1)));
            new_name->SetHint(_L("New physical roll name")); stock_line->Add(new_name, 1);
            auto *manufacturer=new wxTextCtrl(card,wxID_ANY,wxEmptyString,wxDefaultPosition,FromDIP(wxSize(150,-1)));
            manufacturer->SetHint(_L("Manufacturer (optional)")); stock_line->Add(manufacturer,0,wxLEFT,FromDIP(8));
            auto *capacity = new wxSpinCtrlDouble(card, wxID_ANY, "1000", wxDefaultPosition, FromDIP(wxSize(100,-1)), wxSP_ARROW_KEYS, 1, 100000, 1000, 100);
            capacity->SetToolTip(_L("New roll nominal capacity (g)")); stock_line->Add(capacity, 0, wxLEFT, FromDIP(8));
            card_layout->Add(stock_line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
            m_controls.push_back({swatch, current, roll, ha_swatch, ha_material, ha, mode, load_button, save_button, stock, local_stock, new_name, manufacturer, capacity});
            roll->Bind(wxEVT_CHOICE, [this,i](wxCommandEvent &) { reset_stock(i); update_rows(false); });
            mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { update_rows(false); });
            load_button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { perform(false, i); });
            save_button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { perform(true, i); });
        }
    }

    void reset_stock(size_t i)
    {
        auto &c=m_controls.at(i); int selected=c.roll->GetSelection()-1;
        c.stock->SetValue(selected>=0 && size_t(selected)<m_rows.size() ? m_rows[selected].remaining_mg/1000.0 : 0.0);
        if(selected>=0 && size_t(selected)<m_rows.size() && m_rows[selected].local_only) {
            const auto &row=m_rows[selected];c.new_name->SetValue(from_u8(row.assignment.product));c.manufacturer->SetValue(from_u8(row.assignment.manufacturer));c.capacity->SetValue(row.nominal_mg/1000.0);
        }
    }

    void render_slots()
    {
        if (m_slots->GetSizer()) m_slots->GetSizer()->Clear(true);
        auto *strip = m_slots->GetSizer();
        if (!strip) {strip=new wxBoxSizer(wxHORIZONTAL); m_slots->SetSizer(strip);}
        for (const auto &row : m_rows) {
            if (row.assignment.slot.empty()) continue;
            auto *box=new wxBoxSizer(wxVERTICAL);
            auto *swatch=new HaMaterialColourPreview(m_slots); swatch->set_color(row.assignment.color);
            box->Add(new wxStaticText(m_slots, wxID_ANY, from_u8(row.assignment.slot)),0,wxALIGN_CENTER_HORIZONTAL);
            box->Add(swatch,0,wxALIGN_CENTER_HORIZONTAL|wxTOP|wxBOTTOM,FromDIP(4));
            auto *label=new wxStaticText(m_slots,wxID_ANY,row.assignment.spool_uuid.empty()?_L("Empty"):from_u8(row.assignment.product));
            label->Wrap(FromDIP(135));box->Add(label,0,wxALIGN_CENTER_HORIZONTAL);
            if (!row.assignment.spool_uuid.empty()) box->Add(new wxStaticText(m_slots,wxID_ANY,wxString::Format("%.1f g",row.remaining_mg/1000.0)),0,wxALIGN_CENTER_HORIZONTAL);
            strip->Add(box,1,wxALL,FromDIP(6));
        }
        m_slots->Layout();
    }

    void load_occupied()
    {
        try {
            const auto selected=HaProjectMaterialSync::all_occupied_selections(m_rows);
            std::vector<std::pair<size_t,std::string>> applied;
            for(const auto &item:selected) applied.emplace_back(item.project_index,m_rows.at(item.source_index).assignment.spool_uuid);
            auto updated=m_load(selected,m_project,m_rows);
            if (!updated) return;
            m_project=*updated; m_changed=true;
            m_list->Clear(true); m_controls.clear(); build_controls();
            m_rows=m_refresh(); update_rows(true);
            for(const auto &item:applied) for(size_t j=0;j<m_rows.size();++j)
                if(m_rows[j].assignment.spool_uuid==item.second) {m_controls.at(item.first).roll->SetSelection(int(j+1)); reset_stock(item.first);}
            update_rows(false);
            m_status->SetLabel(_L("All occupied HA slots loaded. Existing object material indices are retained."));
        } catch(const std::exception &error) {wxMessageBox(from_u8(error.what()),GetTitle(),wxOK|wxICON_ERROR,this);}
    }

    void update_rows(bool rebuild)
    {
        if (rebuild) render_slots();
        bool all_load = !m_controls.empty(), all_save = !m_controls.empty();
        for (size_t i = 0; i < m_controls.size(); ++i) {
            auto &c = m_controls[i];
            c.current->SetLabel(from_u8(m_project.presets[i]));
            c.current->Wrap(FromDIP(215));
            c.swatch->set_color(m_project.colors[i]);
            if (rebuild) {
                c.roll->Clear(); c.roll->Append(_L("Select HA roll / slot"));
                for (const auto &row : m_rows) {
                    const auto manufacturer = from_u8(row.assignment.manufacturer);
                    const auto product = from_u8(row.assignment.product);
                    const auto name = !manufacturer.empty() && !product.Lower().StartsWith(manufacturer.Lower()) ? manufacturer + " " + product : product;
                    c.roll->Append(from_u8(row.assignment.slot.empty()?(row.local_only?"Quack only":"Stock"):row.assignment.slot) + " - " + (row.assignment.spool_uuid.empty()?_L("Empty"):name));
                }
                c.roll->Append(_L("Create a new physical roll in HA"));
                c.roll->SetSelection(0);
            }
            const int index = c.roll->GetSelection() - 1;
            const bool mapped = index >= 0 && size_t(index) < m_rows.size() && !m_rows[index].assignment.spool_uuid.empty();
            const bool creating = index >= 0 && (size_t(index)==m_rows.size() || (mapped && m_rows[index].local_only));
            c.local_stock->Enable(mapped && m_local_stock(m_rows[index].assignment.spool_uuid).has_value());
            c.new_name->Show(creating); c.manufacturer->Show(creating); c.capacity->Show(creating); c.stock->Enable(mapped||creating);
            const bool can_load = mapped && !m_rows[index].local_only && (c.mode->GetSelection() == 2 || !m_rows[index].resolved_preset.empty());
            c.load->Enable(can_load); c.save->Enable(mapped||creating);
            all_load = all_load && can_load; all_save = all_save && (mapped||creating);
            c.ha_swatch->set_color(mapped ? m_rows[index].assignment.color : std::string());
            if (mapped) {
                const auto &row = m_rows[index];
                const wxString source = row.resolved_preset.empty() ? _L("No compatible profile") :
                    row.resolved_preset == row.assignment.material_preset ? _L("Linked material profile") : _L("Automatic standard profile");
                c.ha_material->SetLabel(from_u8(row.assignment.material_type) + "  /  " + source);
                c.ha->SetLabel(from_u8(row.resolved_preset));
                c.ha->SetToolTip(from_u8(row.resolved_preset));
            } else {
                c.ha_material->SetLabel(_L("Home Assistant"));
                c.ha->SetLabel(creating?_L("Enter a roll name, remaining stock and capacity. A new UUID will be created."):_L("Choose a roll to preview its color and profile"));
                c.ha->UnsetToolTip();
            }
            c.ha_material->Wrap(FromDIP(300));
            c.ha->Wrap(FromDIP(300));
            c.roll->SetToolTip(mapped ? from_u8("UUID: " + m_rows[index].assignment.spool_uuid) : _L("Select the physical HA roll"));
        }
        m_load_all->Enable(all_load); m_save_all->Enable(all_save);
        Layout();
        m_scroll->FitInside();
    }

    void perform(bool save, size_t index)
    {
        try {
            std::vector<Selection> selections;
            for (size_t i = 0; i < m_controls.size(); ++i) {
                if (index != size_t(-1) && i != index) continue;
                const int selected = m_controls[i].roll->GetSelection() - 1;
                if (selected < 0) throw std::runtime_error("Select an HA roll for every material in this operation");
                selections.push_back({size_t(selected), i, m_controls[i].mode->GetSelection() == 2,
                                     m_controls[i].mode->GetSelection() == 1});
            }
            if (save) {
                std::vector<HaProjectRollPublish::RollPublish> rolls;
                for(const auto &selection:selections) {
                    auto &c=m_controls.at(selection.project_index);
                    const bool draft=selection.source_index==m_rows.size();
                    const bool create=draft || m_rows.at(selection.source_index).local_only;
                    if(draft && c.new_uuid.empty()) c.new_uuid=boost::uuids::to_string(boost::uuids::random_generator()());
                    HaProjectRollPublish::RollPublish roll;
                    roll.project_index=selection.project_index; roll.create=create;
                    roll.uuid=draft?c.new_uuid:m_rows.at(selection.source_index).assignment.spool_uuid;
                    roll.remaining_mg=std::llround(c.stock->GetValue()*1000.0);
                    roll.name=into_u8(c.new_name->GetValue()); roll.manufacturer=into_u8(c.manufacturer->GetValue()); roll.nominal_mg=std::llround(c.capacity->GetValue()*1000.0);
                    roll.color_only=selection.color_only; roll.profile_only=selection.profile_only;
                    if(create && (roll.color_only||roll.profile_only)) throw std::runtime_error("A new roll requires profile and color");
                    rolls.push_back(std::move(roll));
                }
                if (!m_save(rolls,m_project,m_rows)) return;
                m_status->SetLabel(_L("HA rolls, selected profile associations and stock synchronized."));
            } else {
                const auto updated=m_load(selections,m_project,m_rows);
                if(!updated) return;
                m_project=*updated;
                m_status->SetLabel(_L("Selected project materials loaded from HA."));
            }
            m_changed = true;
            std::vector<std::string> ids;
            const auto old_rows=m_rows;
            std::vector<double> unsent_stock;
            std::vector<bool> creating_drafts;
            for(const auto &c:m_controls) creating_drafts.push_back(c.roll->GetSelection()==int(m_rows.size()+1));
            for(const auto &c:m_controls) unsent_stock.push_back(c.stock->GetValue());
            for (const auto &c : m_controls) {
                const int selected = c.roll->GetSelection() - 1;
                ids.push_back(selected >= 0 && size_t(selected)<m_rows.size() ? m_rows.at(selected).assignment.spool_uuid : c.new_uuid);
            }
            m_rows = m_refresh();
            if(save) for(size_t i=0;i<m_controls.size();++i) if(index==size_t(-1)||index==i) m_controls[i].new_uuid.clear();
            update_rows(true);
            for (size_t i = 0; i < ids.size(); ++i)
                for (size_t j = 0; j < m_rows.size(); ++j)
                    if (!ids[i].empty() && m_rows[j].assignment.spool_uuid == ids[i]) {
                        m_controls[i].roll->SetSelection(int(j + 1));
                        if(index!=size_t(-1)&&index!=i) {
                            const auto old=std::find_if(old_rows.begin(),old_rows.end(),[&](const auto&r){return r.assignment.spool_uuid==ids[i];});
                            if(old!=old_rows.end() && std::llround(unsent_stock[i]*1000.0)!=old->remaining_mg) {
                                m_rows[j]=*old; m_controls[i].stock->SetValue(unsent_stock[i]);
                            } else reset_stock(i);
                        } else reset_stock(i);
                    }
            for(size_t i=0;i<ids.size();++i) if(index!=size_t(-1)&&index!=i && creating_drafts[i] && m_controls[i].roll->GetSelection()==0) {
                m_controls[i].roll->SetSelection(int(m_rows.size()+1)); m_controls[i].stock->SetValue(unsent_stock[i]);
            }
            update_rows(false);
        } catch (const std::exception &error) {
            m_status->SetLabel(from_u8(error.what()));
            m_status->Wrap(FromDIP(900));
            Layout();
            wxMessageBox(from_u8(error.what()), GetTitle(), wxOK | wxICON_ERROR, this);
        }
    }
};

} // namespace Slic3r::GUI
