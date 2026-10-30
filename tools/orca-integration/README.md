# Local Orca/Quack integration tools

These helpers create synthetic comparison inputs and an isolated offline test profile.
They do not send print jobs, change printer settings or publish a release.

## Local Windows build

Use an x64 Visual Studio developer shell (this build used VS 2026/MSVC 14.51),
CMake and Ninja. Configure matching CAD support in both dependency and app builds:

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = [Environment]::ProcessorCount
cmake -S deps -B build-test/deps -G Ninja -DCMAKE_BUILD_TYPE=Release -DSLIC3R_CAD=ON
cmake --build build-test/deps --parallel 1
$prefix = (Resolve-Path build-test/deps/OrcaSlicer_dep/usr/local).Path.Replace('\','/')
cmake -S . -B build-test/app -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix" -DSLIC3R_CAD=ON -DBUILD_TESTS=ON -DQUACK_LOCAL_TEST=ON -DSLIC3R_MSVC_PDB=OFF
cmake --build build-test/app --parallel 12
ctest --test-dir build-test/app/tests -C Release --parallel 6 --output-on-failure
cmd /c scripts\run_gettext.bat
cmake --install build-test/app --config Release --prefix build-test/QuackSlicer-Beta-Test/runtime
```

Dependency subbuilds use all 32 logical threads on the test machine. App compilation
uses 12 workers within its 31GB RAM; increasing workers until memory is exhausted
would reduce throughput. CTest numeric IDs may change on relink: use stable names,
tags or a fresh full run rather than a stale `--rerun-failed` list.

## Synthetic inputs and safe profile copy

```powershell
python tools/orca-integration/create_comparison_fixtures.py --output build-test/comparison
python tools/orca-integration/prepare_test_profile.py --source "$env:APPDATA/QuackSlicer" --output build-test/test-profile
```

The profile output must not exist. The source configuration is read only; SQLite's
backup API reads the committed source state through a read-only connection.
Only allowlisted preferences and saved JSON presets are copied, credential fields
are removed, and HA/preheat/network sync start disabled. The credential-free saved
HA endpoint is retained so the dialog does not silently fall back to the local demo
port; authentication must be configured separately for the test profile. Presets whose parents
exist only in an un-copied cloud/HA context may require explicit reselection.
Never point a trial app at the production data directory.

`console_probe.cpp` is an optional Windows console entry point next to an unchanged
OrcaSlicer.dll. Compile with `cl /EHsc /std:c++17 console_probe.cpp /Feorca-console.exe`
in a developer shell and place the EXE beside the DLL and its runtime dependencies.
It calls the exported `orcaslicer_main` directly. Use a terminal for CLI trials;
the application may reopen console output instead of respecting pipe redirection.

## Progress and remaining work

- [x] Four synthetic fixtures and checksummed manifest.
- [x] Isolated profile snapshot, credential exclusion and SQLite integrity check.
- [x] Common stock/Quack P2S/PLA CLI baseline with identical movement commands.
- [x] Windows native test build and documented review fixes.
- [ ] Complete stock replacement trials and record the user's decisions.
- [ ] Qualify all platform packages, feature combinations, cloud services and hardware.

See `docs/QUACK_BETA_LOCAL_TEST_2026-10-30_DE.md` and the German comparison guide
for actual results, boundaries and open acceptance gates.

Profile snapshot regressions (no live service required):

```powershell
python -m unittest discover -s tools/orca-integration -p test_prepare_test_profile.py
```
