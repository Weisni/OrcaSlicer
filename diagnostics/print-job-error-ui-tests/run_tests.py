"""Execute SelectMachine's actual job/finalizer callbacks with UI boundary doubles.

The production replace_job expression is compiled unchanged. No wxWidgets event
loop, printer, profile, inventory or Home Assistant connection is created.
"""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parents[1]
text = (SOURCE / 'src/slic3r/GUI/SelectMachine.cpp').read_text(encoding='utf-8')
start = text.index('    replace_job(*m_worker,', text.index('void SelectMachineDialog::on_send_print()'))
pos = start + text[start:].index('(')
depth = 1
end = pos + 1
while depth:
    depth += (text[end] == '(') - (text[end] == ')')
    end += 1
dispatch = text[start:end] + ';'

harness = r'''
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct Job { struct Ctl {}; };
struct PrintJob {
    int process_calls = 0, finalize_calls = 0;
    bool fail_finalize = false;
    bool canceled = false;
    void process(Job::Ctl &) { ++process_calls; }
    void finalize(bool cancel, std::exception_ptr &) {
        ++finalize_calls;
        canceled = cancel;
        if (fail_finalize) throw std::logic_error("inventory finalization failed");
    }
};
struct Worker {
    std::function<void(Job::Ctl &)> process;
    std::function<void(bool, std::exception_ptr &)> finish;
};
void replace_job(Worker &worker, std::shared_ptr<PrintJob> job) {
    worker.process = [job](Job::Ctl &ctl) { job->process(ctl); };
    worker.finish = [job](bool canceled, std::exception_ptr &error) { job->finalize(canceled, error); };
}
template<class Process, class Finalize>
void replace_job(Worker &worker, Process process, Finalize finalize) {
    worker.process = std::move(process);
    worker.finish = std::move(finalize);
}
enum class PrintDialogStatus { PrintStatusInit = 0 };
struct Dialog {
    std::shared_ptr<int> m_token = std::make_shared<int>(0);
    std::shared_ptr<PrintJob> m_print_job = std::make_shared<PrintJob>();
    std::unique_ptr<Worker> m_worker = std::make_unique<Worker>();
    bool m_is_in_sending_mode = true;
    bool editing_enabled = false, skip_visible = true;
    int status = 1;
    std::vector<std::function<void()>> deferred;
    void CallAfter(std::function<void()> fn) { deferred.push_back(std::move(fn)); }
    void show_status(PrintDialogStatus value) { status = int(value); }
    void prepare_mode() {
        m_is_in_sending_mode = false;
        editing_enabled = true;
        skip_visible = false;
    }
    void drain() {
        auto callbacks = std::move(deferred);
        for (auto &callback : callbacks) callback();
    }
    void dispatch() {
__DISPATCH__
    }
};
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void process(Dialog &dialog) {
    dialog.dispatch();
    Job::Ctl ctl;
    dialog.m_worker->process(ctl);
}
void error_returns_to_prepare(const std::string &message) {
    Dialog dialog;
    process(dialog);
    auto error = std::make_exception_ptr(std::runtime_error(message));
    const auto original = error;
    dialog.m_worker->finish(false, error);
    require(error == original, "the original error must reach the worker's error reporter");
    require(dialog.m_print_job->finalize_calls == 1, "inventory finalization must run exactly once");
    require(dialog.m_is_in_sending_mode, "UI reset must be deferred until finalization has returned");
    dialog.drain();
    require(!dialog.m_is_in_sending_mode, "failed job leaves the send dialog stuck in sending mode");
    require(dialog.editing_enabled, "failed job must restore editing");
    require(!dialog.skip_visible, "failed job must remove the stale preheat action");
    require(dialog.status == 0, "failed job must clear the sending status");
    require(dialog.m_print_job->process_calls == 1, "UI recovery must not retry physical dispatch");
}
void no_error_keeps_existing_flow(bool canceled) {
    Dialog dialog;
    process(dialog);
    std::exception_ptr error;
    dialog.m_worker->finish(canceled, error);
    require(dialog.m_print_job->canceled == canceled, "cancellation must reach the real job");
    require(dialog.deferred.empty(), "normal completion/cancel must retain its existing UI path");
}
void expired_dialog_is_ignored() {
    Dialog dialog;
    process(dialog);
    dialog.m_token.reset();
    auto error = std::make_exception_ptr(std::runtime_error("closed"));
    dialog.m_worker->finish(false, error);
    require(dialog.deferred.empty(), "finalization must not schedule UI on a closed dialog");
}
void expired_dialog_after_schedule_is_ignored() {
    Dialog dialog;
    process(dialog);
    auto error = std::make_exception_ptr(std::runtime_error("closed"));
    dialog.m_worker->finish(false, error);
    dialog.m_token.reset();
    dialog.drain();
    require(dialog.m_is_in_sending_mode, "deferred callback must recheck the dialog lifetime");
}
void replaced_job_is_ignored() {
    Dialog dialog;
    process(dialog);
    auto error = std::make_exception_ptr(std::runtime_error("old job"));
    dialog.m_worker->finish(false, error);
    dialog.m_print_job = std::make_shared<PrintJob>();
    dialog.drain();
    require(dialog.m_is_in_sending_mode, "an old failure must not reset a replacement job");
}
void finalized_error_also_restores_ui() {
    Dialog dialog;
    process(dialog);
    dialog.m_print_job->fail_finalize = true;
    std::exception_ptr error;
    dialog.m_worker->finish(false, error);
    require(bool(error), "a finalization failure must still reach the error reporter");
    dialog.drain();
    require(!dialog.m_is_in_sending_mode, "finalization failure must also restore the dialog");
}
int main() {
    int failed = 0;
    const auto run = [&failed](const char *name, auto test) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    run("HA timeout restores prepare", [] { error_returns_to_prepare("curl Error 28: Connection timed out after 15005 milliseconds"); });
    run("HA conflict restores prepare", [] { error_returns_to_prepare("409: manual preheat occupies this printer"); });
    run("success keeps existing flow", [] { no_error_keeps_existing_flow(false); });
    run("cancel keeps existing flow", [] { no_error_keeps_existing_flow(true); });
    run("expired dialog is ignored", expired_dialog_is_ignored);
    run("deferred callback rechecks lifetime", expired_dialog_after_schedule_is_ignored);
    run("replacement job is preserved", replaced_job_is_ignored);
    run("finalization failure restores prepare", finalized_error_also_restores_ui);
    return failed ? 1 : 0;
}
'''
build = HERE / 'build'
build.mkdir(exist_ok=True)
(build / 'print_job_error_ui_tests.cpp').write_text(
    harness.replace('__DISPATCH__', dispatch), encoding='utf-8')
subprocess.run(['cmake', '-S', str(HERE), '-B', str(build), '-G', 'Visual Studio 17 2022', '-A', 'x64'], check=True)
subprocess.run(['cmake', '--build', str(build), '--config', 'Release', '--parallel', '2'], check=True)
subprocess.run(['ctest', '--test-dir', str(build), '-C', 'Release', '--output-on-failure'], check=True)
