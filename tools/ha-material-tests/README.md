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

## HA DNS transport regression (2026-10-10)

Configure with `-DHA_HTTP_DNS_TESTS=ON` to include real HTTP transfers against
a local loopback server. The first request seeds the production DNS cache;
the next independent handle must reach the server without resolving the
otherwise nonexistent hostname. No HA credentials or database writes are used.
Removing shared DNS reuse reproduced a timeout; enabling it passed.

Live diagnosis: Windows name resolution for `homeassistant.local` sometimes
took about eleven seconds, exceeding the previous eight-second connection
deadline. HA now opts into thread-safe DNS reuse and allows a 15-second cold
connection deadline within a cancellable 20-second whole-request deadline.
Non-HA HTTP callers retain their existing behavior. Transport errors identify
the actual curl/HTTP failure instead of claiming every failed read has a pending
write. Initial live probes passed at 11-12 seconds cold and 0.24-0.28 seconds
for subsequent requests. Full GUI acceptance is recorded separately.
