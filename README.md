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
