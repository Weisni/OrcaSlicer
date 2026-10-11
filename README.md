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
- The latest direct-EXE upload test was interrupted after startup did not expose
  a window. Native regression/build checks passed; live upload acceptance and
  clean cross-platform release CI remain open. The PR is prepared as a draft.

- Release-preparation checks: 258 HA Python tests, bundled browser tests,
  focused native contract/DNS/transport/inventory/profile suites, 29 loading
  checks and 10 name-search checks passed. Independent review identified and
  corrected internal cache/project presets in the saved-template chooser.
