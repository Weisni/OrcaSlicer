# Send-dialog exception recovery verification

The production `replace_job` expression from `SelectMachineDialog::on_send_print`
is extracted and compiled unchanged into a small C++17 executable. UI scheduling
and the print-job boundary use doubles; the test executes the real process and
finalization callbacks without a printer, HA connection, inventory or profile.

Run `python diagnostics/print-job-error-ui-tests/run_tests.py [source-root]`.
The default source root is the isolated `chamber-preheat-target/OrcaSlicer`
worktree. CMake uses the installed Visual Studio 2022 toolchain with at most two
build workers.

Observed RED before the production correction:

- Timeout and HTTP 409 both left the dialog in sending mode.
- An exception during inventory finalization escaped without scheduling recovery.

Observed GREEN after the correction: eight scenarios covering timeout, conflict,
success, cancellation, expired dialog before/after scheduling, replacement job,
and a finalization failure. The original error still reaches the worker reporter;
UI recovery does not invoke the physical job again.

The original `PrintJob::finalize` remains responsible for material reservations,
uncertain dispatch and authority reconciliation. No bookkeeping or network call
is added to the dialog recovery callback. A retained HA reservation can still
require explicit review before another user-initiated attempt.

This harness verifies the callback boundary, not rendering or a real wxWidgets
event loop. The actual application compile and a live, user-operated send-error
check remain separate validation. No physical print or preheat is started here.
