#include <catch2/catch_all.hpp>

#include <memory>

#include <boost/filesystem.hpp>

#include "libslic3r/FilamentInventory.hpp"
#include "slic3r/GUI/FilamentInventoryService.hpp"

namespace fs = boost::filesystem;

using namespace Slic3r::FilamentInventory;
using Slic3r::GUI::FilamentInventoryService;

namespace {

struct TemporaryInventoryService
{
    TemporaryInventoryService()
        : path(fs::temp_directory_path() /
               fs::unique_path("quackslicer-filament-service-%%%%-%%%%.sqlite3"))
        , service(std::make_unique<FilamentInventoryService>(path.string()))
    {}

    ~TemporaryInventoryService()
    {
        service.reset();
        boost::system::error_code error;
        fs::remove(path, error);
        fs::remove(path.string() + "-wal", error);
        fs::remove(path.string() + "-shm", error);
    }

    fs::path                                  path;
    std::unique_ptr<FilamentInventoryService> service;
};

SpoolInput service_spool_input(const std::string &name)
{
    SpoolInput input;
    input.manufacturer        = "Quack Materials";
    input.material_type       = "PLA";
    input.name                = name;
    input.color_hex           = "#12ABEF";
    input.nominal_capacity_mg = 500'000;
    input.initial_weight_mg   = 500'000;
    return input;
}

PrintJobInput service_job_input(const std::string &key)
{
    return {key, "Tracked print", "tracked.3mf", "printer-1"};
}

FilamentInventoryService::BambuStatusSnapshot status(
    const std::string &value, const std::string &job_id = {})
{
    return {"printer-1", value, job_id};
}

} // namespace

TEST_CASE("Bambu completion commits an observed print without an external job id",
          "[FilamentInventory][BambuLifecycle]")
{
    TemporaryInventoryService inventory;
    Store &store = inventory.service->store();
    const Spool spool = store.create_spool(service_spool_input("ID-less LAN print"));
    const PrintJob job = store.reserve_job(
        service_job_input("bambu:no-id"), {{spool.id, 0, 100'000}});

    inventory.service->mark_bambu_dispatch_accepted(
        job.id, "printer-1", status("IDLE"), status("IDLE"));
    inventory.service->observe_bambu_status(status("RUNNING"));
    inventory.service->wait_for_idle();
    CHECK(store.get_job(job.id).state == JobState::printing);

    inventory.service->observe_bambu_status(status("FINISH"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(job.id).state == JobState::completed);
    CHECK(store.get_spool(spool.id).current_weight_mg == 400'000);
    CHECK(store.get_spool(spool.id).reserved_weight_mg == 0);
}

TEST_CASE("Bambu terminal snapshot before print start preserves pending correlation",
          "[FilamentInventory][BambuLifecycle]")
{
    TemporaryInventoryService inventory;
    Store &store = inventory.service->store();
    const Spool spool = store.create_spool(service_spool_input("Ambiguous print"));
    const PrintJob job = store.reserve_job(
        service_job_input("bambu:ambiguous"), {{spool.id, 0, 75'000}});

    inventory.service->mark_bambu_dispatch_accepted(
        job.id, "printer-1", status("IDLE"), status("IDLE"));
    inventory.service->observe_bambu_status(status("FINISH"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(job.id).state == JobState::reserved);
    CHECK(store.get_spool(spool.id).current_weight_mg == 500'000);
    CHECK(store.get_spool(spool.id).reserved_weight_mg == 75'000);

    inventory.service->observe_bambu_status(status("RUNNING"));
    inventory.service->observe_bambu_status(status("FINISH"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(job.id).state == JobState::completed);
    CHECK(store.get_spool(spool.id).current_weight_mg == 425'000);
}

TEST_CASE("Bambu exact job id completion remains automatic",
          "[FilamentInventory][BambuLifecycle]")
{
    TemporaryInventoryService inventory;
    Store &store = inventory.service->store();
    const Spool spool = store.create_spool(service_spool_input("Exact id"));
    const PrintJob job = store.reserve_job(
        service_job_input("bambu:exact"), {{spool.id, 0, 50'000}});

    inventory.service->mark_bambu_dispatch_accepted(
        job.id, "printer-1",
        status("IDLE"), status("RUNNING", "current-job"));
    inventory.service->observe_bambu_status(
        status("FINISH", "current-job"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(job.id).state == JobState::completed);
    CHECK(store.get_spool(spool.id).current_weight_mg == 450'000);
    const auto identifier = store.find_job(
        "bambu:printer-1", "job_id", "current-job");
    REQUIRE(identifier);
    CHECK(identifier->id == job.id);
}

TEST_CASE("Bambu lifecycle ignores a stale cached identifier from the preceding print",
          "[FilamentInventory][BambuLifecycle]")
{
    TemporaryInventoryService inventory;
    Store &store = inventory.service->store();
    const Spool spool = store.create_spool(service_spool_input("Stale identifier"));

    const PrintJob preceding = store.reserve_job(
        service_job_input("bambu:preceding"), {{spool.id, 0, 10'000}});
    store.bind_job_identifier(
        preceding.id, "bambu:printer-1", "job_id", "previous-job");
    store.commit_job(preceding.id);

    const PrintJob current = store.reserve_job(
        service_job_input("bambu:current"), {{spool.id, 0, 100'000}});
    inventory.service->mark_bambu_dispatch_accepted(
        current.id, "printer-1",
        status("FINISH", "previous-job"),
        status("FINISH", "previous-job"));

    inventory.service->observe_bambu_status(
        status("FINISH", "previous-job"));
    inventory.service->wait_for_idle();
    CHECK(store.get_job(current.id).state == JobState::reserved);

    inventory.service->observe_bambu_status(
        status("RUNNING", "previous-job"));
    inventory.service->wait_for_idle();
    CHECK(store.get_job(current.id).state == JobState::printing);

    inventory.service->observe_bambu_status(
        status("FINISH", "previous-job"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(current.id).state == JobState::completed);
    CHECK(store.get_spool(spool.id).current_weight_mg == 390'000);
    const auto identifier = store.find_job(
        "bambu:printer-1", "job_id", "previous-job");
    REQUIRE(identifier);
    CHECK(identifier->id == preceding.id);
}

TEST_CASE("Bambu stale id never completes an open preceding inventory job",
          "[FilamentInventory][BambuLifecycle]")
{
    TemporaryInventoryService inventory;
    Store &store = inventory.service->store();
    const Spool spool = store.create_spool(service_spool_input("Open stale job"));

    const PrintJob preceding = store.reserve_job(
        service_job_input("bambu:open-preceding"), {{spool.id, 0, 10'000}});
    store.bind_job_identifier(
        preceding.id, "bambu:printer-1", "job_id", "previous-open-job");
    store.mark_printing(preceding.id);

    const PrintJob current = store.reserve_job(
        service_job_input("bambu:after-open"), {{spool.id, 0, 100'000}});
    inventory.service->mark_bambu_dispatch_accepted(
        current.id, "printer-1",
        status("FINISH", "previous-open-job"),
        status("FINISH", "previous-open-job"));
    inventory.service->observe_bambu_status(
        status("FINISH", "previous-open-job"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(preceding.id).state == JobState::printing);
    CHECK(store.get_job(current.id).state == JobState::reserved);

    inventory.service->observe_bambu_status(
        status("RUNNING", "previous-open-job"));
    inventory.service->observe_bambu_status(
        status("FINISH", "previous-open-job"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(preceding.id).state == JobState::needs_review);
    CHECK(store.get_job(current.id).state == JobState::completed);
    CHECK(store.get_spool(spool.id).current_weight_mg == 400'000);
    CHECK(store.get_spool(spool.id).reserved_weight_mg == 10'000);
}

TEST_CASE("Bambu known job id change never completes the pending print",
          "[FilamentInventory][BambuLifecycle]")
{
    TemporaryInventoryService inventory;
    Store &store = inventory.service->store();
    const Spool spool = store.create_spool(
        service_spool_input("Known next generation"));

    const PrintJob other = store.reserve_job(
        service_job_input("bambu:other-known"), {{spool.id, 0, 10'000}});
    store.bind_job_identifier(
        other.id, "bambu:printer-1", "job_id", "other-known-job");

    const PrintJob pending = store.reserve_job(
        service_job_input("bambu:pending-known"), {{spool.id, 0, 100'000}});
    inventory.service->mark_bambu_dispatch_accepted(
        pending.id, "printer-1",
        status("IDLE"), status("RUNNING", "pending-job"));
    inventory.service->observe_bambu_status(
        status("FINISH", "other-known-job"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(pending.id).state == JobState::needs_review);
    CHECK(store.get_job(other.id).state == JobState::reserved);
    CHECK(store.get_spool(spool.id).current_weight_mg == 500'000);
    CHECK(store.get_spool(spool.id).reserved_weight_mg == 110'000);
}

TEST_CASE("Bambu foreign known id is not mistaken for a stale baseline id",
          "[FilamentInventory][BambuLifecycle]")
{
    TemporaryInventoryService inventory;
    Store &store = inventory.service->store();
    const Spool spool = store.create_spool(
        service_spool_input("Foreign known id"));

    const PrintJob known = store.reserve_job(
        service_job_input("bambu:known"), {{spool.id, 0, 10'000}});
    store.bind_job_identifier(
        known.id, "bambu:printer-1", "job_id", "known-job");

    const PrintJob pending = store.reserve_job(
        service_job_input("bambu:not-known"), {{spool.id, 0, 100'000}});
    inventory.service->mark_bambu_dispatch_accepted(
        pending.id, "printer-1",
        status("IDLE", "baseline-job"),
        status("RUNNING", "known-job"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(known.id).state == JobState::reserved);
    CHECK(store.get_job(pending.id).state == JobState::reserved);

    inventory.service->observe_bambu_status(
        status("RUNNING", "known-job"));
    inventory.service->observe_bambu_status(
        status("FINISH", "known-job"));
    inventory.service->wait_for_idle();

    CHECK(store.get_job(known.id).state == JobState::completed);
    CHECK(store.get_job(pending.id).state == JobState::reserved);
    CHECK(store.get_spool(spool.id).current_weight_mg == 490'000);
    CHECK(store.get_spool(spool.id).reserved_weight_mg == 100'000);
}

TEST_CASE("Bambu failure or idle transition never consumes reserved material",
          "[FilamentInventory][BambuLifecycle]")
{
    const auto verify_review = [](const std::string &terminal_status) {
        TemporaryInventoryService inventory;
        Store &store = inventory.service->store();
        const Spool spool = store.create_spool(
            service_spool_input("Unsuccessful " + terminal_status));
        const PrintJob job = store.reserve_job(
            service_job_input("bambu:" + terminal_status),
            {{spool.id, 0, 60'000}});

        inventory.service->mark_bambu_dispatch_accepted(
            job.id, "printer-1", status("IDLE"), status("IDLE"));
        inventory.service->observe_bambu_status(status("RUNNING"));
        inventory.service->observe_bambu_status(status(terminal_status));
        inventory.service->wait_for_idle();

        CHECK(store.get_job(job.id).state == JobState::needs_review);
        CHECK(store.get_spool(spool.id).current_weight_mg == 500'000);
        CHECK(store.get_spool(spool.id).reserved_weight_mg == 60'000);
    };

    SECTION("FAILED")
    {
        verify_review("FAILED");
    }
    SECTION("IDLE")
    {
        verify_review("IDLE");
    }
}
