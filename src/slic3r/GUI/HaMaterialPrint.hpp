#pragma once

#include "HaMaterialProvider.hpp"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevFilaSystem.h"
#include "libslic3r/ProjectTask.hpp"
#include <memory>

namespace Slic3r::GUI::HaMaterialPrint {
struct Ticket {
    std::string source, printer_id;
    std::vector<HaMaterialBinding::Binding> bindings;
    std::vector<HaMaterialBinding::Usage> usages;
    std::vector<HaMaterialBinding::Resolved> expected;
    std::map<size_t,std::string> target_colors;
};

// This adapter deliberately describes the paired P2S topology, using the native
// device tray map for actual dispatch indices (AMS HT is 128, never 128 * 4).
inline std::vector<HaMaterialBinding::LiveSlot> live_slots(MachineObject *machine)
{
    if (!machine || !machine->is_connected() || !machine->GetFilaSystem() ||
        std::chrono::system_clock::now() - machine->last_update_time > std::chrono::seconds(60))
        throw std::runtime_error("Refresh the paired printer: current material-slot telemetry is unavailable");
    std::vector<HaMaterialBinding::LiveSlot> slots;
    for (const auto &[tray_index, address] : machine->GetFilaSystem()->GetTrayIndexMap()) {
        const auto ams_id = std::to_string(address.first), slot_id = std::to_string(address.second);
        std::string name;
        DevAmsTray *tray = nullptr;
        if (address.first == 0 && address.second >= 0 && address.second < 4)
            name = "A" + std::to_string(address.second + 1);
        else if (address.first == 128 && address.second == 0) name = "HT1";
        else if (address.first == VIRTUAL_TRAY_MAIN_ID && address.second == 0) name = "EXT";
        else continue;
        if (name == "EXT") {
            for (auto &external : machine->vt_slot) if (external.id == ams_id) { tray = &external; break; }
        } else if (auto *ams = machine->GetFilaSystem()->GetAmsById(ams_id)) tray = ams->GetTray(slot_id);
        if (!tray) continue;
        const bool present = !tray->is_slot_placeholder && tray->is_tray_info_ready() && (name == "EXT" || tray->is_exists);
        slots.push_back({name,ams_id,slot_id,tray_index,present,tray->get_filament_type(),tray->color});
    }
    return slots;
}
inline MachineObject *paired_machine(const std::string &id)
{
    auto *manager = wxGetApp().getDeviceManager();
    auto *machine = manager ? manager->get_my_machine(id) : nullptr;
    if (!machine || machine->get_dev_id() != HaMaterialProvider::physical_device_id())
        throw std::runtime_error("Select the physical printer paired with the HA material source");
    return machine;
}
inline std::vector<HaMaterialBinding::Resolved> check(const Ticket &ticket, MachineObject *machine,
    const std::string &job_uuid = {}, bool refresh = true)
{
    if (!HaMaterialProvider::enabled() || ticket.source != HaMaterialProvider::endpoint() ||
        !machine || ticket.printer_id != machine->get_dev_id())
        throw std::runtime_error("The HA material source or selected printer changed; reopen the print dialog");
    const auto snapshot = refresh ? HaMaterialProvider::fresh_snapshot() : HaMaterialProvider::snapshot();
    return HaMaterialBinding::preflight(ticket.bindings,ticket.usages,snapshot,
        ticket.source,HaMaterialProvider::physical_device_id(),machine->get_dev_id(),live_slots(machine),job_uuid);
}
inline std::shared_ptr<Ticket> prepare(const DynamicPrintConfig &config, size_t count,
    const std::vector<HaMaterialBinding::Usage> &usages, MachineObject *machine, bool refresh = true)
{
    auto ticket = std::make_shared<Ticket>();
    ticket->source = HaMaterialProvider::endpoint(); ticket->printer_id = machine->get_dev_id();
    ticket->bindings = HaMaterialBinding::read(config,count); ticket->usages = usages;
    ticket->expected = check(*ticket,machine,{},refresh);
    const auto devices = live_slots(machine);
    for (const auto &target : ticket->expected) {
        const auto device = std::find_if(devices.begin(),devices.end(),[&](const auto &slot) { return slot.slot == target.slot; });
        if (device == devices.end()) throw std::runtime_error("A printer slot changed during mapping");
        ticket->target_colors.emplace(target.project_index,HaMaterialBinding::rgb(target.color) + "FF");
    }
    return ticket;
}
inline std::shared_ptr<Ticket> preview(const DynamicPrintConfig &config,
    const std::vector<FilamentInfo> &filaments, MachineObject *machine, bool refresh)
{
    size_t count = 0;
    std::vector<HaMaterialBinding::Usage> usages;
    for (const auto &filament : filaments) {
        if (filament.id < 0) throw std::runtime_error("Invalid project material index");
        count = std::max(count,size_t(filament.id)+1);
        usages.push_back({size_t(filament.id),filament.type,0});
    }
    return prepare(config,count,usages,machine,refresh);
}
inline void apply_mapping(const Ticket &ticket, const std::vector<FilamentInfo> &filaments,
    std::vector<FilamentInfo> &mapping)
{
    mapping = filaments;
    for (auto &item : mapping) {
        const auto target = std::find_if(ticket.expected.begin(),ticket.expected.end(),[&](const auto &row) { return row.project_index == size_t(item.id); });
        if (target == ticket.expected.end()) { item.tray_id = -1; continue; }
        item.tray_id = target->tray_id; item.ams_id = target->ams_id; item.slot_id = target->slot_id;
        item.color = ticket.target_colors.at(target->project_index); item.ctype = 0; item.colors.clear();
        item.mapping_result = 0;
    }
}
inline std::vector<HaMaterialBinding::Resolved> verify_mapping(const Ticket &ticket, const std::string &job_uuid)
{
    if (job_uuid.empty()) throw std::runtime_error("The HA print reservation is missing");
    const auto current = check(ticket,paired_machine(ticket.printer_id),job_uuid);
    for (const auto &row : current) {
        const auto expected = std::find_if(ticket.expected.begin(),ticket.expected.end(),[&](const auto &item) { return item.project_index == row.project_index; });
        if (expected == ticket.expected.end() || row.slot != expected->slot || row.revision != expected->revision ||
            row.tray_id != expected->tray_id || row.spool_uuid != expected->spool_uuid)
            throw std::runtime_error("A printer slot changed after material mapping; reopen the print dialog");
    }
    return current;
}
inline void before_dispatch(const Ticket &ticket, const std::string &job_uuid, const std::string &print_name)
{
    nlohmann::json allocations = nlohmann::json::array();
    for (const auto &row : verify_mapping(ticket,job_uuid))
        allocations.push_back({{"filament_index",row.project_index},{"spool_uuid",row.spool_uuid},{"slot",row.slot},{"revision",row.revision}});
    const auto response = HaInventoryAuthority::provider_job("prepare_print",job_uuid,
        {{"printer_id",ticket.printer_id},{"print_name",print_name},{"allocations",allocations}});
    const auto &authorization = response.at("provider_job");
    if (authorization.at("job_uuid") != job_uuid || authorization.at("printer_id") != ticket.printer_id ||
        authorization.at("status") != "prepared")
        throw std::runtime_error("HA did not authorize a new dispatch for this job and printer");
}
inline void dispatch_result(const std::string &job_uuid, const std::string &outcome)
{
    HaInventoryAuthority::provider_job("dispatch_result",job_uuid,{{"outcome",outcome}});
}
inline void upload_check(const std::string &device_id)
{
    if (!HaMaterialProvider::enabled()) return;
    paired_machine(device_id);
    HaMaterialProvider::fresh_snapshot();
}
}
