#pragma once

#include <wx/choicdlg.h>
#include <wx/listbox.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include "I18N.hpp"

namespace Slic3r::GUI {
class HaFilamentProfileDialog : public wxSingleChoiceDialog {
public:
    HaFilamentProfileDialog(wxWindow* parent, const wxString& message,
                            const wxString& caption, const wxArrayString& choices)
        : wxSingleChoiceDialog(parent, message, caption, choices), m_choices(choices)
    {
        auto* search = new wxSearchCtrl(this, wxID_ANY, wxEmptyString,
                                       wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        search->SetDescriptiveText(_L("Search profiles"));
        search->ShowSearchButton(true);
        search->ShowCancelButton(true);
        GetSizer()->Insert(1, search, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        m_listbox->SetMinSize(FromDIP(wxSize(620, 220)));
        GetSizer()->SetSizeHints(this);
        CentreOnParent();
        search->Bind(wxEVT_TEXT, [this, search](wxCommandEvent&) {
            const auto selected = m_listbox->GetStringSelection();
            const auto query = search->GetValue().Lower();
            m_listbox->Freeze();
            m_listbox->Clear();
            for (const auto& choice : m_choices)
                if (choice.Lower().Contains(query)) m_listbox->Append(choice);
            const int previous = m_listbox->FindString(selected, true);
            if (m_listbox->GetCount()) m_listbox->SetSelection(previous == wxNOT_FOUND ? 0 : previous);
            m_listbox->Thaw();
            FindWindow(wxID_OK)->Enable(m_listbox->GetCount() != 0);
        });
        search->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, [search](wxCommandEvent&) { search->SetValue(wxEmptyString); });
        search->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& event) {
            if (m_listbox->GetSelection() != wxNOT_FOUND) OnOK(event);
        });
        search->SetFocus();
    }
private:
    wxArrayString m_choices;
};
}
