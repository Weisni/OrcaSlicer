"""Compile the real HA preheat Client with fake HTTP and deterministic clocks."""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parents[1]
text = (SOURCE / 'src/slic3r/GUI/HaJobPreheat.hpp').read_text(encoding='utf-8')
client = text[text.index('class Client {'):text.rindex('} // namespace')]
harness = r'''
#include "libslic3r/HaJobPreheat.hpp"
#include <chrono>
#include <optional>
#include <algorithm>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace boost::uuids {
struct random_generator { std::string operator()() { return "instance-a"; } };
std::string to_string(const std::string &value) { return value; }
}
using Json = nlohmann::json;
namespace Slic3r::GUI {
class HaInventoryRequestError : public std::runtime_error {
public:
    unsigned status;
    HaInventoryRequestError(unsigned code, const std::string &message) : std::runtime_error(message), status(code) {}
};
struct Request { std::string url, token; Json body; bool posting; };
std::deque<std::function<Json(const Request &)>> replies;
std::vector<Request> requests;
Json ha_inventory_request_raw_with_token(const std::string &url, const std::string &token, const Json *body = nullptr) {
    requests.push_back({url, token, body ? *body : Json(), bool(body)});
    if (replies.empty()) throw std::logic_error("unexpected extra HTTP request");
    auto reply = std::move(replies.front()); replies.pop_front();
    return reply(requests.back());
}
namespace HaJobPreheat {
std::int64_t wall_time = 1000, elapsed_seconds = 0;
std::int64_t now() { return wall_time; }
std::chrono::steady_clock::time_point monotonic_now() {
    return std::chrono::steady_clock::time_point(std::chrono::seconds(elapsed_seconds));
}
__CLIENT__
}
}
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::HaJobPreheat;
Json parameters() { return {{"enabled",true},{"chamber_target",55},{"print_bed_target",60},{"preheat_bed_target",110}}; }
Json response(const std::string &phase = "heating", double expires = 5000) {
    return {{"schema_version",1},{"job_uuid","job-a"},{"printer_id","printer-a"},{"instance_id","instance-a"},
        {"version",2},{"expires_at",expires},{"phase",phase},{"reason",phase == "ready" ? "target_reached" : "stabilizing"},
        {"ready",phase == "ready"},{"parameters",parameters()}};
}
Client client() { return {"http://ha/api/quack_materials/materials","secret-token","job-a","printer-a",parameters()}; }
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class Fn> void require_throw(Fn fn, const char *message) {
    bool threw = false;
    try { fn(); } catch (const std::runtime_error &) { threw = true; }
    require(threw,message);
}
void transport() { replies.push_back([](const Request &) -> Json { throw HaInventoryRequestError(0,"curl Error 28"); }); }
void answer(const Json &value) { replies.push_back([value](const Request &) { return value; }); }
void reset() { requests.clear(); replies.clear(); wall_time = 1000; elapsed_seconds = 0; }

void initial_lost_reply_reuses_identity() {
    auto c = client();
    transport();
    require(!c.start(),"a lost preheat request reply must permit bounded recovery");
    require(c.phase() == "waiting_for_connection","initial request loss must expose the connection state");
    require(!c.reason().empty(),"connection recovery must explain the pending retry");
    require_throw([&] { c.claim(); },"uncertain initial request must not authorize dispatch");
    elapsed_seconds = 2;
    answer(response());
    require(!c.read(),"recovered heating state must still wait");
    require(requests.size() == 2,"one poll must make exactly one recovery request");
    require(requests[0].posting && requests[1].posting,"unknown initial outcome must replay the preheat request");
    require(requests[0].body == requests[1].body,"replayed preheat request must retain its exact idempotent identity");
    require(requests[0].url == requests[1].url,"recovery must preserve the job endpoint");
    answer(response("ready"));
    require(c.read(),"recovered job may become ready");
    require(!requests.back().posting,"known preheat jobs must recover through status GET");
    require(c.claim().at("instance_id") == "instance-a","claim must retain the original instance");
}
void get_timeout_then_recovers() {
    auto c = client(); answer(response()); require(!c.start(),"initial job must heat");
    transport(); require(!c.read(),"one status timeout must not abort preheat");
    require(c.phase() == "waiting_for_connection","poll timeout must replace stale heating text");
    answer(response("ready")); require(c.read(),"next successful poll may restore readiness");
    require(c.phase() == "ready","successful recovery must clear the connection state");
    require(!requests[1].posting && !requests[2].posting,"status recovery must never replay a print/preheat command");
}
void cached_ready_cannot_claim_during_outage() {
    auto c = client(); answer(response("ready")); require(c.start(),"initial readiness must be valid");
    require(!c.claim().empty(),"confirmed readiness must authorize a claim payload");
    transport(); require(!c.read(),"timeout must withhold readiness");
    require_throw([&] { c.claim(); },"cached readiness must not survive a lost connection");
    answer(response()); require(!c.read(),"fresh heating response must not reuse old readiness");
    require_throw([&] { c.claim(); },"nonready recovery must still withhold claim");
}
void outage_is_bounded() {
    auto c = client(); answer(response()); c.start(); transport(); require(!c.read(),"first outage enters recovery");
    elapsed_seconds = 59; transport(); require(!c.read(),"outage may recover within its budget");
    elapsed_seconds = 60;
    require_throw([&] { c.read(); },"prolonged outage must terminate after sixty seconds");
    require(requests.size() == 3,"expired outage must not send another request");
}
void request_duration_counts_towards_outage() {
    auto c = client();
    replies.push_back([](const Request &) -> Json { elapsed_seconds = 61; throw HaInventoryRequestError(0,"late timeout"); });
    require_throw([&] { c.start(); },"the failed request duration must count towards the outage budget");
}
void definitive_conflict_is_not_retried() {
    auto c = client();
    replies.push_back([](const Request &) -> Json { throw HaInventoryRequestError(409,"manual preheat occupied"); });
    try { c.start(); require(false,"HTTP 409 must propagate"); }
    catch (const HaInventoryRequestError &error) { require(error.status == 409,"definitive error must keep its HTTP status"); }
    require(requests.size() == 1,"definitive conflicts must not retry");
}
void terminal_status_cannot_resume() {
    auto c = client(); answer(response()); c.start(); transport(); require(!c.read(),"outage enters recovery");
    answer(response("manual_takeover"));
    require_throw([&] { c.read(); },"server denial must terminate recovery");
    require_throw([&] { c.claim(); },"terminal server status must not retain claim readiness");
}
void original_expiry_bounds_recovery() {
    auto c = client(); answer(response("heating",1010)); c.start();
    transport(); require(!c.read(),"short outage may recover before expiry");
    wall_time = 1010; elapsed_seconds = 10;
    require_throw([&] { c.read(); },"server expiry must end recovery before sixty seconds");
    require(requests.size() == 2,"expired server job must not be polled again");
}
void late_success_cannot_extend_original_expiry() {
    auto c = client(); answer(response("heating",1010)); c.start();
    transport(); require(!c.read(),"short outage enters recovery");
    replies.push_back([](const Request &) { wall_time = 1010; elapsed_seconds = 10; return response("ready",5000); });
    require_throw([&] { c.read(); },"late response must not extend the original job lifetime");
    require_throw([&] { c.claim(); },"late ready reply must not authorize dispatch");
}
void cancel_does_not_replay_pending_start() {
    auto c = client(); transport(); require(!c.start(),"lost start enters recovery");
    answer(response()); answer(response("cancelled"));
    require(c.cancel(),"explicit cancel must recover version and cancel the same job");
    require(requests.size() == 3,"cancel must use exactly one GET and one explicit cancel");
    require(!requests[1].posting,"cancel must not replay pending preheat start");
    require(requests[2].url.find("/cancel") != std::string::npos,"only the explicit cancellation may follow the GET");
}
void cancel_timeout_propagates_once() {
    auto c = client(); answer(response()); c.start(); transport(); require(!c.read(),"outage enters recovery");
    transport(); require_throw([&] { c.cancel(); },"cancel status failure must propagate without an automatic retry");
    require(requests.size() == 3,"cancel must make only one status request when transport fails");
}
void cancel_command_timeout_is_not_retried() {
    auto c = client(); answer(response()); c.start(); answer(response()); transport();
    require_throw([&] { c.cancel(); },"a lost cancel command reply must propagate");
    require(requests.size() == 3,"a cancel write must never retry automatically");
}
void malformed_response_invalidates_readiness() {
    auto c = client(); answer(response("ready")); c.start();
    auto invalid = response("ready"); invalid["job_uuid"] = "other-job"; answer(invalid);
    require_throw([&] { c.read(); },"protocol mismatch must propagate");
    require_throw([&] { c.claim(); },"protocol mismatch must not retain cached readiness");
}
void skip_transport_failure_invalidates_readiness() {
    auto c = client(); answer(response("ready")); c.start(); transport();
    require_throw([&] { c.skip(); },"a failed explicit skip must propagate");
    require_throw([&] { c.claim(); },"skip transport failure must invalidate cached readiness");
    require(requests.size() == 2,"explicit skip must not retry automatically");
}
void cancel_transport_failure_invalidates_readiness() {
    auto c = client(); answer(response("ready")); c.start(); transport();
    require_throw([&] { c.cancel(); },"a failed cancel status request must propagate");
    require_throw([&] { c.claim(); },"cancel transport failure must invalidate cached readiness");
}
void fresh_bounded_grace_updates_confirmed_expiry() {
    auto c = client(); answer(response("heating",1010)); c.start();
    answer(response("heating",1910)); wall_time = 1005;
    require(!c.read(),"a confirmed extension itself cannot authorize a print");
    wall_time = 1015; answer(response("ready",1910));
    require(c.read(),"fresh server-authorized grace may outlive the first expiry");
    require(!c.claim().empty(),"fresh plateau readiness within confirmed grace can be claimed");
    wall_time = 1910;
    require_throw([&] { c.claim(); },"grace still has a firm expiry");
}
void grace_cannot_keep_extending_or_trust_unconfirmed_readiness() {
    auto c = client(); answer(response("heating",1010)); c.start();
    answer(response("heating",1910)); wall_time = 1005; c.read();
    answer(response("ready",1911));
    require_throw([&] { c.read(); },"server grace is capped to one additional fifteen minutes");
    require_throw([&] { c.claim(); },"an excessive expiry invalidates cached readiness");
}
int main() {
    int failed = 0;
    const auto run = [&failed](const char *name, auto test) {
        reset();
        try { test(); require(replies.empty(),"test left an expected request unconsumed"); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    run("lost initial reply reuses identity",initial_lost_reply_reuses_identity);
    run("GET timeout then recovery",get_timeout_then_recovers);
    run("cached readiness invalidated",cached_ready_cannot_claim_during_outage);
    run("outage bounded to sixty seconds",outage_is_bounded);
    run("request duration counts",request_duration_counts_towards_outage);
    run("HTTP conflict propagates",definitive_conflict_is_not_retried);
    run("terminal denial propagates",terminal_status_cannot_resume);
    run("original server expiry bounds recovery",original_expiry_bounds_recovery);
    run("late success cannot extend expiry",late_success_cannot_extend_original_expiry);
    run("cancel never replays pending start",cancel_does_not_replay_pending_start);
    run("cancel GET timeout propagates once",cancel_timeout_propagates_once);
    run("cancel command timeout propagates once",cancel_command_timeout_is_not_retried);
    run("malformed response invalidates readiness",malformed_response_invalidates_readiness);
    run("skip transport invalidates readiness",skip_transport_failure_invalidates_readiness);
    run("cancel transport invalidates readiness",cancel_transport_failure_invalidates_readiness);
    run("fresh bounded server grace",fresh_bounded_grace_updates_confirmed_expiry);
    run("grace cannot renew indefinitely",grace_cannot_keep_extending_or_trust_unconfirmed_readiness);
    return failed ? 1 : 0;
}
'''
build = HERE / 'build'
build.mkdir(exist_ok=True)
(build / 'ha_preheat_recovery_tests.cpp').write_text(harness.replace('__CLIENT__',client), encoding='utf-8')
subprocess.run(['cmake','-S',str(HERE),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64',f'-DQUACK_ROOT={SOURCE}'],check=True)
subprocess.run(['cmake','--build',str(build),'--config','Release','--parallel','32'],check=True)
subprocess.run(['ctest','--test-dir',str(build),'-C','Release','--output-on-failure'],check=True)
