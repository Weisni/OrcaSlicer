# Wave Overhangs

Wave Overhangs is an experimental, opt-in toolpath strategy for printing steep
or horizontal FDM overhangs with less conventional support material. Instead
of filling an unsupported region with straight bridge lines, the slicer starts
at the supported boundary and emits successive curved wavefronts. Each new
line overlaps and bonds laterally with the line printed immediately before it,
allowing the path to advance into otherwise unsupported space.

## Intended behavior

- The feature is disabled by default. With it disabled, slicing and generated
  G-code must remain equivalent to the existing OrcaSlicer behavior.
- OrcaSlicer's existing overhang detection identifies candidate regions. The
  strategy is geometric and can therefore apply to angled as well as fully
  horizontal overhangs.
- A narrow anchor band is created at the supported edge. Wavefronts expand
  through the unsupported region until it is covered or no valid front can be
  generated.
- Pattern, spacing, overlap, flow, speed, cooling, seam placement, and maximum
  propagation controls are available in advanced mode. Simple mode exposes
  only the master switch.
- Areas that cannot be covered safely remain eligible for normal bridge or
  support generation. Enabling waves must not silently remove all fallback
  support.
- Wave regions are tagged in G-code so they can be inspected and diagnosed.

## Pipeline integration

1. Detect wave candidates from the current layer and the supported geometry
   below it.
2. Generate wave paths from supported boundary seeds and record the covered
   area on the layer.
3. Exclude the covered area from conflicting wall, bridge, infill, and support
   paths while preserving uncovered remainders.
4. Emit wave extrusion with its configured speed, flow, cooling, and optional
   diagnostic comments.
5. Treat the configured floor layers immediately above a wave region as solid
   surfaces and ramp their speed back toward the ordinary profile value.

## Compatibility and limitations

All new process settings have defaults and are serialized through the existing
configuration system so older projects and profiles continue to load. The
master setting gates every behavior change.

The method is experimental. PLA with strong part cooling is the most practical
starting point. Large spans may warp, and PETG, ABS, PC, and other slower-
cooling materials may delaminate or deform. Users should inspect the preview
and validate conservative settings on a small test model before relying on the
feature for production parts.

The implementation is based on the published wave-inspired path-planning work
and the OrcaSlicer WaveOverhangs reference integration:

- https://doi.org/10.2139/ssrn.6640458
- https://github.com/dennisklappe/OrcaSlicer-WaveOverhangs
