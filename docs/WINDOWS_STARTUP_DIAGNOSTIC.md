# Windows local-build startup recovery

Date: 2026-09-26.

## Confirmed fault

The local Release application exited with `0xc0000374` before showing its main
window, with both a separate cloned profile and an empty profile. Disabling
antialiasing did not help. The initial crash dumps ended in heap management and
NVIDIA allocation frames; these were detection sites, not evidence of a driver
defect.

Full page-heap verification under GDB located the first invalid write in
`GLCanvas3D::GLCanvas3D`, initializing a member at offset `0x3280`. The caller
`View3D::init` in the same DLL allocated only `0x3280` bytes. Thus the constructor
wrote immediately beyond the allocation. Local matching PDBs resolved the
stack; exported-symbol guesses from GDB alone were not used as function names.

The caller object `GUI_Preview.obj` was dated September 24, while the constructor
object `GLCanvas3D.obj` and class header were dated September 26. The binary
combined incompatible class layouts from an incomplete incremental build.
Recompiling only the edited `.cpp` files and relinking is insufficient after
class-layout changes: every dependent translation unit must be rebuilt.

Diagnostic artifacts (local, ignored build directory):

- `build/arrangement-review/gdb-startup.log`
- `build/arrangement-review/startup-symbols.txt`
- `build/arrangement-review/resolve_startup_symbols.py`
- `build/startup-rebuild-final.log`
- `build/startup-test-rebuild-final.log`
- `build/startup-regressions.log`

The verifier was enabled only for a uniquely named copy,
`quack-slicer-startup-test.exe`, and its settings were removed after capture.
The installed application and its profile were not used for diagnostics.

## Recovery and prevention

Project-owned source and generated-header directories now use normal CMake
include directories; third-party directories retain `SYSTEM`. This keeps local
headers distinct from external dependencies. An isolated compiler probe showed
that a simple `SYSTEM` header edit *does* trigger recompilation on this toolchain;
the `SYSTEM` flag alone is therefore not established as the cause of the stale
object. The complete application was recompiled with consistent headers.

Use `/m:1 /p:CL_MPCount=2` as a conservative setting on this machine to avoid
compiler memory exhaustion. This recovery uses four compiler workers after
checking available memory, while keeping project builds serialized.
Always invoke the build through CMake, which selects the configured BuildTools
installation (MSVC 14.44). A separate direct invocation of the installed Community
MSBuild selected MSVC 14.43 and failed to link the existing 14.44 dependencies;
the tests were rebuilt successfully through CMake with the matching toolchain.
After a class-layout change, verify dependent object rebuilds and run the GUI
with an isolated `--datadir`; successful linking and backend tests alone do not
prove a usable application.

## Acceptance

- [x] Reproduce the startup fault with isolated profiles.
- [x] Locate the out-of-bounds write and prove the allocation/layout mismatch.
- [x] Remove temporary Application Verifier settings.
- [x] Finish the consistent Release build.
- [x] Re-run arrangement and inventory regressions: 73 cases / 1,628 assertions,
  randomized order with seed `26092026`, all passed on the rebuilt core.
- [x] Verify two normal starts with 4-sample antialiasing, loading all three
  plates of the project copy, and normal shutdown of the first test instance.
  Full arrangement acceptance was not completed: the additional arranger
  reported a fixed-object/exclusion conflict before the user stopped UI testing.

## Arrangement rollback (2026-09-26)

The user subsequently withdrew the additional arrangement feature after finding
the existing built-in tool. The original seven arrangement source/test files
were restored, including the original `GLCanvas3D` layout; the feature-specific
fixture and documents were removed. The CMake project-header correction and
order-management changes remain. Historical test totals above describe the
pre-rollback build and do not describe the retained test suite.

- [x] Verify the arrangement files exactly match their original repository versions.
- [x] Rebuild the affected Release components consistently after the rollback.
  `GLCanvas3D.cpp` and `GUI_Preview.cpp` were both recompiled, with the application
  DLL rebuilt at 15:48 on 2026-09-26.
- [x] Run the original arrangement and retained inventory tests: all 59 cases /
  744 assertions passed, randomized order with seed `26092026`.
- [x] Check startup with an isolated profile and 4-sample antialiasing: the main
  window responded, the 3D canvas initialized on NVIDIA OpenGL 4.6, and the test
  instance shut down normally. This was a startup smoke test, not a repeat of
  the full management UI acceptance checks.

Rollback evidence is stored locally in `build/arrangement-rollback/build.log`,
`tests.log`, `startup-status.json`, and `gui-data/log/`.

Never modify `Downloads/Bottle Thread Game V2.3mf`; GUI checks use the copy in
`build/arrangement-review`.
