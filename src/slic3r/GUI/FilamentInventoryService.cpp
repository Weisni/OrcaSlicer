#include "FilamentInventoryService.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Utils.hpp"

namespace Slic3r::GUI {

namespace {

std::string trim_copy(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string();
}

bool usable_external_id(const std::string &value)
{
    const std::string normalized = trim_copy(value);
    return !normalized.empty() &&
           std::any_of(normalized.begin(), normalized.end(), [](unsigned char character) {
               return character != '0' && std::isspace(character) == 0;
           });
}

bool active_bambu_status(const std::string &status)
{
    return status == "RUNNING" || status == "PAUSE" ||
           status == "SLICING" || status == "PREPARE";
}

bool active_evidence(
    const FilamentInventoryService::BambuStatusSnapshot &baseline,
    const FilamentInventoryService::BambuStatusSnapshot &current)
{
    if (!active_bambu_status(current.status))
        return false;
    if (usable_external_id(baseline.job_id))
        return usable_external_id(current.job_id) &&
               trim_copy(baseline.job_id) != trim_copy(current.job_id);
    return !active_bambu_status(baseline.status);
}

bool new_generation_id(
    const FilamentInventoryService::BambuStatusSnapshot &baseline,
    const FilamentInventoryService::BambuStatusSnapshot &current)
{
    return usable_external_id(current.job_id) &&
           (!usable_external_id(baseline.job_id) ||
            trim_copy(baseline.job_id) != trim_copy(current.job_id));
}

std::string bambu_provider(const std::string &printer_id)
{
    return "bambu:" + trim_copy(printer_id);
}

} // namespace

FilamentInventoryService::FilamentInventoryService()
    : m_worker([this] { worker_loop(); })
{}

FilamentInventoryService::~FilamentInventoryService()
{
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_stopping = true;
    }
    m_queue_condition.notify_one();
    if (m_worker.joinable())
        m_worker.join();
}

FilamentInventory::Store &FilamentInventoryService::store()
{
    std::lock_guard<std::mutex> lock(m_store_mutex);
    if (!m_store) {
        const boost::filesystem::path directory =
            boost::filesystem::path(Slic3r::data_dir()) / "filament_inventory";
        boost::filesystem::create_directories(directory);
        const std::string database_path =
            Slic3r::data_dir() + "/filament_inventory/inventory.sqlite3";
        m_store = std::make_unique<FilamentInventory::Store>(database_path);
    }
    return *m_store;
}

void FilamentInventoryService::mark_bambu_dispatch_accepted(
    const std::string &inventory_job_id, const std::string &printer_id,
    BambuStatusSnapshot baseline, BambuStatusSnapshot current)
{
    if (inventory_job_id.empty() || printer_id.empty())
        return;
    enqueue([this, inventory_job_id, printer_id,
             baseline = std::move(baseline),
             current = std::move(current)](FilamentInventory::Store &store) mutable {
        const FilamentInventory::PrintJob job = store.get_job(inventory_job_id);
        if (job.state != FilamentInventory::JobState::reserved &&
            job.state != FilamentInventory::JobState::printing)
            return;
        baseline.printer_id = printer_id;
        current.printer_id = printer_id;

        const auto previous_pending = m_pending_bambu_jobs.find(printer_id);
        if (previous_pending != m_pending_bambu_jobs.end() &&
            previous_pending->second.inventory_job_id != inventory_job_id) {
            const FilamentInventory::PrintJob previous_job =
                store.get_job(previous_pending->second.inventory_job_id);
            if (previous_job.state != FilamentInventory::JobState::completed &&
                previous_job.state != FilamentInventory::JobState::discarded &&
                previous_job.state != FilamentInventory::JobState::needs_review) {
                store.mark_needs_review(previous_job.id);
                m_revision.fetch_add(1, std::memory_order_release);
            }
            m_pending_bambu_jobs.erase(previous_pending);
        }

        PendingBambuJob pending {
            inventory_job_id, std::move(baseline), false, {}
        };

        const std::string provider = bambu_provider(printer_id);
        const auto exact = usable_external_id(current.job_id) ?
            store.find_job(provider, "job_id", trim_copy(current.job_id)) :
            std::nullopt;
        const bool identity_available =
            !exact || exact->id == inventory_job_id;
        const bool correlated_current_id =
            (exact && exact->id == inventory_job_id) ||
            (identity_available && new_generation_id(pending.baseline, current));
        if (correlated_current_id) {
            pending.external_job_id = trim_copy(current.job_id);
            store.bind_job_identifier(
                inventory_job_id, provider, "job_id", pending.external_job_id);
        }

        const bool terminal_current =
            current.status == "FINISH" || current.status == "FAILED";
        if (terminal_current && correlated_current_id) {
            if (current.status == "FINISH")
                store.commit_job(inventory_job_id);
            else
                store.mark_needs_review(inventory_job_id);
            m_revision.fetch_add(1, std::memory_order_release);
            return;
        }
        if ((current.status == "FINISH" || current.status == "FAILED") &&
            trim_copy(pending.baseline.status) != trim_copy(current.status)) {
            store.mark_needs_review(inventory_job_id);
            m_revision.fetch_add(1, std::memory_order_release);
            return;
        }

        if ((identity_available && active_evidence(pending.baseline, current)) ||
            (exact && exact->id == inventory_job_id &&
             active_bambu_status(current.status))) {
            if (usable_external_id(current.job_id) &&
                pending.external_job_id.empty()) {
                pending.external_job_id = trim_copy(current.job_id);
                store.bind_job_identifier(
                    inventory_job_id, provider, "job_id", pending.external_job_id);
            }
            if (job.state == FilamentInventory::JobState::reserved) {
                store.mark_printing(inventory_job_id);
                m_revision.fetch_add(1, std::memory_order_release);
            }
            pending.active_seen = true;
        }
        m_pending_bambu_jobs[printer_id] = std::move(pending);
    });
}

void FilamentInventoryService::bind_bambu_job_id(
    const std::string &inventory_job_id, const std::string &printer_id,
    const std::string &external_job_id)
{
    if (inventory_job_id.empty() || printer_id.empty() || !usable_external_id(external_job_id))
        return;
    enqueue([this, inventory_job_id, printer_id, provider = bambu_provider(printer_id),
             external_job_id = trim_copy(external_job_id)](FilamentInventory::Store &store) {
        store.bind_job_identifier(inventory_job_id, provider, "job_id", external_job_id);
    });
}

void FilamentInventoryService::mark_dispatch_failed(
    const std::string &inventory_job_id, bool ambiguous)
{
    if (inventory_job_id.empty())
        return;
    enqueue([this, inventory_job_id, ambiguous](FilamentInventory::Store &store) {
        const FilamentInventory::PrintJob job = store.get_job(inventory_job_id);
        if (job.state != FilamentInventory::JobState::completed &&
            job.state != FilamentInventory::JobState::discarded) {
            if (ambiguous || job.state != FilamentInventory::JobState::reserved)
                store.mark_needs_review(inventory_job_id);
            else
                store.discard_job(inventory_job_id);
            m_revision.fetch_add(1, std::memory_order_release);
        }
        for (auto iterator = m_pending_bambu_jobs.begin();
             iterator != m_pending_bambu_jobs.end();) {
            if (iterator->second.inventory_job_id == inventory_job_id)
                iterator = m_pending_bambu_jobs.erase(iterator);
            else
                ++iterator;
        }
    });
}

void FilamentInventoryService::observe_bambu_status(BambuStatusSnapshot snapshot)
{
    if (snapshot.printer_id.empty())
        return;
    enqueue([this, snapshot = std::move(snapshot)](FilamentInventory::Store &store) {
        const std::string provider = bambu_provider(snapshot.printer_id);
        std::optional<FilamentInventory::PrintJob> job;
        if (usable_external_id(snapshot.job_id))
            job = store.find_job(provider, "job_id", trim_copy(snapshot.job_id));
        const bool resolved_by_exact_id = job.has_value();

        bool matched_pending = false;
        auto pending = m_pending_bambu_jobs.find(snapshot.printer_id);
        const auto pending_has_different_generation = [&] {
            return pending != m_pending_bambu_jobs.end() &&
                   pending->second.active_seen &&
                   usable_external_id(pending->second.external_job_id) &&
                   usable_external_id(snapshot.job_id) &&
                   trim_copy(pending->second.external_job_id) !=
                       trim_copy(snapshot.job_id);
        };
        if (pending_has_different_generation() &&
            (!job || job->id == pending->second.inventory_job_id)) {
            const FilamentInventory::PrintJob pending_job =
                store.get_job(pending->second.inventory_job_id);
            if (pending_job.state != FilamentInventory::JobState::completed &&
                pending_job.state != FilamentInventory::JobState::discarded &&
                pending_job.state != FilamentInventory::JobState::needs_review) {
                store.mark_needs_review(pending_job.id);
                m_revision.fetch_add(1, std::memory_order_release);
            }
            m_pending_bambu_jobs.erase(pending);
            return;
        }

        if (!job) {
            if (pending != m_pending_bambu_jobs.end()) {
                if (active_evidence(pending->second.baseline, snapshot)) {
                    pending->second.active_seen = true;
                    matched_pending = true;
                } else if (pending->second.active_seen &&
                           (snapshot.status == "FINISH" ||
                            snapshot.status == "FAILED" ||
                            snapshot.status == "IDLE")) {
                    matched_pending = true;
                }
                if (matched_pending)
                    job = store.get_job(pending->second.inventory_job_id);
            }
        }
        if (!job)
            return;

        pending = m_pending_bambu_jobs.find(snapshot.printer_id);
        if (pending != m_pending_bambu_jobs.end() &&
            pending->second.inventory_job_id == job->id &&
            usable_external_id(snapshot.job_id) &&
            pending->second.external_job_id.empty())
            pending->second.external_job_id = trim_copy(snapshot.job_id);

        if (usable_external_id(snapshot.job_id))
            store.bind_job_identifier(
                job->id, provider, "job_id", trim_copy(snapshot.job_id));

        const auto clear_matching_pending = [&] {
            const auto current_pending = m_pending_bambu_jobs.find(snapshot.printer_id);
            if (current_pending != m_pending_bambu_jobs.end() &&
                current_pending->second.inventory_job_id == job->id)
                m_pending_bambu_jobs.erase(current_pending);
        };

        if (job->state == FilamentInventory::JobState::completed ||
            job->state == FilamentInventory::JobState::discarded ||
            job->state == FilamentInventory::JobState::needs_review) {
            clear_matching_pending();
        } else if (active_bambu_status(snapshot.status)) {
            store.mark_printing(job->id);
            if (job->state == FilamentInventory::JobState::reserved)
                m_revision.fetch_add(1, std::memory_order_release);
            if (pending != m_pending_bambu_jobs.end() &&
                pending->second.inventory_job_id == job->id)
                pending->second.active_seen = true;
        } else if (snapshot.status == "FINISH") {
            const bool safe_terminal =
                resolved_by_exact_id ||
                (pending != m_pending_bambu_jobs.end() &&
                 pending->second.inventory_job_id == job->id &&
                 pending->second.active_seen &&
                 usable_external_id(pending->second.external_job_id));
            if (safe_terminal)
                store.commit_job(job->id);
            else
                store.mark_needs_review(job->id);
            m_revision.fetch_add(1, std::memory_order_release);
            clear_matching_pending();
        } else if (snapshot.status == "FAILED") {
            store.mark_needs_review(job->id);
            m_revision.fetch_add(1, std::memory_order_release);
            clear_matching_pending();
        } else if (snapshot.status == "IDLE" &&
                   job->state == FilamentInventory::JobState::printing) {
            store.mark_needs_review(job->id);
            m_revision.fetch_add(1, std::memory_order_release);
            clear_matching_pending();
        }
    });
}

void FilamentInventoryService::enqueue(Task task)
{
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_stopping)
            return;
        m_tasks.emplace_back(std::move(task));
    }
    m_queue_condition.notify_one();
}

void FilamentInventoryService::worker_loop()
{
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_condition.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
            if (m_stopping && m_tasks.empty())
                return;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }

        try {
            task(store());
        } catch (const std::exception &error) {
            BOOST_LOG_TRIVIAL(error)
                << "Filament inventory background operation failed: " << error.what();
        }
    }
}

} // namespace Slic3r::GUI
