#pragma once
#include "HaInventoryAuthority.hpp"
#include "libslic3r/HaInventorySelection.hpp"
#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r::GUI {
class HaInventorySelectionDialog : public wxDialog {
    using Json=HaInventorySelection::Json;
    using Selection=HaInventorySelection::Selection;
    Json local,remote;
    std::string focus;
    wxChoice *direction;
    wxCheckListBox *rolls,*fields;
    wxStaticText *preview;
    std::vector<Selection> choices;
    void rebuild() {
        rolls->Clear();fields->Clear();choices.clear();
        const auto &source=upload?local:remote,&target=upload?remote:local;
        for(const auto &row:source.at("tables").at("spools")) {
            auto id=row.at("id").get<std::string>();if(!focus.empty() && id!=focus) continue;
            auto text=row.at("name").get<std::string>()+" ["+row.at("material_type").get<std::string>()+"] "+id;
            if(!HaInventorySelection::spool(target,id)) text+=" (new)";
            rolls->Append(wxString::FromUTF8(text.c_str()));choices.push_back({id,{},false});
        }
        if(!choices.empty()) {rolls->SetSelection(0);if(!focus.empty()){rolls->Check(0);choices[0].fields.insert("filament_preset_id");}show_fields();}
    }
    void show_fields() {
        fields->Clear();int index=rolls->GetSelection();if(index<0) return;
        const auto &item=choices.at(index);const auto &source=upload?local:remote,&target=upload?remote:local;
        auto *from=HaInventorySelection::spool(source,item.uuid),*to=HaInventorySelection::spool(target,item.uuid);
        for(const auto &field:HaInventorySelection::fields) {
            auto text=field+": "+(to?to->at(field).dump():"<new>")+" -> "+from->at(field).dump();
            fields->Append(wxString::FromUTF8(text.c_str()));fields->Check(fields->GetCount()-1,item.fields.count(field)!=0);
        }
        auto text="remaining_mg: "+(to?std::to_string(HaInventorySelection::balance(target,item.uuid)):"<new>")+" -> "+std::to_string(HaInventorySelection::balance(source,item.uuid));
        fields->Append(wxString::FromUTF8(text.c_str()));fields->Check(fields->GetCount()-1,item.stock);
        fields->Enable(focus.empty());
        if(!focus.empty()) {
            fields->SetSelection(3);
            show_preview();
        } else preview->SetLabel("Select a field to read its complete old/new value.");
    }
    void show_preview() {
        int row=rolls->GetSelection(),field=fields->GetSelection();if(row<0||field<0)return;
        const auto &item=choices.at(row);const auto &source=upload?local:remote,&target=upload?remote:local;
        auto *from=HaInventorySelection::spool(source,item.uuid),*to=HaInventorySelection::spool(target,item.uuid);
        std::string before,after,name;
        if(field==static_cast<int>(HaInventorySelection::fields.size())) {name="remaining_mg";before=to?std::to_string(HaInventorySelection::balance(target,item.uuid)):"<new>";after=std::to_string(HaInventorySelection::balance(source,item.uuid));}
        else {name=HaInventorySelection::fields.at(field);before=to?to->at(name).dump():"<new>";after=from->at(name).dump();}
        preview->SetLabel(wxString::FromUTF8((name+"\nDestination: "+before+"\nSource: "+after).c_str()));preview->Wrap(850);Layout();
    }
public:
    bool upload=true;
    std::vector<Selection> selected;
    HaInventorySelectionDialog(wxWindow *parent,const Json &a,const Json &b,const std::string &profile_uuid)
        :wxDialog(parent,wxID_ANY,profile_uuid.empty()?"Synchronize HA inventory":"Publish material profile association",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER),local(a),remote(b),focus(profile_uuid) {
        auto *layout=new wxBoxSizer(wxVERTICAL);
        layout->Add(new wxStaticText(this,wxID_ANY,"Check rolls and their individual fields. Unchecked data and job/customer history stay unchanged.\nExisting rolls are kept; select status explicitly to archive/restore. No permanent deletion.\nNew rolls require all fields and stock. Quack stock uploads are estimates."),0,wxALL,12);
        direction=new wxChoice(this,wxID_ANY);direction->Append("Quack -> Home Assistant");direction->Append("Home Assistant -> Quack");direction->SetSelection(0);direction->Enable(focus.empty());layout->Add(direction,0,wxEXPAND|wxLEFT|wxRIGHT,12);
        auto *body=new wxBoxSizer(wxHORIZONTAL);
        rolls=new wxCheckListBox(this,wxID_ANY,wxDefaultPosition,wxSize(350,420));fields=new wxCheckListBox(this,wxID_ANY,wxDefaultPosition,wxSize(560,420));
        body->Add(rolls,1,wxEXPAND|wxALL,12);body->Add(fields,2,wxEXPAND|wxALL,12);layout->Add(body,1,wxEXPAND);
        preview=new wxStaticText(this,wxID_ANY,"Select a field to read its complete old/new value.");
        layout->Add(preview,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,12);
        auto *all=new wxButton(this,wxID_ANY,"Select all fields for this roll");all->Enable(focus.empty());layout->Add(all,0,wxLEFT|wxBOTTOM,12);
        layout->Add(CreateStdDialogButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,12);SetSizerAndFit(layout);SetMinSize(wxSize(800,500));
        direction->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){upload=direction->GetSelection()==0;rebuild();});
        rolls->Bind(wxEVT_LISTBOX,[this](wxCommandEvent&){show_fields();});
        fields->Bind(wxEVT_LISTBOX,[this](wxCommandEvent&){show_preview();});
        fields->Bind(wxEVT_CHECKLISTBOX,[this](wxCommandEvent &e){int row=rolls->GetSelection(),field=e.GetInt();if(row<0)return;auto &item=choices.at(row);
            if(field==static_cast<int>(HaInventorySelection::fields.size()))item.stock=fields->IsChecked(field);
            else {auto name=HaInventorySelection::fields.at(field);if(fields->IsChecked(field))item.fields.insert(name);else item.fields.erase(name);}});
        all->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){int row=rolls->GetSelection();if(row<0)return;auto &item=choices.at(row);item.fields={HaInventorySelection::fields.begin(),HaInventorySelection::fields.end()};item.stock=true;rolls->Check(row);show_fields();});
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){selected.clear();for(size_t i=0;i<choices.size();++i)if(rolls->IsChecked(i)&&(!choices[i].fields.empty()||choices[i].stock))selected.push_back(choices[i]);
            if(selected.empty()){wxMessageBox("Select at least one roll and field; Cancel keeps everything unchanged.","Inventory review",wxOK|wxICON_INFORMATION,this);return;}
            try {if(upload)HaInventorySelection::upload_changes(local,remote,selected);else HaInventorySelection::download_bundle(local,remote,selected,"review","2026-10-02T00:00:00Z");}
            catch(const std::exception &e){wxMessageBox(wxString::FromUTF8(e.what()),"Inventory review",wxOK|wxICON_WARNING,this);return;}EndModal(wxID_OK);},wxID_OK);
        rebuild();CentreOnParent();
    }
};
// Only explicit buttons reach this function. The durable journal never includes credentials.
inline bool synchronize_ha_inventory_explicit(wxWindow *parent,FilamentInventory::Store &store,const std::string &profile_uuid={}) {
    using namespace HaInventorySelection;
    if(!ha_inventory_demo_enabled())throw std::runtime_error("Configure the Home Assistant material source first");
    const auto endpoint=wxGetApp().app_config->get("ha_material_demo_endpoint");const std::string suffix="/materials";
    if(!boost::algorithm::ends_with(endpoint,suffix))throw std::runtime_error("Configure the HA /materials endpoint first");
    const auto action=endpoint.substr(0,endpoint.size()-suffix.size())+"/action/native_apply",journal=data_dir()+"/ha-explicit-pending.json";
    auto send=[&](const Json &payload){
        bool accepted=false;
        try {auto request=payload;request["response"]="ack";
            const auto receipt=ha_inventory_request(action,&request);accepted=true;validate_ha_inventory_receipt(receipt,request);
            auto snapshot=ha_inventory_request(endpoint);validate_ha_inventory_snapshot(snapshot);
            if(HaInventoryAuthority::enabled()) HaInventoryAuthority::refresh(snapshot);
            auto state=Json::parse(store.ha_demo_sync_state());if(!state.is_object())state=Json::object();state["explicit_sync"]=true;state["endpoint"]=endpoint;state["revision"]=snapshot.at("revision");
            for(const auto &change:payload.at("changes")){auto id=change.at("spool_uuid").get<std::string>();for(auto field=change.at("fields").begin();field!=change.at("fields").end();++field)state["acknowledged"][id][field.key()]=field.value();if(change.contains("remaining_mg"))state["acknowledged"][id]["remaining_mg"]=change.at("remaining_mg");}
            store.save_ha_demo_sync_state(state.dump());if(!wxRemoveFile(wxString::FromUTF8(journal.c_str())))throw std::runtime_error("HA accepted; journal could not be cleared. Retry the same request safely.");
        }catch(const HaInventoryRequestError &e){if(!accepted&&(e.status==400||e.status==409))wxRemoveFile(wxString::FromUTF8(journal.c_str()));throw;}
    };
    if(wxFileExists(wxString::FromUTF8(journal.c_str()))) {Json saved;std::ifstream in(journal);in>>saved;
        if(saved.at("endpoint")!=endpoint)throw std::runtime_error("Pending request belongs to another HA endpoint; reconnect that source");
        if(wxMessageBox("Replay the previous confirmed transfer? HA prevents duplicate application. No keeps it pending.","Pending transfer",wxYES_NO|wxNO_DEFAULT|wxICON_WARNING,parent)!=wxYES)return false;
        send(saved.at("payload"));return true;}
    auto snapshot=ha_inventory_request(endpoint);validate_ha_inventory_snapshot(snapshot);auto local=Json::parse(store.export_ha_demo_bundle()),remote=snapshot.at("native_bundle");
    HaInventorySelectionDialog review(parent,local,remote,profile_uuid);if(review.ShowModal()!=wxID_OK)return false;
    if(Json::parse(store.export_ha_demo_bundle())!=local)throw std::runtime_error("Local inventory changed during review; reopen synchronization");
    const auto key="quack-explicit:"+std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    if(review.upload){auto changes=upload_changes(local,remote,review.selected);if(changes.empty())return false;
        Json payload={{"revision",snapshot.at("revision")},{"request_key",key},{"confirmed",true},{"concurrency","fields"},{"response","ack"},{"changes",changes}};const auto temporary=journal+".tmp";
        {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out<<Json{{"endpoint",endpoint},{"payload",payload}}.dump();out.close();if(!out)throw std::runtime_error("Cannot save transfer journal; no data sent");}
        if(!wxRenameFile(wxString::FromUTF8(temporary.c_str()),wxString::FromUTF8(journal.c_str()),false))throw std::runtime_error("Cannot persist transfer journal; no data sent");send(payload);
    }else{auto fresh=ha_inventory_request(endpoint);validate_ha_inventory_snapshot(fresh);const auto &current=fresh.at("native_bundle");
        for(const auto &item:review.selected){auto *before=spool(remote,item.uuid),*after=spool(current,item.uuid);if(!after)throw std::runtime_error("HA roll disappeared during review");for(const auto &field:item.fields)if(before->at(field)!=after->at(field))throw std::runtime_error("HA field changed during review; reopen synchronization");if(item.stock&&balance(remote,item.uuid)!=balance(current,item.uuid))throw std::runtime_error("HA stock changed during review; reopen synchronization");}
        if(HaInventoryAuthority::enabled()){HaInventoryAuthority::refresh(fresh);return true;}
        auto merged=download_bundle(local,current,review.selected,key,std::string(wxDateTime::UNow().ToUTC().FormatISOCombined('T').ToUTF8().data())+"Z");
        auto state=acknowledge(Json::parse(store.ha_demo_sync_state()),merged,review.selected);state["endpoint"]=endpoint;state["revision"]=fresh.at("revision");state["_expected_local"]=local;store.import_ha_demo_bundle(merged.dump(),state.dump());}
    return true;
}
} // namespace Slic3r::GUI
