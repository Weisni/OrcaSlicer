#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "libslic3r/FilamentInventory.hpp"

namespace Slic3r::GUI {

class FilamentInventoryService
{
public:
    struct BambuStatusSnapshot {
        std::string printer_id;
        std::string status;
        std::string job_id;
    };

    FilamentInventoryService();
    ~FilamentInventoryService();

    FilamentInventoryService(const FilamentInventoryService &) = delete;
    FilamentInventoryService &operator=(const FilamentInventoryService &) = delete;

    FilamentInventory::Store &store();
    std::uint64_t revision() const noexcept
    {
        return m_revision.load(std::memory_order_acquire);
    }

    // These methods only enqueue value data. They are safe to call from the
    // printer worker, network callbacks, and the wx main thread.
    void mark_bambu_dispatch_accepted(
        const std::string &inventory_job_id, const std::string &printer_id,
        BambuStatusSnapshot baseline, BambuStatusSnapshot current);
    void bind_bambu_job_id(
        const std::string &inventory_job_id, const std::string &printer_id,
        const std::string &external_job_id);
    void mark_dispatch_failed(const std::string &inventory_job_id, bool ambiguous);
    void observe_bambu_status(BambuStatusSnapshot snapshot);

private:
    using Task = std::function<void(FilamentInventory::Store &)>;

    void enqueue(Task task);
    void worker_loop();

    std::mutex                                  m_store_mutex;
    std::unique_ptr<FilamentInventory::Store>   m_store;

    std::mutex              m_queue_mutex;
    std::condition_variable m_queue_condition;
    std::deque<Task>        m_tasks;
    struct PendingBambuJob {
        std::string         inventory_job_id;
        BambuStatusSnapshot baseline;
        bool                active_seen {false};
        std::string         external_job_id;
    };
    // Accessed only by m_worker. It bridges the short interval between an
    // accepted Bambu dispatch and the first status message carrying its IDs.
    std::map<std::string, PendingBambuJob> m_pending_bambu_jobs;
    std::atomic<std::uint64_t> m_revision {0};
    bool                    m_stopping {false};
    std::thread             m_worker;
};

} // namespace Slic3r::GUI
