<div align="center">

  <img src="QuackSlicer/QuackSlicer_Wordmark.png" alt="QuackSlicer" width="520">
  <h1>QuackSlicer</h1>
  <p>An independent fork of <a href="https://github.com/OrcaSlicer/OrcaSlicer">OrcaSlicer</a></p>
</div>

> [!IMPORTANT]
> QuackSlicer is an independent project. It is not the official OrcaSlicer
> application and is not maintained, endorsed, or supported by the OrcaSlicer
> team. References to OrcaSlicer in this repository identify the upstream
> project and acknowledge the software on which this fork is based.

## Why this fork exists

QuackSlicer keeps OrcaSlicer's proven slicing foundation while adding features
for workflows that are specific to this project. Maintaining these changes in a
fork allows them to evolve independently without presenting them as part of the
official OrcaSlicer product.

The main areas developed by this fork include:

- filament and spool inventory management;
- customer and order management;
- print-job tracking and material assignment;
- QuackSlicer-specific branding, packaging, and update channels;
- project-specific workflow and slicing experiments.

The aim is to stay compatible with OrcaSlicer projects and profiles wherever
practical while periodically integrating relevant upstream improvements.
QuackSlicer may nevertheless differ from the corresponding OrcaSlicer release,
so important projects and configuration should be backed up before switching
between applications.

## Relationship to OrcaSlicer

Most of QuackSlicer's slicer engine, printer and filament profile ecosystem,
calibration tools, and cross-platform application framework originate from
OrcaSlicer. Unless a feature is explicitly documented as QuackSlicer-specific,
the [OrcaSlicer Wiki](https://www.orcaslicer.com/wiki) remains the best general
reference for slicer operation and settings.

Upstream changes are integrated deliberately rather than assumed to be present
immediately. A QuackSlicer version number therefore does not by itself guarantee
feature parity with an OrcaSlicer release carrying a similar number.

## Project links

| Resource | QuackSlicer fork | OrcaSlicer upstream |
| --- | --- | --- |
| Source code | [Weisni/OrcaSlicer](https://github.com/Weisni/OrcaSlicer) | [OrcaSlicer/OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) |
| Downloads | [QuackSlicer releases](https://github.com/Weisni/OrcaSlicer/releases) | [Official OrcaSlicer releases](https://github.com/OrcaSlicer/OrcaSlicer/releases) |
| Issues | [QuackSlicer issues](https://github.com/Weisni/OrcaSlicer/issues) | [OrcaSlicer issues](https://github.com/OrcaSlicer/OrcaSlicer/issues) |
| Documentation | This repository and release notes | [OrcaSlicer Wiki](https://www.orcaslicer.com/wiki) |
| Website | — | [orcaslicer.com](https://www.orcaslicer.com/) |

Use the QuackSlicer issue tracker for behavior found in QuackSlicer builds,
especially for inventory, order, print-job, branding, update, or other
fork-specific functionality. If an issue can also be reproduced in an
unmodified current OrcaSlicer build, it may instead belong in the upstream issue
tracker. Please do not ask the OrcaSlicer maintainers to support QuackSlicer-only
changes.

## Downloading QuackSlicer

Prebuilt versions published by this fork are available from the
[QuackSlicer releases page](https://github.com/Weisni/OrcaSlicer/releases).
Assets from the [official OrcaSlicer releases
page](https://github.com/OrcaSlicer/OrcaSlicer/releases) install OrcaSlicer, not
QuackSlicer.

Only download binaries from a release page you trust. The two projects have
separate maintainers and release channels.

## Development progress and open tasks

### QuackSlicer 2.5.25 release validation

The user authorized release publication after final validation. The candidate
integrates the official Orca 2.5.0 Beta while retaining Quack workflows. The
regular release remains gated until the exact source commit, full hosted build
matrix and final package checks pass.

- [x] Repeat 436 HA Python tests, 11 JavaScript test files and test-profile
  credential/endpoint regressions.
- [x] Exercise STL/3MF CLI slicing, standard-3MF material roundtrips and GUI
  STEP import, automatic arrangement and slicing in separate project copies.
- [x] Review the remaining 52 shared merge paths and seven critical integration
  paths; fix partial Wave Overhang coverage removing uncovered tree-support areas.
- [x] Check A4 on physical filament 1, A3 on 2 and the virtual mix on 3 after
  restart, with no outgoing HA profile/color differences; retain their bindings
  and the 50/50 mix through a 0.4/0.6/0.4 nozzle roundtrip.
- [x] Correct platform application identities and strengthen release matrix,
  tag/source and asset-digest checks.
- [x] At local snapshot `c14cbf0735`, build regular 2.5.25 and pass all 3,090 native registrations (nine environment
  skips), check 52 changed C++ files with Clang-tidy and restart
  the updated local runtime with A4/A3 and the virtual mix intact.
- [x] Verify HA multi-printer rejection in the isolated GUI and queue/dispatch
  policy tests, retaining parallel native sends when HA is off.
- [x] Requalify the additional local multi-printer tracking opt-out: default to
  cancellation, retain per-task consent and reject passive preheat before dispatch.
  Pass 90 policy assertions and confirm the passive-preheat GUI rejection.
- [x] Reproduce the platform Clang failure on two retained pre-cooling fields,
  annotate only those fields and rebuild at `c14cbf0735`; repeat the complete
  native suite, matched CLI slicing and the fresh HA comparison after restart.
- [x] Remove three genuinely unused lambda captures after a second platform
  compiler failure. Check all 32 changed translation units in full with both
  unused-capture and unused-private-field warnings treated as errors.
- [x] Extend that warning/syntax check to all 127 compiled Quack translation
  units differing from official Beta. Correct three further unused captures,
  retain four unused object-color fields with targeted attributes, add direct
  headers and pass the project Clang-tidy check on all 58 changed C++ files.
- [x] Reproduce the generated Git metadata/PCH fallback collision and verify
  authoritative commit labels with both Clang and an actual PCH build under
  warnings-as-errors, preserving the legacy CMake path and crash-report consumer.
- [x] Make the two existing cut-connector copy assignments explicitly defaulted
  for current Clang, preserving their memberwise semantics. Repeat the complete
  127-unit warning check with this diagnostic and pass Clang-tidy on 59 files.
- [x] Extend the strict warning check to all 739 compiled application translation
  units. Correct the CAD Camera forward declaration and retain two unused CAD
  edge-body slots with targeted attributes; the repeated four-diagnostic sweep
  passes all 739 units. Clang-tidy passes on 61 changed files.
- [x] Correct a C++17 structured-binding capture in the physical-spool check,
  retaining the same synchronous reference and stock calculation. Reproduce the
  error before the fix, check all 739 units with the broader local CI warning
  scope and pass Clang-tidy on 62 changed files. Compiler-version-specific
  extra diagnostics and their unchanged-policy reruns are recorded in the report.
- [ ] Pass the complete hosted platform CI and verify the final release assets.

See the [German acceptance report](docs/RELEASE_ACCEPTANCE_2.5.25_DE.md)
and [candidate release notes](docs/releases/2.5.25.md). Physical printer,
external-service and platform runtime acceptance remain explicitly separate;
these local checks do not establish exhaustive upstream feature parity.

### HA incremental synchronization (implementation checkout)

HA owns committed material profiles and roll associations. Generation/revision
checks skip unchanged inventories; stable-key row deltas update only changed
SQLite rows. Immutable profile contents are cached persistently by digest and
preloaded in a background worker. Equal effective settings reuse one managed
profile regardless of roll or display label.

Local unsaved material adjustments remain editable. Background refresh preserves
them; selecting a new HA base retains the changed settings over that base.
Initial profile association uploads the saved baseline first and adopts only
the acknowledged HA profile. Temporary edits remain local; explicit
Project-to-HA publication includes them. Ordinary edits do not save a project.

- [x] Implement authenticated incremental reads, immutable content and legacy API fallback.
- [x] Preserve atomic import, receipts, conflict checks and temporary local overrides.
- [x] Verify focused native contracts, profile integration and the component suite.
- [x] Compile the changed GUI translation units and independently review the flow.
- [x] Correct saved-profile identity checks and unchanged base-profile comparisons;
  verify 310 assertions in 31 native profile/binding tests.
- [x] Label missing full profiles as optional initial storage, unchecked by default.
- [x] Build the isolated Windows test runtime and verify loading its new DLL.
- [x] Add profile-name search and preserve shared temporary settings when a single
  project filament switches to its saved HA profile; preflight before one batch upload.
- [x] Include the standard-3MF object-default correction in the runtime core and
  verify all 45 objects in the Whinny Pooh Ears input, including a replaced palette.
- [ ] Validate the isolated Windows runtime for the profile comparison correction.
- [ ] Verify rendered selection, offline restart and refresh with no connected printer.
- [ ] Run complete cross-platform release builds before installation/publication.

Synthetic 1,000-customer data transferred about 1.09 MB through 16 legacy
requests; one changed row used about 4 KB in one request and an unchanged read
used 123 bytes. These measure local transfer volume, not live HA latency.
See [HA materials](docs/HOME_ASSISTANT_MATERIALS.md) and the
[implementation record](docs/superpowers/plans/2026-10-09-ha-incremental-sync.md).
The [native 3MF verification](docs/STANDARD_3MF_NATIVE_IMPORT_VERIFICATION.md)
records the input, mapping and annotation-reset regressions.

### QuackSlicer 2.5.21

Main-thread job callbacks now deliver failures to the job finalizer through
their futures. An HA rejection such as `Another dispatch requires completion
or review` is reported by the existing print-dialog error handler instead of
escaping into the UI event loop. Preparation failures also stop the job before
further work begins. See the [release notes](docs/releases/2.5.21.md).

- [x] Preserve the original callback exception and verify a subsequent job can run.
- [x] Retrieve preparation futures for print, send, arrange, orient and fill-bed jobs.
- [x] Reproduce both worker regressions before the correction; pass all 18 assertions
  with the correction, the GUI utility suite and HA/inventory regressions.
- [x] Verify the local Windows repair starts with an isolated data directory and
  preserve existing user configuration and profiles during installation.
- [ ] Verify an actual accepted HA print dispatch and physical printer behavior.
- [ ] Diagnose the separately reported missing nozzle-match indicator.

Cross-platform builds, unit tests and matching final artifacts gate release
publication. Managed `HA ... [digest]` filament presets remain the effective
local copies used by the HA profile integration.

### QuackSlicer 2.5.20

The optional Home Assistant material source supplies real roll UUIDs, current
stock, colors and filament profiles through the existing filament dropdown.
Mounted rolls appear by slot; other usable rolls appear under Inventory.
Local profile overrides retain the physical roll identity. Explicit
synchronization transfers selected profile/color changes and can import all
occupied HA slots while preserving existing project material indices.

HA remains the writable inventory authority. Changes use row/field conflict
checks, durable retry receipts, and paged snapshots; interrupted reads preserve
the previous cache. Printing retains the native printer connection and checks
the selected rolls and their live slot mapping before dispatch.

See the [release notes](docs/releases/2.5.20.md),
[setup, compatibility and recovery](docs/HOME_ASSISTANT_MATERIALS.md) and the
[Home Assistant component](tools/ha-material-demo/README.md).

- [x] Implement UUID-bound selection, explicit complete-profile synchronization,
  and HA-backed inventory/job bookkeeping.
- [x] Add regressions for large history imports, conflicts, retry receipts,
  profile compatibility and printer mapping.
- [ ] Complete cross-platform CI and verify the release assets for this revision.
- [ ] Complete physical iPhone NFC writing, cold-start scans and two-phone testing.

### QuackSlicer 2.5.19

This maintenance release backports three focused OrcaSlicer safety fixes while
the official stable baselines remain OrcaSlicer 2.4.2 and Bambu Studio 2.8.2.61.
See the [release notes](docs/releases/2.5.19.md) for compatibility, validation,
and the explicit auto-backup compromise.

- [x] Bound ASCII STL solid-name and MakerWorld metadata parsing.
- [x] Prevent CGAL model repair from racing the auto-backup worker.
- [x] Keep per-variant configuration migration within source and destination
  row bounds when switching printers.
- [x] Complete the capped-load Windows Release build, all 532 runnable CTest
  cases (five optional Python host tests skipped), and profile validation for
  all 1,009 printer presets.
- [ ] Complete cross-platform CI and verify the published release assets.

### QuackSlicer 2.5.18

This release fixes missing organic bottom support interfaces and interfaces
placed inside the model. It also respects the configured bottom Z gap when
the top gap is zero. Existing project files and profiles remain compatible.
See the [release notes](docs/releases/2.5.18.md) for behavior and validation.

- [x] Correct bottom contact extrusion, collision placement, and gap handling.
- [x] Verify 11 support cases / 660 assertions and 529 passing CTest cases
  (five NumPy-dependent tests skipped in the local test interpreter).
- [x] Re-slice the reported PETG/PLA project and check lower contact geometry;
  the user confirmed that the reported problem is resolved.
- [ ] Complete the cross-platform release build and verify published assets.
- [ ] Validate physical adhesion, removal, and final print quality separately.

### QuackSlicer 2.5.17

This release adds reversible order archiving and a redesigned order workspace.
See the [release notes](docs/releases/2.5.17.md) for changes, upgrade information,
and validation coverage. The release pipeline builds and tests the supported
platforms before publishing the matching binaries.

### Windows startup recovery (2026-09-26)

The local startup crash was traced to incompatible class layouts in stale
Release object files: the 3D-view caller allocated `0x3280` bytes while its newer
constructor wrote beyond that size. The initial NVIDIA/heap stack identified
where corruption was detected, not its source. See the
[diagnostic and recovery record](docs/WINDOWS_STARTUP_DIAGNOSTIC.md).

- [x] Capture the first invalid write using an isolated executable/profile.
- [x] Remove temporary verifier settings and separate project headers from external includes.
- [x] Finish a consistent rebuild and verify normal GUI startup and project-copy loading before the arrangement rollback.

### Arrangement extension withdrawn (2026-09-26)

At the user's request, the additional adaptive/grid arrangement feature,
preview dialog, and its dedicated tests/fixture were removed. The existing
built-in arrangement implementation and controls are restored. Order archiving,
management UI improvements, and the startup/build correction are retained.
The original Bottle Thread Game 3MF was not edited.

- [x] Restore the original arrangement sources and tests.
- [x] Rebuild the local Release application; all 59 arrangement/inventory tests
  passed (744 assertions). An isolated startup check reached the responsive
  main window and initialized the 3D canvas, then shut down normally.

### Order archiving and management UI (2026-09-24)

Completed and cancelled customer orders can now be archived and restored in
**Filaments > Orders**. The order filter offers **Not archived**,
**Archived**, and **All orders**; search matches order numbers, titles, notes,
and customer names. Archiving preserves status, print jobs, costs, and invoice
details. Restore an order before editing it or its historical print jobs.

- [x] Add reversible order archiving and an additive inventory schema 7-to-8 migration.
- [x] Add archive filtering, search, empty-state guidance, and state-aware actions.
- [x] Preserve customer and print-job selection during refresh.
- [x] Wrap management toolbars and make customer, order, cost, and invoice forms scrollable.
- [x] Review inventory, tracking, allocation, billing, settings, and navigation workflows.
- [x] Build the Windows Release app and pass 49 inventory/lifecycle tests (695 assertions).
- [x] Verify order sorting/filtering and archive/restore in the Windows GUI with synthetic data.
- [ ] Complete the remaining management dialog, keyboard, and export checks listed in the UI review.
- [ ] Validate layouts on macOS/Linux and with additional DPI and language settings.

The [UI review and verification notes](docs/ORDER_ARCHIVE_UI_REVIEW.md) track
coverage, remaining improvements, and compatibility boundaries.

### Order workspace redesign (2026-09-26)

Orders and customers now have separate tabs. Search and status/archive filters
sit directly above the order table; selection-specific actions are grouped below
it. Status badges lead each row. The default order is Active, Draft, Completed,
Cancelled, then the displayed order name alphabetically, ignoring case.

Eight primary columns cover progress and totals. **Cost details** reveals the
remaining weight and cost columns without losing data. Buttons use the existing
application style, and wrapping toolbars no longer stretch their last action.

- [x] Separate customer administration from daily order work.
- [x] Add status-first sorting, a status filter, badges, and optional cost details.
- [x] Build and verify the rendered layout in light/dark themes, filtering,
  selection, and archive/restore. All 49 inventory tests (695 assertions) pass.
- [ ] Validate other platforms, increased DPI, and translated labels.

## Building from source

QuackSlicer retains OrcaSlicer's CMake-based, cross-platform build system. The
upstream [compilation
guide](https://github.com/OrcaSlicer/OrcaSlicer/wiki/How-to-build) provides the
required dependency and platform setup and is the starting point for building
this fork.

After preparing the dependencies and build tree, the usual build commands are:

```bash
# macOS
cmake --build build/arm64 --config RelWithDebInfo --target all --

# Linux
cmake --build build --config RelWithDebInfo --target all --

# Windows, run from the configured build directory
cmake --build . --config Release --target ALL_BUILD -- -m
```

Fork-specific build or packaging behavior is defined in this repository and may
differ from the upstream guide.

## Project lineage and acknowledgements

QuackSlicer would not exist without the work of the OrcaSlicer maintainers and
contributors. OrcaSlicer itself builds on a long open-source slicer lineage:

1. [Slic3r](https://github.com/slic3r/Slic3r)
2. [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer)
3. [Bambu Studio](https://github.com/bambulab/BambuStudio)
4. [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer)
5. QuackSlicer

Please support and credit the upstream projects when using or redistributing
this work.

## License

QuackSlicer is distributed under the GNU Affero General Public License,
version 3. See [LICENSE.txt](LICENSE.txt) for the license text.

The repository contains work inherited from OrcaSlicer and its upstream
projects as well as QuackSlicer-specific changes. Existing copyright, license,
and third-party attribution notices remain applicable to their respective
components.

### HA material editing follow-up (2026-10-10)

- Investigated per-request loading-window flicker and internal hashes in Save preset.
- Identified broad preset replacement and a shared edit buffer as causes of cross-filament changes.
- Completed: one loading window, readable unique save names/editor labels, and isolated per-filament HA edits in project memory.
- Verified: scoped Windows runtime rebuild; 1,727 assertions in 131 cases; 27 native network/UI checks, including callback deferral during dialog destruction.
- Live acceptance: ELEGOO black/SUNLU blue local edits and Save as affect only the selected filament. A2/A3/A2 spool switches restore the correct names and material values; unchanged profiles produce an empty Project-to-HA comparison.
- Database revision 6811 and all ten table hashes remain unchanged after the direct-EXE live tests. Material completion observations were 1.46-2.73 seconds including native automation/screenshot overhead.
- Remaining coverage: macOS/Linux, additional DPI/languages, and project save/reopen with HA profile caches absent. The earlier grouped 3MF import acceptance was not repeated in this material-editing follow-up.

### HA profile upload DNS repair (2026-10-10)

- Reproduced the SUNLU A1 profile-choice failure before its upload: Windows
  name resolution occasionally exceeded the eight-second connection deadline.
- HA requests now share a thread-safe DNS cache, allow bounded cold resolution,
  and report the concrete transport error while preserving uncertain writes.
- Local DNS regression failed before the fix and passed afterward. Live reads
  measured 11-12 seconds cold, then 0.24-0.28 seconds with DNS reuse.
- Direct-EXE live acceptance: SUNLU PLA Matte @System was published for A1,
  HA revision 6823 to 6824. All other rolls and physical slots remained unchanged.
- A1/A3/A1 switching restored correct profile names, settings, and colors with
  no synchronization errors. Completion snapshots: 2.23 seconds for profile
  publication and 0.91/0.94 seconds for switching, including automation overhead.
- A1 and A3 produced no Project-to-HA differences. The existing Filament 2
  PETG roll genuinely lacks a complete HA profile for this nozzle context;
  its unchecked save offer and its project binding were preserved.
- Verified: 1,727 assertions in 131 cases, two configured CTest targets, and
  27 native network/UI checks. The application remains open with A1 selected.
- Remaining: cross-platform runtime and release validation; the grouped
  3MF and local-edit acceptance were not repeated in this DNS follow-up.

### HA upload loading-window continuity (2026-10-11)

- Project-to-HA upload now groups review refresh, publication, read-back and
  profile activation in one loading sequence. Nested publication reuses it.
- Parent windows remain disabled between requests for the whole visible
  sequence; genuine user-choice dialogs release the parent safely.
- Native regression reproduced premature parent re-enabling before the fix.
  Afterward, 29 native checks passed, including exactly one show/hide across
  three requests, parent lifetime, paint, cancellation and callback deferral.
- Both configured HA CTest targets passed. Scoped Windows runtime build and
  direct-EXE live upload acceptance are recorded in the local diagnostics.
- Remaining: macOS/Linux runtime and release acceptance; this follow-up does
  not repeat grouped 3MF or per-filament Save-as tests.

### QuackSlicer 2.5.22 release preparation (2026-10-11)

- Prepared the accumulated HA/profile/import fixes against current main and
  incremented the source version to 2.5.22.
- Scope, upgrade compatibility and open release gates are documented in
  [release preparation](docs/RELEASE_PREPARATION_2.5.22.md).
- Direct-EXE live tests confirmed profile upload, readable names and empty
  comparisons after publication; unrelated rolls and physical slots were
  unchanged. Windows focus restoration now has 34 passing native checks.
  Final live focus verification and clean CI on the final revision remain
  release gates. See the PR verification results before publication.

- Release-preparation checks: 258 HA Python tests, bundled browser tests,
  focused native contract/DNS/transport/inventory/profile suites, 34 loading
  checks and 10 name-search checks passed. Independent review identified and
  corrected internal cache/project presets in the saved-template chooser.

## QuackSlicer 2.5.23: project-load crash correction

- Fixed the deferred printer-tab refresh after importing a 3MF project.
- Keep the printer settings already loaded from the project instead of selecting the preset again.
- Verification and remaining release checks: [project-load crash report](docs/PROJECT_LOAD_CRASH_FIX.md).


## QuackSlicer 2.5.24: HA preheat and project material recovery

This release selects chamber and bed temperatures from the filaments actually
used by the submitted plate. Home Assistant can own a bounded preheat operation
for the reserved print attempt, including atomic takeover of manual advance
warming. Fresh safety/temperature checks remain mandatory before dispatch.
An already reached chamber target bypasses plateau waiting; otherwise a proven
plateau can release the chamber wait. Connection recovery never repeats a
physical print dispatch. Reopened projects retain readable HA material names
and their existing local material settings. Send errors restore the preparation
screen while preserving any unresolved reservation or dispatch outcome.

The existing local preheat path remains available for integrations without the
job-preheat capability. See [release details](docs/releases/2.5.24.md) for setup,
validation, compatibility and remaining physical acceptance work.


### OrcaSlicer 2.5.0 Beta local integration (2026-10-30)

The full official Beta tag `345efec64c7b95a05ce5cf96df0dde66a158835c` is integrated
into an isolated branch based on remote Quack 2.5.24. The user selected Beta over
the originally audited Alpha. The Windows x64 Release build includes CAD and the
bundled Python/plugin runtime, while preserving Quack inventory/orders/HA,
Projected and surface-normal paint, Wave Overhangs and per-object towers.

- [x] Preserve root [agent instructions](AGENTS.md) and [working guidelines](docs/WORKING_GUIDELINES.md).
- [x] Resolve the tagged merge and build fresh matching dependencies/application.
- [x] Run native CTest: 3,044 registered, zero failures, nine environment skips.
- [x] Run 436 HA tests on bundled Python 3.12.13 and 11 JavaScript UI test files.
- [x] Compile/check all gettext catalogs; create an isolated offline profile and database copy.
- [x] Independent review: fix cloud credential namespace and plugin database-access boundaries.
- [x] Compare a common P2S/PLA CLI slice with official Beta: 4,009 identical movement commands,
  equal calculated 2.40 g consumption; this is one baseline, not broad equivalence proof.
- [ ] Complete individual stock-replacement user trials and record the user's decisions.
- [ ] Qualify every feature interaction, all platform packages, external services and hardware
  before any release. Unpublished primary-checkout changes remain separate.

Start the local package with `build-test/QuackSlicer-Beta-Test/Start-Quack-Beta-Test.cmd`
in the integration worktree. It uses `build-test/test-profile`, its own credential
service, no production installer/associations, and disabled HA/preheat/sync by default.
The package is a local Windows test build, not a release-qualified installer.

See the [German test handoff](docs/QUACK_BETA_LOCAL_TEST_2026-10-30_DE.md),
[stock-Orca trial guide](docs/ORCA_STANDARD_COMPARISON_TRIALS_DE.md),
[comparison matrix](docs/upstream/quack-orca-standard-comparison.md),
[Beta coverage ledger](docs/upstream/orca-2.5.0-beta-coverage.csv) and
[implementation plan](docs/superpowers/plans/2026-10-30-orca-alpha-integration.md).

#### HA connection correction (2026-10-30)

- [x] Diagnose the local test profile's omitted HA address: the dialog fell back to
  loopback port 8765, whose unrelated local service returned HTTP 404.
- [x] Retain credential-free HA endpoint URLs when preparing future test profiles;
  preserve disabled-by-default networking and isolated credentials. Two Python
  regression tests pass, including rejection of credentials/query tokens in URLs.
- [x] Correct the running test profile to the saved Home Assistant server address.
- [x] Confirm authenticated read-only HA access using the saved test credential.
  Production credentials were not copied; GUI import acceptance is tracked below.

#### HA legacy material compatibility correction (2026-10-30)

- [x] Accept the exact 149-setting Quack 2.5.24 material schema alongside Beta's
  151-setting schema. The two absent toolchange overrides inherit printer values
  (`nil`); unrelated missing or unknown settings remain rejected.
- [x] Preserve original payload hashes, cached content and unchanged legacy profile
  identities; explicit Beta overrides remain part of the synchronized content.
- [x] Reproduce the decode/cache failures before the fix; pass 25 focused cases
  with 299 assertions and the complete 3,047-test registration (zero failures,
  nine environment skips). Independent review found no blocking issue.
- [x] Rebuild and update the local runtime; save/reopen the active test project.
- [x] Confirm authenticated roster display after the import compatibility fixes below.

Correction: credential metadata confirms that the token was saved. The manual
restart changed the data-path spelling (`/` versus `\`), which changes the current
credential service hash. The local starter now consistently uses the original
absolute `/` spelling. The acceptance fixes below now normalize directory identity
and migrate matching legacy keys; the previous session-only explanation was incorrect.

#### HA unknown-consumption import correction (2026-10-30)

- [x] Diagnose the graph-integrity failure with a read-only live snapshot: one of
  459 allocations omits its unknown actual weight and material cost. SQLite
  exports these fields as null; all other supplied fields round-trip unchanged.
- [x] Normalize only these two optional allocation fields to null before import
  and integrity hashing. Preserve measured values, atomic rollback, stock and
  reservation checks; reject unrelated incomplete rows.
- [x] Reproduce the original error in a regression test; pass 19 import/transport
  cases with 102 assertions, including removal of stale actual values.
- [x] Preserve HA snapshot vector lengths when reloading root filament files:
  ordinary preset variant expansion otherwise changes their content identity.
  Reload regressions cover legacy/current profiles with Standard/High Flow variants.
- [x] Pass 44 focused cases with 406 assertions; independent review found no
  blocking issue. Runtime displays all six mounted slots and opens the sync
  chooser without either previous error, using the saved credential automatically.
- [x] Complete the final full native regression run after the preset-loader fix:
  3,049 registered tests, zero failures, nine environment skips.

#### HA workflow and native nozzle controls acceptance (2026-10-30)

- [x] Remove the competing Quack flow selector from the Variant card. The native
  Beta nozzle control owns concrete extruder flow and plate mapping, retaining
  connected-printer hardware restrictions.
- [x] Exercise mounted-roll dropdown selection, batch import of equal profiles
  with different roll UUIDs, and Standard/High Flow plus 0.4/0.6 mm changes in GUI.
- [x] Preserve genuine local overrides while treating verified native broadcast
  expansion as unchanged. Correct short nullable-value getters and transfer-dialog ordering.
- [x] Exercise explicit profile publication, HTTP 409, a lost successful response
  and idempotent retry against the local synthetic HA service. Stock stays unchanged.
- [x] Verify saved credentials after restart with a backslash/parent-segment data
  path; read all six live mounted slots without entering a token or publishing profiles.
- [x] Restore readable existing-binding names from verified offline cache without
  authorizing selections or dispatch from an expired roster.
- [x] Run 3,063 native registrations with zero failures and nine environment skips,
  436 HA Python tests, 11 JS test files and clang-tidy on 26 changed C++ files.
- [ ] Live HA profile-upload acceptance and physical printer/AMS/preheat acceptance
  remain separate; this task uses read-only live HA and synthetic local writes.

Detailed observed chains, evidence and limits: [German HA acceptance report](docs/HA_BETA_ACCEPTANCE_2026-10-30_DE.md).

#### Mixed-filament preservation on nozzle changes (2026-10-30)

- [x] Keep all seven project mix arrays when the printer/nozzle switch preserves
  project slots; do not replace unsaved definitions with the target printer snapshot.
- [x] Preserve normal per-printer snapshot restoration when project slots are not retained.
- [x] Reproduce the reset in a native regression, then pass 28 mixer/color cases
  with 673 assertions and all 3,064 native registrations (zero failures, nine skips).
- [x] Verify GUI changes 0.2 -> 0.4 -> 0.6 -> 0.2 mm retain the recovered 90/10 mix,
  including a target variant that previously stored a different mix; save/read back the 3MF.
- [x] Update the local Release runtime; clang-tidy passes on 28 changed C++ files.

The pre-fix user state and recovered 90/10 and 67/33 definitions are separate
project copies. See the [updated local handoff](docs/QUACK_BETA_LOCAL_TEST_2026-10-30_DE.md).

#### Physical HA slots and virtual mixed colors (2026-10-30)

- [x] Reproduce A3 sharing logical slot 2 with a self-referencing mixture;
  keep physical roll colors separate from computed mixed colors.
- [x] Reject virtual HA import targets and exclude virtual slots from uploads;
  normalize appended physical slots before mixed slots, including material,
  support, painted-face, plate sequence and tool-change references.
- [x] Clear missing mixed metadata when loading ordinary projects and preserve
  the physical/mixed boundary across printer snapshot changes.
- [x] Pass 77 focused cases / 962 assertions, all 3,077 native registrations
  (zero failures, nine environment skips), and clang-tidy on 35 tracked plus
  two new C++ files. Build and stage the Release runtime.
- [x] Reselect live A3 read-only, recreate the 50/50 mixture as slot 3, save and
  restart: A3 stays white and has no outgoing color difference.
- [x] Fix A3's remaining restart profile difference by expanding per-profile
  variant broadcasts before packing and verifying unchanged managed baselines
  on load. Preserve real local overrides and distinct nozzle temperatures.
- [x] Pass 79 focused cases / 1,241 assertions, all 3,078 native registrations
  (zero failures, nine environment skips), and clang-tidy on 35 tracked C++
  files. Verify HA import, save, restart and outgoing comparison in the GUI.
- [x] Retain the user's explicit A4 choice for physical slot 1 and A3 for slot 2;
  reopen the saved copy and verify no outgoing profile/color differences or
  initial-profile warning. Preserve the unbound virtual 50/50 mix on slot 3.
- [x] Label virtual mixtures as `Mixed Filament 3` in both material context menus.
  Reject initial profile publication when the project/physical roll material
  families disagree; show an assignment error instead of an initial-save hint.
- [x] Build and stage the updated runtime; pass 80 focused cases / 1,244 assertions,
  all 3,079 native registrations (zero failures, nine skips), clang-tidy on 36
  tracked C++ files and extraction of the new UI strings.
