# HA incremental synchronization implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the
> isolated server, inventory transport and profile-cache tasks; integrate their
> shared protocol and review the complete change before completion.

**Goal:** Fast authoritative HA synchronization while preserving unsaved local
material overrides.

**Architecture:** An unchanged generation/revision returns no graph. Changed
snapshots carry stable-key row deltas applied atomically; immutable profile
content is cached by digest. GUI refresh runs asynchronously and publishes a
complete result without changing edited material settings.

**Tech Stack:** Python/SQLite HA component; C++17/wxWidgets/nlohmann JSON;
Catch2 and Python unittest/pytest.

**Spec:** `docs/superpowers/specs/2026-10-09-ha-incremental-sync-design.md`

## Global constraints

- Preserve schema-8 inventory and existing API compatibility; optional HA mode
  must not alter behavior when disabled.
- Maximum native graph: 64 MiB and 250,000 rows.
- No automatic upload of local edits, no live deployment or printer actions.
- At most two compiler workers; no simultaneous whole-GUI builds.
- Preserve the primary checkout and existing user/system profiles.

## Review focus

- A temporary edit remains active after background refresh or HA revision change.
- An older network response cannot overwrite a newer acknowledged association.
- Server restart or discarded delta history requests full resync.
- Profile contents shared by rolls are downloaded once and survive reassignment.
- An interrupted upload or graph update preserves the previous usable state.

### Task 1: HA read protocol and immutable profile content

Files: component `read_api.py`, `profiles.py`, `store.py`, `__init__.py`, focused
Python tests; create a focused sync module if needed.

Interfaces: authenticated GET `/sync?generation=...&revision=...` produces the
spec envelope; GET `/profile_content?sha256=...` returns an existing sealed
profile. Existing write endpoints continue to accept profiles and persist
immutable content transactionally.

- [x] Add tests proving unchanged reads avoid `snapshot`, row changes/deletions
  are minimal, reset is complete, and old profile content remains readable.
- [x] Run those tests and record the expected failures.
- [x] Implement bounded revision history, stable identities, transactionally
  captured deltas and immutable profile storage/backfill.
- [x] Run focused and full component suites, including access boundaries.

### Task 2: Native inventory transport and atomic incremental import

Files: `HaInventoryTransport.hpp`, `HaInventoryDemo.hpp`,
`HaInventoryAuthority.hpp`, `FilamentInventory.cpp`, focused existing tests.

Interfaces: consumes Task 1's `/sync` envelope; merges `native_delta.tables`
upserts/deletes by `row_key`; refresh reuses the last acknowledged graph on
unchanged replies. Detect older servers and use the existing bounded read path.

- [x] Add delta tests for reset, single update, deletion, malformed/duplicate
  identities, generation mismatch and unchanged/no-local-export behavior.
- [x] Watch focused tests fail, then implement minimal validated merge and
  import; avoid touching unchanged SQLite rows.
- [x] Verify existing authority receipts, conflict checks and rollback.

### Task 3: Persistent profiles and asynchronous provider refresh

Files: `HaMaterialProvider.hpp`, focused profile/refresh helper headers and
tests. Coordinate public helpers with Task 2; do not edit its owned files.

Interfaces: consumes all profile summaries from provider snapshots and immutable
`/profile_content`; expose asynchronous `refresh_if_due` and a GUI-thread
completion step if needed. Cache/install helpers deduplicate content and do
not replace edited preset settings.

- [x] Add regression tests for same content/different roll or name, changed
  association during download, persistent cache integrity, and local overrides.
- [x] Watch them fail; implement deduplicated persistent downloads and preload
  every published profile/variant, keeping compatibility checks on selection.
- [x] Ensure one refresh is in flight, endpoint changes cancel publication, and
  shutdown joins workers safely; network work must not hold GUI locks.
- [x] Run existing material profile, binding and refresh tests.

### Task 4: GUI integration, explicit publication and verification

Files: `Plater.cpp`, `HaMaterialSyncDialog.hpp`, docs and README.

Interfaces: call Task 3's completion/preload helpers; explicit publication
uploads selected effective settings through existing HA writes, then switches
to the confirmed managed profile while preserving independent temporary edits.

- [x] Inspect all callers and pin override preservation with focused tests.
- [x] Integrate confirmed profile assignment and asynchronous completion.
- [x] Run component integration tests, native contract tests and changed GUI
  compilation with at most two workers; measure request/byte savings locally.
- [x] Perform an independent review and fix material findings; record validation
  limits, update README/guidelines and commit the isolated change.

## Progress

User instruction "implement this" authorizes continuous implementation of the
approved conversational design, including the additional override requirement.
The written spec and plan make that instruction reviewable; no extra approval
round is inferred from the skills. Offline cache may support editing; existing
online print-start checks stay in force.

## Verification record — 2026-10-09

The isolated implementation starts at HA-enabled commit `82913dede6`; the primary
checkout and its unrelated arrangement/3MF changes remain separate.

| Check | Result |
| --- | --- |
| Complete Python component suite | 258 tests passed |
| Profile/cache/async/override contracts | 289 assertions, 25 cases passed |
| Preset-linked profile/binding integration | 281 assertions, 27 cases passed |
| Delta transport | 61 assertions, 14 cases passed |
| Atomic native import | 20 assertions, 1 case passed |
| Existing inventory regressions | 1,003 assertions, 51 cases passed |
| Windows GUI/source compilation | Plater, PresetComboBoxes, GUI_App, Tab and FilamentInventory compiled |
| Independent static review | Findings corrected; no Important/Critical findings remain |

The Preset integration target uses current headers/test sources and the existing
HA checkout's core archive. The standalone inventory regression target excludes
one unrelated sliced-GCode-usage case. The GUI check compiles a focused static
archive using configured dependencies, with one compiler worker; it does not
link a complete application. Existing macro-redefinition/unused-variable
warnings remain in unrelated headers. Scratch projects/binaries are untracked.

Regression failures were observed before implementing delta/reset handling,
immutable content, profile identity/cache validation, stale async rejection,
temporary-setting merging, memory eviction and offline-cache completeness.
Final review verified effective-preset capture in the association picker and
corrected the lost material index after closing its editor. Unsaved edits follow
the replacement HA base; conflicting replacements of a shared edited preset
are rejected before project mutation rather than silently losing settings.
The same guard prevents temporary settings from leaking into another project
filament that already uses the replacement profile.
Current revision checks now bypass snapshot creation even without retained
history, and ready workers are completed independently of printer telemetry.

Synthetic 1,000-customer data: legacy 16 requests / 1,092,156 bytes; reset one
request / 1,091,256 bytes; one changed row one request / 4,066 bytes; unchanged
one request / 123 bytes. This measures transfer volume on local test data and
does not claim measured live HA or GUI latency.

Remaining acceptance: complete cross-platform builds, rendered dirty-profile
selection/refresh, offline restart and telemetry-independent refresh; deployment
and physical printer tests need a separate requested operation. No live HA,
printer, installation, push, merge or release action was performed.
