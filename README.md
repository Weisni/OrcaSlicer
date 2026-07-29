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
