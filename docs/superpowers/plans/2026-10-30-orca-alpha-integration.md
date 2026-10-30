# OrcaSlicer 2.5.0 Alpha integration implementation plan

> **For agentic workers:** Use `superpowers:executing-plans` to implement this
> plan task by task after design review. Steps use checkboxes for tracking.

**Goal:** Deliver every tagged Orca Alpha capability while preserving Quack
workflow outcomes and identifying custom implementations that upstream can replace.

**Architecture:** Merge the pinned Alpha into current Quack main in an isolated
worktree. First compare each custom feature with unmodified Orca and prepare
user-repeatable trials. Retain custom implementations until a tested upstream
replacement and the user's migration decision establish how to preserve outcomes.

**Tech stack:** C++17, wxWidgets, CMake, Catch2, Python 3.12.13, SQLite,
JavaScript/Node, CGAL 6.2.1, OCCT 8.0.1, SolveSpace and bundled plugin tooling.

**Spec:** [Integration design](../specs/2026-10-30-orca-alpha-integration-design.md).
Status: implementation authorized on 2026-10-30; the user selected Beta instead of Alpha. See the execution ledger for current results.

## Global constraints

- Quack baseline: `2e38319f4e0762bc4cf44d46a28160e0fa2ae3f5` (2.5.24).
- Current target: `345efec64c7b95a05ce5cf96df0dde66a158835c` (`v2.5.0-beta`), explicitly selected by the user on 2026-10-30.
- Preserve primary-checkout changes and every Q1-Q12 contract from the design.
- Code/Git documentation in English; user-facing findings and summaries in German.
- Windows x64/ARM64, macOS Intel/ARM64/universal and Linux x64/ARM64/Flatpak
  must retain the supported package matrix; no platform may silently lose features.
- Existing projects/profiles/databases require explicit compatibility/migration tests.
- Optional Quack behavior must match upstream with the option disabled.
- Use separate test data, credential identity and databases; no physical sends,
  heating, HA deployment, merge, tag or publication is authorized by this plan.
- Keep authenticated SSH access open for as long as an authorized task needs it.

## Review focus

1. Virtual mixes, support and purge can double-charge or omit physical-roll usage (T4/T7).
2. Preset migration/reload can erase HA local overrides or select the wrong nozzle variant (T4/T8).
3. Plugin/printer events can duplicate terminal accounting or bypass preheat ownership (T3/T6/T7).
4. New tower, support and bridge paths can erase custom wave/tower/paint behavior (T5).
5. Packaging/ABI or credential identity can make a tested developer build differ from the delivered app (T2/T9).

## Files and dependencies

Track per-entry status/evidence in `docs/upstream/orca-2.5.0-alpha-coverage.csv`
and per-shared-path resolution in `docs/upstream/orca-2.5.0-alpha-shared-paths.csv`.
Suggested statuses: `planned`, `in_progress`, `verified`, `blocked`; add evidence
with commit SHA, command/log, platform and GUI or live acceptance scope. Existing
equivalent backports require evidence, not a silent deletion. Avoid `not_applicable`
for a supported upstream capability just because the user's P2S cannot exercise it.

Track custom-feature replacement decisions in
`docs/upstream/quack-orca-standard-comparison.md`; every Q1-Q12 item needs a
disposition, including partial matches and features with no stock equivalent.

Order: T0 -> T1 -> T2 -> T3/T4 -> T5/T6 -> T7/T8 -> T9. This indicates dependencies,
not authorization for parallel agents or concurrent editing of shared C++ files.
T4's physical-material contract must settle before T5/T6 consume it. T7 has an
explicit standalone lifecycle/accounting gate even if code overlaps T4/T6.

## T0: Evaluate Orca standard replacements before porting custom features

**Files:** `docs/upstream/quack-orca-standard-comparison.md`, both ledgers,
design and README; read the Quack/Orca source anchors from Q1-Q12.
**Produces:** a per-feature decision matrix and reproducible trials the user
can run in an unmodified official Orca build before choosing a replacement.

- [ ] For every Q1-Q12 contract, split independently replaceable behaviors and
  compare the pinned Quack implementation with the pinned official Alpha.
  Link upstream code, settings, documented workflow and relevant fixes. Similar
  names, presence of a plugin API or a formerly reported upstream fix do not
  establish functional equivalence.
- [ ] Classify each behavior as `stock_equivalent`, `stock_partial`,
  `stock_with_adapter`, `custom_plugin_required`, `no_equivalent` or
  `not_yet_evaluated`. Separate built-in standards from third-party plugins and
  proposed custom plugins. Record supported printers/platforms, default and
  failure behavior, persistence, costs/stock and safety-related differences.
- [ ] Prioritize suspected overlap in towers, color penetration, supports,
  3MF/preset fixes, nozzle variants and preheat; also assess inventory/orders/HA,
  waves, updater/identity and newer local work. Do not assume any is redundant.
- [ ] Prepare a small comparison kit for each candidate: copied or synthetic
  project/profile/database fixtures, exact application versions, setting mapping,
  entry-point instructions, expected results and a side-by-side result sheet.
  State what the user must inspect in stock Orca and Quack, including missing
  options and differing workflows. Run stock Orca without Quack patches/plugins;
  an additional plugin-assisted trial must be labeled separately.
- [ ] Test equivalent inputs in isolated profiles/credentials; record geometry,
  meaningful G-code, material assignments, migration/round trips and applicable
  workflow/error cases. Explicitly identify any unavoidable setting differences.
  Separate software findings from optional user-operated physical comparisons;
  this planning request authorizes no heating, dispatch or live HA changes.
- [ ] For each behavior propose `reuse_stock`, `reuse_with_thin_adapter`,
  `retain_custom`, or `retirement_candidate`, with benefits, losses, remaining
  gaps, migration/rollback and maintenance impact. Let the user reproduce the
  stock-Orca trial and record their choice, including accepting a documented
  workflow difference. No custom implementation is removed merely on an agent's
  equivalence claim; unresolved cases retain the current behavior.
- [ ] Feed accepted decisions into T1-T8 before porting those custom paths:
  preserve required outcomes through upstream implementations where proven,
  keep only necessary adapters, and test old custom settings/project migration.
  If custom-code retirement is subsequently authorized, remove it in a focused
  change with retained regression coverage and a tested rollback path.

## T1: Establish and review the integration candidate

**Files:** Both ledgers, `README.md`, `docs/WORKING_GUIDELINES.md`; all merge
conflicts as discovered. **Produces:** pinned merge candidate and resolved-path
record; no silent omissions from either tree.

- [ ] Review the design, refresh remote main/releases/open PRs and record any
  newer Quack baseline. If it changed, regenerate the path ledger and delta
  inventory; do not silently keep the old feature inventory.
- [ ] Use `superpowers:using-git-worktrees` to create an isolated
  `codex/integrate-orca-2.5.0-alpha` branch from the selected Quack SHA. Record
  the dirty primary tree and all other worktrees. Do not reset/pull them.
- [ ] Inventory local unpublished 3MF/model/arrangement changes and later HA
  audited-status/Print Again work by commit/file origin. Decide preservation or
  separate follow-up with evidence; never silently drop or declare it released.
- [ ] Capture the baseline native/HA test results and copied-project GUI behavior
  before the merge. Record unavailable infrastructure rather than claiming a pass.
- [ ] Merge the exact Alpha SHA with `--no-commit --no-ff` in that worktree.
  Review all 136 shared paths, including clean automatic merges. Never resolve
  all conflicts with blanket `ours`/`theirs`. Complete T2-T8 repairs before
  treating the candidate as qualified; keep checkpoints clearly labeled.
- [ ] Review source-tree differences beyond highlighted release notes and
  duplicated safety backports. Record each ledger disposition, run applicable
  checks, then commit a reviewable integration checkpoint.

## T2: Deliver a coherent dependency and packaging foundation

**Files:** `CMakeLists.txt`, `deps/CMakeLists.txt`, `deps/CGAL/`, `deps/OCCT/`,
tagged Python/solver dependencies, `build_win.bat`, `src/CMakeLists.txt`,
`.github/workflows/{build_deps,build_all,build_orca,unit_tests}.yml`, platform
packaging and `version.inc`. **Produces:** fresh per-architecture application
and dependency prefixes with plugin/CAD features installed.

- [ ] Reconcile the tagged dependency graph with SQLite, updater/signature and
  Quack resource requirements. Explicitly pin and package Python 3.12.13, uv,
  OCCT 8.0.1, CGAL 6.2.1 and SolveSpace; match `SLIC3R_CAD=ON` in both builds.
- [ ] Migrate obsolete Windows script calls to the tagged clang-cl/Ninja build
  path. Check Windows ARM64 OCCT compiler handling and architecture-specific wheels.
- [ ] Build dependencies and app from fresh directories, with tests enabled;
  never reuse old objects across changed class layouts/toolchains.
- [ ] Verify installed/portable apps launch with bundled Python/CAD and find
  resources in long, spaced and non-ASCII paths. Test test-app credential identity
  independently of `--datadir`; production credentials must remain untouched.
- [ ] Require matching platform builds, packaged-runtime smoke checks and
  test discovery before committing this repair set. Reconcile license notices.

## T3: Expose plugins, CAD, command search and updated UI

**Files:** `src/slic3r/plugin/`, `GUI/{MainFrame,GUI_App,Plater,SpeedDialDialog}.*`,
`GUI/CAD/`, `src/libslic3r/CAD/`, plugin resources, `tests/slic3rutils/` and
relevant upstream CAD/core tests. **Produces:** every tagged capability usable
alongside Quack management pages.

- [ ] Run upstream plugin/CAD tests first; add focused failing tests for any
  Quack namespace, permission, event or tab regression before fixing it.
- [ ] Exercise script/pipeline/printer-agent/pages/texture capabilities,
  install/update/reload/remove, disabled/missing dependencies, config overrides
  and 3MF reopen. Exceptions must be recoverable errors.
- [ ] Verify permission deny/revoke and exclusion of Quack config/tokens/secret
  stores. Preserve the upstream audit-hook protections under the Quack identity.
- [ ] Keep Filaments/orders and print controls visible with docked/plugin/CAD
  pages at compact window sizes. Exercise English/German Speed Dial, user
  shortcuts, text-input focus and access to existing Quack actions.
- [ ] Create/edit a constrained CAD part, text and revolve; transfer to plate,
  undo/redo, save/reopen and edit dimensions. Bake/reload is checked separately T4.
- [ ] Verify Publish 3MF, Home/cloud links and login/logout with explicit
  external-service evidence. Record service/identity failures as blockers.
- [ ] Run native plugin tests and isolated rendered GUI smoke checks; commit
  repairs and update all T3 coverage entries with evidence.

## T4: Integrate virtual materials, variants and colored geometry

**Files:** `FilamentMixer*`, `PrintConfig.*`, `PresetBundle.*`, `Model.*`,
`Format/{3mf,bbs_3mf}.*`, `GUI/{MixedFilamentDialog,TextureImportDialog,Tab,Plater}.*`,
texture gizmo/jobs, `GUI/FilamentReservationPlan.hpp`, `HaMaterial*.hpp`,
`HaProject*.hpp`, `GUI/Ha*.hpp`, native inventory/profile tests.
Paths without a prefix are under `src/libslic3r/`; `GUI/` is under `src/slic3r/`.
**Produces:** a reviewed logical-slot -> physical-UUID usage contract, consumed
by T5/T6/T7; preserve existing public interfaces where feasible.

- [ ] Add failing tests to `tests/libslic3r/test_filament_reservation_plan.cpp`
  and `test_ha_material_binding.cpp` for two/three-source mixes, repeated UUIDs,
  support/purge, ratio rounding and independent nozzle/flow contexts. Define
  units, rounding, provenance and conservation before modifying accounting.
- [ ] Expand logical mixed slots into the physical source rolls; aggregate stock
  and cost once per UUID. Derive support/purge and parallel-copy usage from the
  actual tagged pipeline statistics, not only nominal mix ratios.
- [ ] Preserve saved HA baselines, readable labels, unsaved local overrides and
  digest-pinned variant selection. An unresolved mapping must keep selections
  and offer correction; never silently choose another physical roll.
- [ ] Extend `test_3mf.cpp` / `test_model_filament_remapping.cpp` and upstream
  `tests/fff_print/test_mixed_filament.cpp` to cover grouped/painted imports,
  added/reordered/deleted source slots and texture-generated mixed colors.
- [ ] GUI-test glTF/GLB/FBX/OBJ and standard 3MF, mix/gradient/decomposition,
  texture projections/UV/layers/image adjustment/bake/undo/reopen with embedded
  images. Preserve projected paint behavior verified in T5.
- [ ] Verify H2D/X2D mixed sizes and H2C/A2L/general variants using suitable
  profiles and synthetic data. Actual device compatibility is a separate gate.
- [ ] Run native material/import tests plus HA profile/variant tests, review
  quantities and migration behavior, commit and update T4 ledger evidence.

## T5: Integrate slicing, towers, waves, belt, parallel modes and CLI

**Files:** `Print*`, `Layer*`, `Fill/`, `Support/`, `GCode*`, `GCode/`,
`WaveOverhangs/`, `MultiMaterialSegmentation.*`, `Belt*`, `IMEX*`,
`GUI/Jobs/ArrangeJob.*`, `GUI/GLCanvas3D.*`, CLI entry points and native tests.
**Consumes:** T4's physical-material accounting contract.

- [ ] Add failing regressions for Q6-Q10 in the owning suite. Create
  `tests/fff_print/test_wave_overhangs.cpp`, `test_prime_tower_by_object.cpp`
  and `test_color_penetration.cpp` if no existing subsystem tests cover them;
  list new files in suite CMake. Reuse upstream `fff_print/test_helpers.hpp`.
- [ ] Prove wave-disabled equivalence to unmodified Alpha for matched config;
  enabled tests cover anchor/coverage/fallback, floor layers and restoration of
  speed, temperature, fans and flow. Compare geometry/meaningful G-code, ignoring
  only documented volatile metadata.
- [ ] Verify per-object towers with new rib/sparse/rotation/tool ordering and
  collision placement; check `color_penetration_mode` inwards/projected,
  top/bottom depth, material painting and texture interaction.
- [ ] Re-run organic bottom-interface cases: independent Z gaps, material,
  contact extrusion and no collision with the model. Review upstream support,
  bridge, scarf, seam, infill, cooling and ZAA changes in the coverage ledger.
- [ ] Qualify belt transforms/brims/supports/purge/arrange/preview and IDEX/IQEX
  zones/copies, including stock totals from T4. Document/test prohibited feature
  combinations; unsupported Quack interactions remain release blockers.
- [ ] Exercise all tagged CLI additions, error cases, effective settings,
  default rotation changes, mixed colors, thumbnails and JSON statistics; check
  GUI/CLI setting equivalence and deterministic repeated slice/export results.
- [ ] Run complete core/FFF/filament-group suites and targeted calibration,
  render/performance checks. Commit reviewed changes and update T5 entries.

## T6: Preserve print reservation, preheat and agent dispatch

**Files:** `GUI/Jobs/{PrintJob,PrintJobPreheat}.*`, `GUI/HaJobPreheat.hpp`,
`src/libslic3r/HaJobPreheat.hpp`, `GUI/{SelectMachine,SendToPrinter,DeviceManager}.*`,
`src/slic3r/Utils/*Agent*`, HA `preheat*.py`; existing native/HA preheat tests.
**Consumes:** T4 physical allocations, T3 printer-agent/event lifecycle.

- [ ] Extend failing tests in `test_ha_job_preheat.cpp`,
  `test_print_job_preheat.cpp` and HA `test_job_preheat.py` for changed agent
  callbacks, duplicate events, disconnection and uncertain physical send.
- [ ] Preserve reserve -> owner-bound preheat -> fresh validation/claim ->
  one physical dispatch -> observed settlement. Plugin events must not create
  additional reservations, sends or settlement authorities.
- [ ] Recheck plate-only targets, manual takeover, target-already-met,
  below-target plateau, door invalidation, bed conditioning, cancellation,
  zero-target recovery, HA/app restart and two instances; retain the bounded
  additional 15-minute stabilization allowance and transport recovery contract.
- [ ] Exercise legacy HA without `job_preheat_v1` and opt-in behavior, failures
  restoring a usable send dialog, and no fallback/retry after uncertain claim.
- [ ] Verify Bambu 02.08 agents/AMS drying/colors/camera/files and non-Bambu
  host paths with mocks first; record live service/device acceptance separately.
- [ ] Run native and HA suites; no test may send heat or print commands to a
  physical printer. Commit and attach T6 evidence.

## T7: Qualify inventory, order and HA migration/accounting

**Files:** `FilamentInventory.*`, `GUI/FilamentInventoryService.*`, order/invoice
dialogs; HA `store.py`, `accounting.py`, `consumption.py`, `lifecycle.py`,
`provider.py`, `recovery.py`, profile/NFC/payment/incremental-sync modules.

- [ ] Start with copied schema-8 and supported older migration fixtures;
  failing tests cover mixed/parallel usage, repeated terminal events and unknown
  consumption. Any schema change requires additive migration plus restore tests.
- [ ] Run `test_filament_inventory*.cpp` and reservation tests; verify customer,
  order, archive/restore, costs, invoices, editable jobs and selection stability.
- [ ] Run HA backend and JS UI suites for stock/payment/NFC/QR, profile authority,
  CAS/full/delta/reset/offline cache, legacy capability fallback and recovery.
- [ ] Reconcile newer audited-display-status/Print Again work separately:
  keep observer identity, native stock lifecycle and settlement review even when
  displayed status changes. Never translate missing actual consumption to zero.
- [ ] Verify migration/backup/restore on copied data and rendered management UI;
  commit only after lifecycle totals and historical records are unchanged.

## T8: Qualify profiles, projects and translations

**Files:** `resources/profiles/`, profile schema/migration code, `Preset*`,
`Format/`, `localization/i18n/`, profile/gettext scripts and tests.

- [ ] Reconcile OFL one-ID changes and printer/nozzle compatibility with saved
  HA identities/variants. Test old and new 3MF/profile copies, local overrides,
  missing plugins, CAD history, texture assets and mixed material round trips.
- [ ] Retain all tagged new/changed printer/material profiles and OTA behavior;
  test-slice the shipped profile set with the upstream profile-check workflow.
- [ ] Verify Bambu 3MF output defaults, P2S/AMS 2 Pro/AMS HT mappings and bed/nozzle
  contexts without replacing the user's working profiles.
- [ ] Merge translation keys/contexts with Quack strings; obey root localization
  rules and glossary. Preserve placeholders/newlines and run gettext format
  checks; machine rewrites require `# AI Translated` comments.
- [ ] Test German/English UI and HA/web dialogs; document older-reader limitations
  and backup-based rollback. Commit and complete T8 ledger evidence.

## T9: Final cross-platform qualification and handoff

**Files:** Ledgers, README, release notes, test evidence and release workflows.

- [ ] Independently review the final diff, including automatically merged paths,
  every Q contract and all 351 release entries. No `planned`/`blocked` row may
  remain while claiming complete availability. Check unlisted tagged-tree changes.
- [ ] Review T0's comparison matrix and user trial results. Every retained or
  replaced custom behavior needs a recorded reason; any retired implementation
  needs the user's decision, migration/rollback evidence and outcome regressions.
- [ ] Run fresh full builds and CTest on the exact reviewed SHA across supported
  platforms. Use the target's documented test configuration; a typical configured
  native build runs `ctest --test-dir <build>/tests -C Release --output-on-failure`.
  Verify plugin Python tests, profile slices and packaging, not only C++ targets.
- [ ] Run HA checks matching `.github/workflows/ha-materials-tests.yml`:
  from `tools/ha-material-demo`, install `demo/requirements.txt` in an isolated
  environment, set `PYTHONPATH=demo`, run
  `python -m unittest discover -s demo/tests -p 'test_*.py'`, then each
  `demo/tests/test_*.cjs` with Node. Record exact versions/counts/failures/skips.
- [ ] Test delivered packages on clean profiles/credentials: startup, CAD/plugin
  runtime, data migration, representative old/new 3MFs, management UI, mock HA
  allocation/preheat/send errors and signed updater namespace. Compare startup,
  slice/export time and memory using identical projects/settings/hardware.
- [ ] Record external cloud/plugin/agent checks and user-operated hardware
  acceptance individually; no software-only result establishes P2S print quality.
  Belt/IDEX/IQEX hardware verification needs suitable devices or qualified testers.
- [ ] Prepare a Quack prerelease proposal with the preserved Q features, Alpha
  parity ledger, experimental labels, known limitations and backup/rollback steps.
  Obtain authorization for merge/publication if not already given in that future
  session; this document grants neither. Match reviewed head, CI, main build,
  tag/embedded versions and final asset hashes before any release claim.

## Progress and remaining decisions

- [x] Inspect official Alpha release and exact tag, current remote Quack source,
  divergence, shared paths, custom features and existing instructions.
- [x] Create coverage/shared-path ledgers and project agent instructions.
- [x] Record proposed architecture, task dependencies and acceptance criteria.
- [x] User authorized implementation, Beta target and full available compute.
- [ ] Execute T0-T9, including stock-Orca comparison trials and replacement decisions.
- [ ] Select the Quack prerelease version and qualification hardware when needed.

The historical planning snapshot above is superseded by the execution record.
The full Beta tree has now been integrated and built in an isolated Windows worktree.
See `docs/QUACK_BETA_LOCAL_TEST_2026-10-30_DE.md` for tested behavior and open gates. Every task should end in a focused reviewed commit and README update;
do not mix unrelated local fixes into the integration to make tests appear green.


## Execution authorization and Beta target

On 2026-10-30 the user authorized implementation and an isolated local test build, then explicitly selected OrcaSlicer 2.5.0 Beta (345efec64c7b95a05ce5cf96df0dde66a158835c, published 2026-10-27). This supersedes the earlier planning-only scope and Alpha implementation target. Historical Alpha audit counts remain labeled snapshots. Keep the Alpha coverage ledger and additionally evaluate the Beta ledger and regenerated shared-path ledger. No production installation, live HA deployment, physical print/heating or release publication is authorized. No custom feature is retired without its comparison and user decision.

## Local execution result

The isolated Beta merge, matching dependency/application build, native tests,
HA Python/JS checks, gettext catalog compilation and an offline copied-profile
package are complete. Two independent-review findings were reproduced RED and
fixed GREEN. Preserve all unresolved custom behavior.
T0 stock-replacement user decisions and T9 cross-platform/cloud/hardware/full-
individual-feature qualification remain open; this is a local trial handoff,
not an assertion that every acceptance checkbox above passed.
