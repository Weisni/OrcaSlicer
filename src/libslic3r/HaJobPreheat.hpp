#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r::HaJobPreheat {
using Json = nlohmann::json;
class StatusError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
inline bool use_server(bool provider, const Json& snapshot)
{
    const auto capabilities = snapshot.find("capabilities");
    return provider && capabilities != snapshot.end() && capabilities->is_object() &&
        capabilities->value("job_preheat_v1", false);
}

// Pure protocol state: transport and clocks are supplied by the GUI adapter.
class Session {
public:
    Session(std::string source, std::string job, std::string printer, std::string instance, Json parameters)
        : m_source(std::move(source)), m_job(std::move(job)), m_printer(std::move(printer)),
          m_instance(std::move(instance)), m_parameters(std::move(parameters))
    {
        const std::string suffix = "/materials";
        if (m_source.size() < suffix.size() || m_source.compare(m_source.size()-suffix.size(),suffix.size(),suffix) != 0 ||
            m_job.empty() || m_job.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
            m_instance.empty() || m_printer.empty())
            throw std::runtime_error("Invalid HA preheat endpoint or job identity");
        m_base = m_source.substr(0,m_source.size()-suffix.size()) + "/preheat/" + m_job;
    }
    std::string url(const std::string& command = {}) const
    {
        if (!command.empty() && command != "request" && command != "cancel" && command != "skip" && command != "retry" && command != "extend")
            throw std::runtime_error("Invalid HA preheat command");
        return m_base + (command.empty() ? "" : "/" + command);
    }
    Json request() const
    {
        return {{"request_key",m_instance+":"+m_job+":request"},{"instance_id",m_instance},
                {"printer_id",m_printer},{"parameters",m_parameters}};
    }
    Json command(const std::string& name, int seconds = 0) const
    {
        url(name);
        if (name.empty() || name == "request" || m_version < 0)
            throw std::runtime_error("Read the HA preheat job before changing it");
        Json body = {{"request_key",m_instance+":"+m_job+":"+name+":"+std::to_string(m_version)},
                     {"instance_id",m_instance},{"version",m_version}};
        if (name == "extend") body["seconds"] = seconds;
        return body;
    }
    bool accept(const Json& response, std::int64_t now)
    {
        m_ready = false;
        if (response.at("schema_version") != 1 || response.at("job_uuid") != m_job || response.at("printer_id") != m_printer ||
            response.at("instance_id") != m_instance || response.at("parameters") != m_parameters ||
            !response.at("version").is_number_integer() || response.at("version").get<std::int64_t>() < m_version ||
            response.at("version").get<std::int64_t>() < 0 || !response.at("expires_at").is_number() || !std::isfinite(response.at("expires_at").get<double>()) ||
            !response.at("ready").is_boolean() || !response.at("phase").is_string() || !response.at("reason").is_string())
            throw std::runtime_error("HA returned a mismatched or stale preheat job; review it in Home Assistant");
        m_version = response.at("version"); m_expires = response.at("expires_at");
        m_phase = response.at("phase"); m_reason = response.at("reason"); m_ready = response.at("ready");
        if (m_phase != "ready" && m_phase != "selecting_air" && m_phase != "heating" && m_phase != "conditioning_bed" &&
            m_phase != "interrupted" && !terminal())
            throw std::runtime_error("HA returned an unsupported preheat phase; update QuackSlicer and review the job in Home Assistant");
        if (terminal() || m_phase == "interrupted" || m_expires <= now || (m_ready && m_phase != "ready")) {
            m_ready = false;
            throw StatusError("HA preheat cannot continue (" + m_phase + ": " + m_reason + "). Review this job in Home Assistant");
        }
        return m_ready && m_phase == "ready";
    }
    Json claim(std::int64_t now) const
    {
        if (!m_ready || m_phase != "ready" || m_expires <= now)
            throw std::runtime_error("The HA preheat authorization expired; review the job in Home Assistant");
        return {{"instance_id",m_instance},{"version",m_version}};
    }
    bool terminal() const { return m_phase == "cancelled" || m_phase == "expired" || m_phase == "manual_takeover" || m_phase == "handoff" || m_phase == "failed" || m_phase == "needs_review"; }
    const std::string& phase() const { return m_phase; }
    const std::string& reason() const { return m_reason; }
private:
    std::string m_source, m_job, m_printer, m_instance, m_base, m_phase, m_reason;
    Json m_parameters;
    std::int64_t m_version = -1;
    double m_expires = 0;
    bool m_ready = false;
};

// A transport timeout may have consumed the server claim. This state must never
// authorize a second physical attempt or a LAN/cloud fallback.
class DispatchGuard {
public:
    void begin_claim() { if (m_started) throw std::runtime_error("This HA dispatch claim was already attempted"); m_started = true; }
    void confirm_claim() { if (!m_started || m_rejected) throw std::runtime_error("Missing or rejected HA dispatch claim"); m_confirmed = true; }
    void reject_claim() { m_rejected = true; }
    void begin_dispatch() {
        if (!m_confirmed || m_attempted) throw std::runtime_error("HA permits only one physical dispatch attempt");
        m_attempted = true;
    }
    bool uncertain() const { return m_started && !m_confirmed && !m_rejected; }
private:
    bool m_started = false, m_confirmed = false, m_attempted = false, m_rejected = false;
};
} // namespace Slic3r::HaJobPreheat
