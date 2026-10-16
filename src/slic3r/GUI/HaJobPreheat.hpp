#pragma once

#include "HaInventoryDemo.hpp"
#include "libslic3r/HaJobPreheat.hpp"
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <chrono>
#include <optional>

namespace Slic3r::GUI::HaJobPreheat {
inline std::int64_t now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::chrono::steady_clock::time_point monotonic_now()
{
    return std::chrono::steady_clock::now();
}

// Capture the endpoint and its credential on the GUI thread. All requests and
// polling then run on PrintJob's worker; no AppConfig/credential access there.
class Client {
public:
    Client(std::string endpoint, std::string token, const std::string& job, const std::string& printer,
           const nlohmann::json& parameters)
        : m_protocol(std::move(endpoint),job,printer,boost::uuids::to_string(boost::uuids::random_generator()()),parameters),
          m_token(std::move(token)) {}
    bool start() { return poll(); }
    bool read() { return poll(); }
    bool skip() { return command("skip"); }
    bool cancel()
    {
        // A lost request reply still leaves a durable server job. GET recovers
        // its version without reactivating it before the explicit cancellation.
        // Cancellation never replays a pending start or retries a failed write.
        m_connected = false;
        try { receive(ha_inventory_request_raw_with_token(m_protocol.url(),m_token)); }
        catch (const HaInventoryRequestError&) { throw; }
        catch (const Slic3r::HaJobPreheat::StatusError&) {}
        if (m_protocol.phase() == "cancelled") return true;
        if (m_protocol.terminal()) return false;
        try { command("cancel"); }
        catch (const HaInventoryRequestError&) { throw; }
        catch (const Slic3r::HaJobPreheat::StatusError&) { if (m_protocol.phase() != "cancelled") throw; }
        return m_protocol.phase() == "cancelled";
    }
    nlohmann::json claim() const
    {
        if (!m_connected || m_disconnected_since)
            throw std::runtime_error("Wait for a fresh HA preheat status before dispatch");
        check_recovery_deadline();
        return m_protocol.claim(now());
    }
    const std::string& phase() const
    {
        static const std::string waiting = "waiting_for_connection";
        return m_disconnected_since ? waiting : m_protocol.phase();
    }
    const std::string& reason() const
    {
        static const std::string retrying = "connection lost; retrying the same preheat job for up to 60 seconds";
        return m_disconnected_since ? retrying : m_protocol.reason();
    }
private:
    void check_recovery_deadline() const
    {
        if ((m_expires > 0 && m_expires <= now()) ||
            (m_disconnected_since && monotonic_now() - *m_disconnected_since >= std::chrono::seconds(60)))
            throw Slic3r::HaJobPreheat::StatusError(
                "HA preheat connection recovery expired. Review the existing job in Home Assistant before retrying");
    }
    bool poll()
    {
        check_recovery_deadline();
        const auto started = monotonic_now();
        m_connected = false;
        try {
            // A lost initial reply may already have started heating. Replay only
            // this exact idempotent request; known jobs need only a status GET.
            const auto payload = m_protocol.request();
            const auto response = m_request_pending ?
                ha_inventory_request_raw_with_token(m_protocol.url("request"),m_token,&payload) :
                ha_inventory_request_raw_with_token(m_protocol.url(),m_token);
            check_recovery_deadline();
            const bool ready = receive(response);
            m_request_pending = false;
            m_disconnected_since.reset();
            return ready;
        } catch (const HaInventoryRequestError& error) {
            if (error.status != 0)
                throw;
            if (!m_disconnected_since)
                m_disconnected_since = started;
            check_recovery_deadline();
            return false;
        }
    }
    bool receive(const nlohmann::json& response)
    {
        m_connected = false;
        const bool ready = m_protocol.accept(response,now());
        const double expires = response.at("expires_at").get<double>();
        // HA may grant one bounded stabilization allowance before the current
        // deadline. Trust only a fresh, identity-checked response, never a
        // transport timeout or an endlessly renewed lease.
        if (m_expiry_limit == 0)
            m_expiry_limit = expires + 900;
        if (expires > m_expiry_limit)
            throw Slic3r::HaJobPreheat::StatusError("HA preheat extension exceeds the fifteen-minute allowance");
        m_expires = expires;
        m_connected = true;
        return ready;
    }
    bool command(const std::string& command)
    {
        const auto payload = m_protocol.command(command);
        m_connected = false;
        return receive(ha_inventory_request_raw_with_token(m_protocol.url(command),m_token,&payload));
    }
    Slic3r::HaJobPreheat::Session m_protocol;
    std::string m_token;
    std::optional<std::chrono::steady_clock::time_point> m_disconnected_since;
    double m_expires = 0;
    double m_expiry_limit = 0;
    bool m_connected = false;
    bool m_request_pending = true;
};
} // namespace Slic3r::GUI::HaJobPreheat
