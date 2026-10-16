# Manual preheat card tests

Run `node diagnostics/manual-preheat-card-tests/test_manual_preheat_card.cjs`.
Set `MANUAL_PREHEAT_HTTP=1` to exercise the local HTTP UUID fallback.
Rendering, polling and successor inspection must never issue heater commands.
