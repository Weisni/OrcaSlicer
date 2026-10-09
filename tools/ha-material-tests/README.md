# HA material contract tests

This focused native target exercises production payload, persistent cache,
background refresh and temporary override helpers without building the GUI or
the full slicing library. Tests remain in `tests/libslic3r` and also belong to
the normal repository suite.

## Run

Use an existing configured QuackSlicer dependency build and Catch2 libraries:

```powershell
cmake -S tools/ha-material-tests -B tools/ha-material-tests/build -A x64 `
  -DHA_DEPENDENCY_ROOT=C:/path/to/configured/OrcaSlicer
cmake --build tools/ha-material-tests/build --config Release --parallel 1
ctest --test-dir tools/ha-material-tests/build -C Release --output-on-failure
```

On Linux/macOS omit `-A x64`. The dependency root must provide OpenSSL under
`deps/build/OrcaSlicer_dep/usr/local` and built Catch2 libraries under
`build/tests/catch2/src` (or its `Release` subdirectory).

## Progress and validation

- Verified Windows MSVC with one compiler worker: 289 assertions in 25 cases.
- Preset-linked profile/binding integration: 281 assertions in 27 cases, using
  the existing HA checkout's core archive and current headers/test sources.
- Regression failures were observed before the identity, persistent-cache,
  display revision, async completion and conflicting-catalog fixes.
- Persistent content is endpoint/schema scoped, immutable, digest checked and
  compatible with Windows paths longer than `MAX_PATH`.
- Profile labels do not create duplicate installations for identical effective
  settings. The full envelope digest still protects publication provenance.
- Shutdown cancels obsolete work, waits for the bounded active HTTP request and
  prevents further background dispatch.

## Remaining checks

- Run the complete native suite and supported-platform release builds.
- Confirm rendered GUI responsiveness and override retention against live HA
  only after deployment is separately authorized. Local tests do not establish
  live HA or physical printer acceptance.
