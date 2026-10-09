# Native standard 3MF import verification

## Reproduced input and cause

The supplied `C:/Users/marce/Downloads/Whinny Pooh Ears (1).3mf` is a
standard 3MF with 45 single-part objects, 53,012 triangles and five colors.
All colors are inherited from object `pid`/`pindex`; triangles contain no
explicit color override. Original SHA-256 before and after native verification:
`1209638f1a614ffeefcc50b536460c0fbfc1921098f2a040d6fa140358a5018e`.
The original file was read only.

`Plater::priv::load_files()` clears standard-3MF object/volume configuration.
`ObjColorPanel::apply_color_mapping_to_model()` restores mapped volume bases,
but the shared importer previously left single-part object defaults unset.
The object-list row reads the object default and therefore displays filament 1.
The earlier six-line correction existed in the primary checkout only, and was
absent from the native test worktree and its retained runtime core archive.

## Changes

- `Model::obj_import_color_deal()` copies a positive explicit base from the only
  model-part volume to its object when no explicit global first filament applies.
  Multipart defaults and the explicit first filament for OBJ remain unchanged.
- `FacetsAnnotation::reset()` also calls the existing `reset_used_states()` helper.
  Repeated Replace previously retained a removed painted filament in used-state
  metadata: changing filament 6 to 2 reported both 2 and 6 afterward.
  Reset now consistently clears triangle mappings, bitstream and state metadata
  for segmentation, support, seam and fuzzy-skin annotations.
- Regression coverage is in `tests/libslic3r/test_model_filament_remapping.cpp`,
  already registered by `tests/libslic3r/CMakeLists.txt`.

There is no additional GUI source change for this correction. The runtime
must replace **Model.cpp** in its core archive. Remove the archived `Model.obj`
member before linking the new native object, so the older implementation cannot
satisfy the same symbols first. The root task owns runtime assembly and GUI tests.

## Fresh native verification

The isolated projects under `diagnostics/3mf-object-colors/` reuse configured
compiler/link settings and the actual retained native baseline core archive.
The green executables link the new `Model.cpp` object and a copy of that archive
with its old Model member removed. Builds use one compiler worker.

- Baseline mapping regressions: five cases, two pass and three fail because the
  single-part object lacks `extruder` (single-part, painted, repeated Replace).
- Baseline reset regression: all four annotation variants retain the old state,
  producing eight failed checks (clear-state and repaint checks per variant).
- Corrected selected core/import executable: **21 cases, 160 assertions pass**.
  This includes existing standard-3MF round trips, the painted-first-triangle
  base-color regression, color-slot replacement tests, multipart defaults,
  explicit OBJ defaults and all four reset/repaint annotation variants.
- Native supplied-input probe: **45 objects, five colors, zero mismatches** for
  object defaults, volume bases and unnecessary painting. Identity mapping slot
  counts are `5, 4, 16, 19, 1`.
- Explicit replacement palette `5,3,1,4,2`: **zero mismatches**; slot counts become
  `16, 1, 4, 19, 5`. This exercises a non-identity Replace mapping through the
  actual archive importer.
- No existing in-tree test refers to `used_states` or assumes retained states
  after a facet reset.

Relevant logs are `red-tests.log`, `red-reset-tests.log`,
`green-all-selected.log`, `red-input.log`, `green-input.log` and
`green-input-remapped.log` in the diagnostic directory. The source-side fix,
local native object and selected tests are verified. Full CI, rendered GUI
acceptance and a published release remain outside this subtask.

The inherited build settings emit the existing `ExtrusionEntity.hpp` unused
`temp` warning and a library-default conflict warning (`LNK4098`); native builds
and tests complete successfully. The existing multi-nozzle compatibility test
also logs its expected nozzle-list reconstruction warning.

## Repeat read-only checks

With the isolated projects already built, include the configured core-test DLL
folder in PATH and run:

```powershell
./diagnostics/3mf-object-colors/bin/mapping_green.exe
./diagnostics/3mf-object-colors/bin/probe_green.exe `
  'C:/Users/marce/Downloads/Whinny Pooh Ears (1).3mf' `
  (Resolve-Path diagnostics/3mf-object-colors/data).Path
./diagnostics/3mf-object-colors/bin/probe_green.exe `
  'C:/Users/marce/Downloads/Whinny Pooh Ears (1).3mf' `
  (Resolve-Path diagnostics/3mf-object-colors/data).Path '5,3,1,4,2'
```

## Remaining tasks

- [x] Reproduce the supplied archive with the native importer and original core.
- [x] Observe mapping/reset regressions failing before their respective fixes.
- [x] Verify corrected object defaults, volume bases, painted-facet remapping
  and unchanged original archive hash.
- [ ] Include Model.cpp in the assembled native test runtime and verify the GUI.
- [ ] Run release CI and publish through the normal reviewed release flow.
