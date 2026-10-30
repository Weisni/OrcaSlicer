# OrcaSlicer 2.5.0 Alpha integration design

Date: 2026-10-30. Status: proposed integration design; documentation only.
The user requested a source/release audit, a complete integration plan, and
project agent instructions. No application integration, deployment, merge,
release, heating, or physical print is part of this task.

## Intent and scope

Bring the complete OrcaSlicer Alpha feature set into QuackSlicer while retaining
the user's existing inventory, customer/order, HA, print, and slicing workflows.
Availability means working UI/CLI entry points, installed dependencies/assets,
project serialization, profile migration, and verified interaction with Quack
features. Compilation or the presence of a setting alone is insufficient.
Hardware-dependent functionality must be available for compatible printers;
the user's P2S does not become a belt, IDEX, IQEX, or H2C printer.

## Verified baselines

| Reference | Pinned revision / finding |
| --- | --- |
| Orca Alpha | `v2.5.0-alpha`, `3880f2c58ec392ed2bd5938ab31c9c7c496c0eb2` |
| Alpha publication | 2026-10-16, 14:28:11 UTC; prerelease |
| Official stable at audit | `v2.4.2`, published 2026-07-07 |
| Quack remote main | `2e38319f4e0762bc4cf44d46a28160e0fa2ae3f5`, version `2.5.24` |
| Primary checkout HEAD | `a424df1af1691f625e6e7b97b50fe382ada3d50d`, 56 commits behind remote main |
| Common ancestor | `95b781745dfffd458b703f6a743ba93416d5d354` |
| Divergence, Quack / Alpha | 110 / 2,408 commits exclusive to each side |
| Changed paths from ancestor | Quack 414; Alpha 16,323; shared 136, using `--no-renames` |
| Open Quack PRs | None returned by connected GitHub search during this audit; refresh before implementation |

The 136 shared paths are review candidates, not a predicted merge-conflict count.
The wide profile/resource churn makes a hand-picked headline-feature port unsafe
as a route to full parity. Version numbers are independent: Quack 2.5.24 does
not imply Orca 2.5.0 functionality. The tag was fetched without checkout changes.
Live main and installed executables were not substituted for the pinned tag.

The primary working tree contains unrelated modifications and untracked work.
Preserve all of it, including apparently empty/line-ending-only diffs. Existing
README and working-guideline content must be retained. Do not pull/reset this
checkout or treat its older source as the current released-feature inventory.

## Approaches

1. **Recommended: integrate the complete pinned Alpha into current Quack main.**
   Work in an isolated branch/worktree, preserve Git ancestry, resolve shared
   paths by behavior, and restore Quack contracts with targeted test cycles.
   This retains bug fixes, profiles, assets and platform support together, but
   requires substantial integration review and a prerelease qualification cycle.
2. **Rebase Quack onto the Alpha and replay its delta.** Technically possible,
   but rewriting/replaying 110 commits and mixed upstream backports adds history
   and omission risk. Keep as a fallback only if a merge spike demonstrates a
   material reduction in conflicts without losing behavior; document the evidence.
3. **Selective backports.** Useful for isolated urgent stable fixes; rejected
   for this objective because plugins, geometry, materials, printer agents,
   dependencies and profiles interact and full parity becomes hard to establish.

This is the author's recommendation, not an already approved implementation
choice. A future implementation starts by reviewing this design and refreshing
the baselines. No automatic conversion of Quack core features into plugins is
proposed; plugin availability and preservation of core behavior are separate goals.

## Complete Alpha coverage

The companion [coverage ledger](../../upstream/orca-2.5.0-alpha-coverage.csv)
tracks all **351 bold release-note entries**, referring to **646 distinct PR
numbers** from their headings. Entries retain source order (`A001` onward), area,
PR URLs, owning implementation task, disposition and evidence. Locate an entry
by its PR list in the official release notes. This is not the total Alpha PR or
commit count. Changes omitted from release highlights still belong to the
complete tagged source-tree integration. Never use the ledger to cherry-pick
away unlisted fixes. All rows currently mean planned, not implemented/tested.

| Capability family | Required delivery and interaction check | Owner |
| --- | --- | --- |
| Python extension runtime | Bundled Python 3.12.13, package installer, local/cloud install, wheels and dependencies, discovery/reload/update/unload/error handling | T2/T3 |
| Plugin API and configuration | Script, pipeline, printer-agent, pages, texture resources, events, notifications, global/preset overrides, saved-project dependencies | T3/T6 |
| Plugin permission handling | File/network/process/thread prompts; deny/revoke; Quack secret and credential exclusions; do not portray this as an OS sandbox | T3 |
| Command search | Speed Dial in English and German, shortcuts/favorites, plugin actions, settings and Quack management navigation without stealing text entry | T3 |
| Parametric design | CAD tab, sketches/constraints, text/revolve and other tagged tools, SolveSpace/OCCT modules, editable 3MF history, undo and plate transfer | T2/T3 |
| Virtual mixed materials | Two/three-source blends, sublayers/gradients, decomposition and painting; resolve to physical source rolls for stock and costs | T4 |
| Colored-model import | glTF/GLB/FBX, OBJ materials/vertex colors/textures, color reduction and automatic mixing; preserve grouped standard-3MF imports | T4 |
| Texture geometry | Relief baking, eight texture layers, projections/UV editor/model UV, image adjustments, color mixing, embedded assets and undo | T4 |
| Conveyor printers | Transform/slicing, first-layer band, brim/support/purge/arrange/preview behavior and tagged printer profiles | T5 |
| Parallel tools | IDEX/IQEX zones and placement, parallel toolpaths/materials, correct preview and usage/cost accounting | T4/T5 |
| Nozzle variants | H2C/A2L, mixed H2D/X2D sizes, general variants, variant PA/cooling; preserve HA printer/nozzle/flow profile context | T4/T6 |
| Perimeters/seams/surfaces | Precise Seam modifiers, top expansion, inward wipe, unsupported-wall order, retract-after-wipe, Arachne resolution, spiral lift, polyholes and vase rules | T5 |
| Infill | Adaptive Gyroid/TPMS, smoothing, Spiral Inset, surface orders/directions, separated assembly infill, multiline/Cubic/Honeycomb and faster Gyroid | T5 |
| Adhesion/support/cooling | Mouse ears, raft expansion, small-support slowdown, support painting lock, fuzzy bridge exclusion and first-layer fan | T5 |
| Multimaterial purge | Rib/skip-point towers, sparse layer variants, waits/retraction/lifts, tool ordering, finished-tool shutdown and all tagged purge fixes | T4/T5 |
| 3D view/preview | Shadows, X-Ray, section/mass/focus views, preview colors/dimming, outlines, movement lengths, camera controls and accurate G-code listing | T3/T5 |
| Sharing and UI | Publish 3MF, Home/cloud, keyboard mapping, align/distribute/drop, color favorites, plate toolbar, preset-save UI and portable bed assets | T3/T8 |
| G-code and estimates | Regex/time/current-bed placeholders, optional config footer, timing/corner speed, firmware retract, custom-code state restoration and statistics | T5 |
| Bambu connectivity | Network plug-in 02.08, agent interfaces, AMS drying/gradient colors, camera/SD/timelapse and filament lists | T6 |
| Performance/reproducibility | Startup and switch caching, CPU/GPU/memory/export improvements, deterministic slicing/export across thread schedules | T2/T5/T9 |
| CLI | Strict/results errors, grounding/inspection/painting, stdout settings, statistics JSON, arrange rotation defaults, mixed colors and thumbnails | T5 |
| Robustness | Malformed meshes/projects, safe downloads/bundles, multi-instance config locking, credential isolation and resilient networking | T2/T3/T7 |
| Calibration | Adaptive PA validation, nozzle-aware VFA, PA/temperature/retraction tests, speed limits and cancellation | T5 |
| All corrective changes | Wall/bridge/support/brim/import/preset/UI/platform/crash/host fixes in the tagged tree; check existing Quack backports before resolving duplicates | T5/T6/T8 |
| Profiles and OTA | New/updated printers/materials, one filament ID, compatibility migrations, OFL/OTA, Bambu default 3MF and profile test slices | T4/T8 |
| Localization/platforms | Updated context/plurals/units/web languages/Basque; Windows clang-cl, Linux ARM64/Flatpak and macOS packaging | T2/T8 |

## Quack preservation contracts

| ID | Existing behavior and source anchors | Required regression evidence |
| --- | --- | --- |
| Q1 | Inventory and UUID identity: `src/libslic3r/FilamentInventory.*`, `GUI/FilamentInventoryService.*`, `GUI/FilamentReservationPlan.hpp` | Schema-8 upgrade fixtures, stock/history/reservations, duplicate terminal-event idempotency, shared-roll allocations, backup/restore |
| Q2 | Customers/orders/invoices: `GUI/CustomerOrderDialogs.*`, `GUI/CustomerInvoiceDialog.*`, `GUI/FilamentManagerPanel.*` | Archive/restore, statuses/search/sort, stable selection, costs/invoice history and editable tracked jobs survive migration |
| Q3 | HA material authority: `src/libslic3r/HaMaterial*.hpp`, `HaProject*.hpp`, `GUI/HaMaterial*.hpp`, `HaInventory*.hpp` | Same physical UUID via manual/QR/NFC; exact saved context/digest; local overrides and readable names survive reopen/nozzle changes; offline roster never grants stale dispatch |
| Q4 | Incremental/profile sync and HA services: `tools/ha-material-demo/` | Full/delta/reset/CAS/immutable profile payloads, order/cost/payment/NFC/consumption and reconciliation remain intact; legacy capability fallback preserved |
| Q5 | Preheat and dispatch: `GUI/Jobs/PrintJob*`, `GUI/HaJobPreheat.hpp`, `src/libslic3r/HaJobPreheat.hpp`, HA `preheat*.py` | Job/instance ownership; atomic manual takeover; door/readiness revocation; bed conditioning; one bounded 15-minute allowance; cancel/recovery/uncertain dispatch without duplicate sends |
| Q6 | Per-object towers: `enable_prime_tower_by_object`, `prime_tower_object_positions`, `GCode/WipeTower.cpp`, GUI plate/gizmo code | Multiple towers remain serialized, placed safely and emitted correctly under new tower types/tool ordering; disabled behavior matches upstream |
| Q7 | Projected and surface-normal color depth: `color_penetration_mode`, `mmu_surface_normal_penetration_depth`, `MultiMaterialSegmentation.*` | `inwards` and `projected` remain distinct; top/bottom layer counts and colored imports/texture bakes retain intended paint regions |
| Q8 | Wave Overhangs: `WaveOverhangs/`, `PrintObject.cpp`, `Layer*`, `Fill/Fill.cpp`, `GCode*`, `docs/WAVE_OVERHANGS.md` | Disabled path equivalence; enabled paths, uncovered support fallback, floor layers, speed/flow/fan/temperature restoration and diagnostic tags |
| Q9 | Project import and presets: `Format/3mf.cpp`, `Format/bbs_3mf.*`, `Model.cpp`, `PresetBundle.cpp`, `GUI/Plater.cpp` | Keep/replace settings, grouped imports, object/volume/painted-face filament mapping and deferred preset refresh; no crash or unwanted printer/profile reset |
| Q10 | Organic bottom interfaces: `Support/TreeSupport*`, `SupportCommon.cpp`, support tests | Contact presence, material, gap and no extrusion-volume intersection, including zero top gap and independently configured bottom gap |
| Q11 | Product identity/update: `version.inc`, app config/resources, updater and workflows | Quack data/credential namespace, signed update verification/channel, executable metadata, URLs, icons, installer/portable/uninstall behavior |
| Q12 | Local research and newer work | Non-planar remains research; keep ZAA distinct. Reconcile dirty arrange/model/import changes, HA audited-status/Print Again work and other worktrees separately; never infer they shipped in remote main |

`GUI/` paths above are relative to `src/slic3r/`. The remote-main feature list
is backed by source/log inspection; physical behavior is not newly validated.
Later local guidelines describe HA status correction and Print Again behavior
that require provenance checks against their own branch/installed state. Preserve
observer/native accounting, unknown consumption and explicit settlement review;
do not silently promote or overwrite that work during the Alpha merge.

## Integration decisions and unresolved risks

- Before porting custom features, compare every Q1-Q12 behavior with unmodified
  official Orca. Prefer a proven upstream implementation where it preserves the
  required outcome; document partial coverage and the smallest necessary adapter.
  Prepare reproducible stock-Orca trials so the user can choose whether to adopt
  its workflow and retire redundant custom code. Retain current behavior until
  that decision; matching feature names do not prove equivalence.
- Use the [standard comparison matrix](../../upstream/quack-orca-standard-comparison.md)
  to record built-in versus plugin-assisted alternatives, limitations, old-setting
  migration and rollback. Moving a feature to a custom plugin is not adopting a
  stock standard and remains a separate architecture choice.
- Keep virtual material indices separate from stable physical-roll UUIDs. Expand
  every mix into physical usage, including support, purge and texture-generated
  colors. Aggregate once per UUID; retain per-role detail and avoid charging both
  virtual and physical amounts. Use emitted usage when available; label estimates
  and unknown actual consumption explicitly. Ratios alone may miss purge.
- New nozzle types/variants must not collapse HA's printer/nozzle/flow key.
  Unknown/incompatible mappings retain settings and block affected dispatch with
  an actionable correction. Do not globally remove mixed-material functionality.
- Merge the agent changes with Quack reservation/preheat/dispatch at the existing
  print boundary. Plugin callbacks are observations, not a second stock-settlement
  authority. An uncertain physical send must never be retried as a test or recovery.
- Keep Alpha's experimental labels and printer restrictions. Belt/parallel modes
  combined with Quack-specific paths need explicit support-matrix tests; any
  unsupported combination is a recorded blocker, not silently hidden as complete.
- Bundle matching Python, uv, CAD solver, OCCT 8.0.1 and CGAL 6.2.1 assets per
  architecture. `SLIC3R_CAD=ON` must match dependency and application builds.
  Old build caches/ABI artifacts cannot prove the integrated build is coherent.
- Alpha upstream removes old Windows build scripts in favor of `build_win.bat`;
  update Quack CI/docs accordingly. Preserve existing release identity and do not
  point signed Quack updates at Orca releases.
- Cloud, OFL/OTA, plugin hub and binary network-agent compatibility are external
  dependencies. Verify fork identity, API compatibility and permitted endpoints
  live during qualification; never promise that recompilation alone enables them.
- Preserve old projects/settings/database snapshots; additive migrations with
  migration tests and a documented backup-based downgrade are required. Never
  claim an old app can reopen new CAD/texture/mix metadata losslessly without a test.

## Acceptance and scope boundary

Success requires every coverage row and shared-path row to have reviewed
disposition and evidence, all Q1-Q12 outcomes covered by retained custom code or
accepted tested upstream replacements, complete tagged-tree
review, clean platform builds, automatic tests and isolated GUI acceptance.
Cloud/printer/hardware checks remain individually recorded gates. Release is
initially a Quack prerelease; do not infer the next version number or authorize
publication from this planning request. No source implementation was performed.

## Sources and reproducible evidence

- [Official Alpha release](https://github.com/OrcaSlicer/OrcaSlicer/releases/tag/v2.5.0-alpha)
- [Pinned Alpha tree](https://github.com/OrcaSlicer/OrcaSlicer/tree/3880f2c58ec392ed2bd5938ab31c9c7c496c0eb2)
- [Pinned Quack main](https://github.com/Weisni/OrcaSlicer/tree/2e38319f4e0762bc4cf44d46a28160e0fa2ae3f5)
- [Official releases](https://github.com/OrcaSlicer/OrcaSlicer/releases)
- Local source/log/tree reads used the above commits, not only the older checkout.
- `git merge-base <quack-sha> <alpha-sha>`; `git rev-list --left-right --count <quack-sha>...<alpha-sha>`.
- Intersect `git diff --no-renames --name-only <ancestor> <quack-sha>` with the
  corresponding ancestor-to-Alpha list; the [shared-path ledger](../../upstream/orca-2.5.0-alpha-shared-paths.csv) records all 136 paths.
- [Implementation plan](../plans/2026-10-30-orca-alpha-integration.md).


## Execution authorization and Beta target

On 2026-10-30 the user authorized implementation and an isolated local test build, then explicitly selected OrcaSlicer 2.5.0 Beta (345efec64c7b95a05ce5cf96df0dde66a158835c, published 2026-10-27). This supersedes the earlier planning-only scope and Alpha implementation target. Historical Alpha audit counts remain labeled snapshots. Keep the Alpha coverage ledger and additionally evaluate the Beta ledger and regenerated shared-path ledger. No production installation, live HA deployment, physical print/heating or release publication is authorized. No custom feature is retired without its comparison and user decision.
