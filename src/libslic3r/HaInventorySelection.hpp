#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
#include "nlohmann/json.hpp"

namespace Slic3r::HaInventorySelection {
using Json=nlohmann::json;
inline const std::vector<std::string> fields={"name","manufacturer","material_type","filament_preset_id",
    "color_hex","nominal_capacity_mg","diameter_mm","density_g_cm3","warning_mode","warning_value",
    "material_price_per_kg_micros","price_currency","status"};
struct Selection {
    std::string uuid;
    std::set<std::string> fields;
    bool stock=false;
};
inline const Json *spool(const Json &bundle,const std::string &uuid) {
    for(const auto &row:bundle.at("tables").at("spools")) if(row.at("id")==uuid) return &row;
    return nullptr;
}
inline std::int64_t balance(const Json &bundle,const std::string &uuid) {
    std::int64_t result=0;
    for(const auto &event:bundle.at("tables").at("stock_events"))
        if(event.at("spool_id")==uuid) result+=event.at("delta_mg").get<std::int64_t>();
    return result;
}
inline void validate(const std::vector<Selection> &selected) {
    if(selected.size()>100) throw std::runtime_error("Select at most 100 rolls");
    std::set<std::string> seen;
    for(const auto &item:selected) {
        if(item.uuid.empty() || !seen.insert(item.uuid).second) throw std::runtime_error("Duplicate or missing roll UUID");
        for(const auto &field:item.fields)
            if(std::find(fields.begin(),fields.end(),field)==fields.end()) throw std::runtime_error("Unknown selected roll field");
    }
}
inline bool open_allocation(const Json &bundle,const std::string &uuid) {
    for(const auto &a:bundle.at("tables").at("allocations")) if(a.at("spool_id")==uuid)
        for(const auto &job:bundle.at("tables").at("print_jobs")) if(job.at("id")==a.at("job_id")) {
            const auto state=job.at("state").get<std::string>();
            if(state=="reserved" || state=="printing" || state=="needs_review") return true;
        }
    return false;
}
inline Json upload_changes(const Json &local,const Json &remote,const std::vector<Selection> &selected) {
    validate(selected); Json changes=Json::array();
    for(const auto &item:selected) {
        const auto *source=spool(local,item.uuid), *target=spool(remote,item.uuid);
        if(!source) throw std::runtime_error("Selected local roll disappeared; refresh comparison");
        Json change={{"spool_uuid",item.uuid},{"fields",Json::object()},{"expected",Json::object()}};
        if(!target) {
            if(item.fields.size()!=fields.size() || !item.stock)
                throw std::runtime_error("A new roll requires explicit selection of all fields and its initial stock");
            change["create"]=true;
        }
        for(const auto &field:item.fields) {
            if(!target || source->at(field)!=target->at(field)) {
                change["fields"][field]=source->at(field);
                if(target) change["expected"][field]=target->at(field);
            }
        }
        if(item.stock && (!target || balance(local,item.uuid)!=balance(remote,item.uuid))) {
            change["remaining_mg"]=balance(local,item.uuid);
            if(target) change["expected_remaining_mg"]=balance(remote,item.uuid);
            // Slicer ledger corrections are estimates; HA scale readings remain measured until an explicit stock transfer.
            change["quality"]="estimated";
        }
        if(!change["fields"].empty() || change.contains("remaining_mg")) changes.push_back(std::move(change));
    }
    return changes;
}
inline Json download_bundle(const Json &local,const Json &remote,const std::vector<Selection> &selected,
    const std::string &key,const std::string &timestamp) {
    validate(selected); Json result=local;
    for(const auto &item:selected) {
        const auto *source=spool(remote,item.uuid), *old=spool(local,item.uuid);
        if(!source) throw std::runtime_error("Selected HA roll disappeared; refresh comparison");
        if(!old && (item.fields.size()!=fields.size() || !item.stock))
            throw std::runtime_error("A new roll requires explicit selection of all fields and its initial stock");
        if(item.fields.count("status") && source->at("status")=="archived" && open_allocation(local,item.uuid))
            throw std::runtime_error("Resolve open local job allocations before archiving this roll");
        if(item.stock && open_allocation(local,item.uuid))
            throw std::runtime_error("Resolve open local job allocations before replacing stock");
        if(!old) {
            result["tables"]["spools"].push_back(*source);
            if(result["tables"].contains("spool_identifiers"))
                result["tables"]["spool_identifiers"].push_back({{"kind","quack_ndef_uuid"},
                    {"value",item.uuid},{"spool_id",item.uuid},{"created_at",timestamp}});
        }
        else for(auto &row:result["tables"]["spools"]) if(row.at("id")==item.uuid)
            for(const auto &field:item.fields) row[field]=source->at(field);
        if(item.stock) {
            const auto before=balance(local,item.uuid), after=balance(remote,item.uuid);
            if(before!=after || !old) {
                const auto event_key=key+":"+item.uuid;
                result["tables"]["stock_events"].push_back({{"id",event_key},{"spool_id",item.uuid},
                    {"job_id",nullptr},{"allocation_id",nullptr},{"event_type",old?"set_remaining":"initial"},
                    {"delta_mg",after-before},{"balance_after_mg",after},{"operation_key",event_key},
                    {"note","Explicit HA stock import; central job history retained in HA"},{"created_at",timestamp}});
            }
        }
        const auto *effective=spool(result,item.uuid);
        const auto remaining=balance(result,item.uuid);
        if(item.stock || item.fields.count("status") || item.fields.count("nominal_capacity_mg")) {
            if(remaining<0 || (effective->contains("nominal_capacity_mg") && remaining>effective->at("nominal_capacity_mg").get<std::int64_t>()) ||
               (effective->at("status")=="empty" && remaining!=0) || (effective->at("status")=="active" && remaining==0))
                throw std::runtime_error("Selected fields contradict stock/status/capacity. Select the corresponding lifecycle fields explicitly.");
        }
    }
    return result;
}
inline Json acknowledge(Json state,const Json &bundle,const std::vector<Selection> &selected) {
    if(!state.is_object()) state=Json::object();
    state["explicit_sync"]=true;
    for(const auto &item:selected) {
        const auto *row=spool(bundle,item.uuid);
        if(!row) continue;
        for(const auto &field:item.fields) state["acknowledged"][item.uuid][field]=row->at(field);
        if(item.stock) state["acknowledged"][item.uuid]["remaining_mg"]=balance(bundle,item.uuid);
    }
    return state;
}
} // namespace Slic3r::HaInventorySelection
