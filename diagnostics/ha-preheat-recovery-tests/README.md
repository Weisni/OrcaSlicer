# HA preheat transport recovery checks

Run `python diagnostics/ha-preheat-recovery-tests/run_tests.py [source-root]`
with Visual Studio 2022 and CMake available. The default source is this checkout.
The harness compiles the actual GUI Client and pure Session with fake HTTP and
deterministic clocks. Seventeen scenarios cover idempotent request recovery,
fresh-readiness invalidation, bounded outages, original expiry and the single
confirmed fifteen-minute allowance, definitive errors, and explicit cancellation.
It never accesses Home Assistant or a printer. GUI builds and physical acceptance
remain separate checks.
