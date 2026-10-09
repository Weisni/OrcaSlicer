#pragma once
#include "nlohmann/json.hpp"
#include <functional>
#include <optional>
#include <atomic>
#include <future>
#include <memory>
namespace Slic3r::HaMaterialRefresh {
class AsyncRefresh {
public:
    struct Result { std::string source, error; nlohmann::json data; };
    using Work = std::function<nlohmann::json(const std::function<bool()>&)>;
    ~AsyncRefresh() { shutdown(); }
    // start/take/invalidate/shutdown are called on the GUI thread. The worker
    // receives only captured inputs and an atomic cancellation predicate.
    bool start(const std::string& source, Work work)
    {
        if (m_stopped || m_future.valid()) return false;
        const auto epoch = m_epoch;
        m_active_epoch = epoch->load();
        m_future = std::async(std::launch::async, [source, work = std::move(work), epoch, current = m_active_epoch] {
            Result result{source, {}, {}};
            const auto active = [epoch,current] { return epoch->load() == current; };
            try { result.data = work(active); }
            catch (const std::exception& error) { result.error = error.what(); }
            catch (...) { result.error = "The HA background refresh failed"; }
            return result;
        });
        return true;
    }
    bool ready() const { return m_future.valid() && m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
    bool pending() const { return m_future.valid(); }
    std::optional<Result> take(const std::string& source)
    {
        if (!ready()) return {};
        auto result = m_future.get();
        if (m_stopped || m_active_epoch != m_epoch->load() || result.source != source) return {};
        return result;
    }
    void invalidate() { ++*m_epoch; }
    void shutdown()
    {
        if (m_stopped) return;
        m_stopped = true;
        invalidate();
        if (m_future.valid()) { m_future.wait(); m_future.get(); }
    }
private:
    std::shared_ptr<std::atomic<std::uint64_t>> m_epoch = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::uint64_t m_active_epoch = 0;
    std::future<Result> m_future;
    bool m_stopped = false;
};
}
