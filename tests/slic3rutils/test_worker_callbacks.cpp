#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"

using namespace Slic3r::GUI;

namespace {
class CallbackJob final : public Job {
    std::function<void()> m_callback;
public:
    bool continued = false;
    bool finalized = false;
    std::exception_ptr failure;

    explicit CallbackJob(std::function<void()> callback) : m_callback(std::move(callback)) {}
    void process(Ctl &ctl) override
    {
        ctl.call_on_main_thread(m_callback).get();
        continued = true;
    }
    void finalize(bool, std::exception_ptr &error) override
    {
        failure = error;
        error = nullptr;
        finalized = true;
    }
};
}

TEST_CASE("Main-thread rejection reaches job finalization without escaping event processing", "[Worker][Regression]")
{
    BoostThreadWorker worker(nullptr, "callback_rejection_test");
    auto rejected = std::make_shared<CallbackJob>([] {
        throw std::runtime_error("Another dispatch requires completion or review");
    });
    REQUIRE(worker.push(rejected));
    CHECK_NOTHROW(worker.wait_for_idle(3000));
    REQUIRE(worker.wait_for_idle(3000));
    CHECK(rejected->finalized);
    CHECK_FALSE(rejected->continued);
    REQUIRE(rejected->failure);
    CHECK_THROWS_MATCHES(std::rethrow_exception(rejected->failure), std::runtime_error,
                         Catch::Matchers::Message("Another dispatch requires completion or review"));

    auto next = std::make_shared<CallbackJob>([] {});
    REQUIRE(worker.push(next));
    REQUIRE(worker.wait_for_idle(3000));
    CHECK(next->finalized);
    CHECK(next->continued);
    CHECK_FALSE(next->failure);
}

TEST_CASE("Nonstandard main-thread failure remains available to the job", "[Worker][Regression]")
{
    BoostThreadWorker worker(nullptr, "callback_failure_test");
    auto rejected = std::make_shared<CallbackJob>([] { throw 73; });
    REQUIRE(worker.push(rejected));
    CHECK_NOTHROW(worker.wait_for_idle(3000));
    REQUIRE(worker.wait_for_idle(3000));
    REQUIRE(rejected->failure);
    CHECK_THROWS_AS(std::rethrow_exception(rejected->failure), int);
    CHECK_FALSE(rejected->continued);
}
