#include <catch2/catch_all.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include "libslic3r/NativePrintDispatch.hpp"
#include <functional>
#include <optional>
#include <stdexcept>
#include <exception>
#include <memory>
#include <utility>
#include <atomic>
#include <future>
#include <shared_mutex>
#include <chrono>

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

TEST_CASE("A queued native print rechecks HA mode before invoking the agent", "[Worker][NativePrintDispatch][Regression]")
{
    using Slic3r::NativePrintDispatch::Mode;
    const Mode dispatch_mode = GENERATE(Mode{false, false}, Mode{true, false}, Mode{false, true}, Mode{true, true});
    const bool unavailable_at_dispatch = GENERATE(false, true);
    std::optional<Mode> mode = Mode{false, false};
    const auto snapshot = [&] { return mode; };
    int agent_calls = 0;
    std::function<int()> queued;
    REQUIRE(Slic3r::NativePrintDispatch::run(snapshot, [&] {
        queued = [&] { return Slic3r::NativePrintDispatch::run(snapshot, [&] { ++agent_calls; return 17; }); };
        return 0;
    }) == 0);
    REQUIRE(bool(queued));
    mode = unavailable_at_dispatch ? std::nullopt : std::optional<Mode>(dispatch_mode);
    const bool blocked = unavailable_at_dispatch || dispatch_mode.demo || dispatch_mode.provider;
    CHECK(queued() == (blocked ? -1 : 17));
    CHECK(agent_calls == (blocked ? 0 : 1));
}

TEST_CASE("Untracked print admission fails closed when the mode snapshot is unavailable", "[Worker][NativePrintDispatch][Regression]")
{
    using Slic3r::NativePrintDispatch::Mode;
    int dispatched = 0;
    const auto dispatch = [&] { ++dispatched; return 0; };
    CHECK(Slic3r::NativePrintDispatch::run([] { return std::optional<Mode>(); }, dispatch) == -1);
    CHECK(Slic3r::NativePrintDispatch::run([]() -> std::optional<Mode> {
        throw std::runtime_error("The GUI snapshot is unavailable");
    }, dispatch) == -1);
    CHECK(dispatched == 0);
}

TEST_CASE("HA modes reject a native print before it enters the queue", "[Worker][NativePrintDispatch][Regression]")
{
    using Slic3r::NativePrintDispatch::Mode;
    const Mode mode = GENERATE(Mode{true, false}, Mode{false, true}, Mode{true, true});
    int queued = 0;
    CHECK(Slic3r::NativePrintDispatch::run([&] { return std::optional<Mode>(mode); }, [&] { ++queued; return 0; }) == -1);
    CHECK(queued == 0);
}

TEST_CASE("A native agent exception is preserved without retrying a dispatch", "[Worker][NativePrintDispatch][Regression]")
{
    using Slic3r::NativePrintDispatch::Mode;
    int calls = 0;
    CHECK_THROWS_AS(Slic3r::NativePrintDispatch::run([] { return std::optional<Mode>(Mode{false, false}); }, [&]() -> int {
        ++calls;
        throw std::runtime_error("The dispatch outcome is uncertain");
    }), std::runtime_error);
    CHECK(calls == 1);
}

TEST_CASE("Agent reload refuses active sends and concurrent native sends retain their leases", "[Worker][NativePrintDispatch][Regression]")
{
    struct Agent { int result = 23; } current;
    std::atomic<Agent *> agent{&current};
    std::shared_mutex lifetime;
    std::promise<void> first_entered, second_entered, release;
    auto released = release.get_future().share();
    auto first_ready = first_entered.get_future();
    auto second_ready = second_entered.get_future();
    const auto send = [&](std::promise<void> &entered) {
        return Slic3r::NativePrintDispatch::with_agent(lifetime, agent, [&](Agent &active) {
            entered.set_value();
            released.wait();
            return active.result;
        });
    };
    auto first = std::async(std::launch::async, [&] { return send(first_entered); });
    const bool first_running = first_ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    auto second = std::async(std::launch::async, [&] { return send(second_entered); });
    const bool both_running = second_ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    const bool detached_while_busy = Slic3r::NativePrintDispatch::try_detach(lifetime, agent);
    release.set_value(); // Always release before assertions, including a failed concurrency check.
    CHECK(first_running);
    CHECK(both_running);
    CHECK_FALSE(detached_while_busy);
    CHECK(first.get() == 23);
    CHECK(second.get() == 23);
    REQUIRE(Slic3r::NativePrintDispatch::try_detach(lifetime, agent));
    int stale_calls = 0;
    CHECK(Slic3r::NativePrintDispatch::with_agent(lifetime, agent, [&](Agent &) { ++stale_calls; return 0; }) == -1);
    CHECK(stale_calls == 0);
}
