# Quack custom features versus Orca standards

Status: official 2.5.0 source comparison and targeted Windows CLI trials
recorded below. User replacement decisions remain pending. See T0 in the
[integration plan](../superpowers/plans/2026-10-30-orca-alpha-integration.md).

## Purpose and method

Evaluate how far official Orca can cover the user's custom workflow outcomes.
Prepare trials in unmodified Orca so the user can decide whether to adopt its
standard implementation/workflow, keep a small adapter, or retain Quack code.
Preserving a required capability does not require preserving duplicate code.

Use the user-selected pinned Beta and Quack baselines in the design; keep the
Alpha audit as a historical comparison. Stock trials must use the official application without
Quack patches or custom plugins, with copied/synthetic data and isolated profiles
and credentials. Label third-party/custom-plugin alternatives separately.

## Initial coverage inventory

Split these contract groups into independently testable behaviors during T0.
Assessments below are source findings; they do not establish complete runtime replacement.

| Contract | Behavior to compare | Assessment | Decision |
| --- | --- | --- | --- |
| Q1 | Physical UUID inventory, stock, reservation and idempotent settlement | no_equivalent | retain custom pending comparison/user trial |
| Q2 | Customer/order management, job editing, archive, costs and invoices | no_equivalent | retain custom pending comparison/user trial |
| Q3 | HA profile authority, variants, overrides, readable names and offline checks | stock_partial | retain custom pending comparison/user trial |
| Q4 | HA sync, immutable profiles, QR/NFC, payment and reconciliation | no_equivalent | retain custom pending comparison/user trial |
| Q5 | Manual/job preheat, takeover, readiness, recovery and final dispatch | stock_partial | retain custom pending comparison/user trial |
| Q6 | Per-object towers and stored positions versus upstream tower modes | stock_partial | retain custom pending comparison/user trial |
| Q7 | Projected/inwards color penetration and layer depths | stock_partial | retain custom pending comparison/user trial |
| Q8 | Wave Overhangs, coverage/support fallback and floor layers | no_equivalent | retain custom pending comparison/user trial |
| Q9 | Grouped 3MF, color mapping, retained project edits and refresh fixes | stock_partial | retain custom pending comparison/user trial |
| Q10 | Organic bottom interface contact, material, gaps and collision fixes | stock_partial | retain custom pending comparison/user trial |
| Q11 | Product/update/data/credential identity; distinguish product choices from duplicates | stock_with_adapter | retain custom pending comparison/user trial |
| Q12 | Newer local fixes and research; compare only identified implemented behavior | not_yet_evaluated | retain custom pending comparison/user trial |

## Required record for each candidate

- **Identity:** Q contract/sub-behavior, Quack source/commit, Orca source/commit,
  package versions, documentation/settings/entry points and target platforms/devices.
- **Assessment:** `stock_equivalent`, `stock_partial`, `stock_with_adapter`,
  `custom_plugin_required`, `no_equivalent` or `not_yet_evaluated`; explain why.
- **Scope:** what stock covers, missing outcomes, changed defaults/UX, unsupported
  combinations and dependencies. A plugin API alone is not an implemented feature.
- **Comparison kit:** fixture paths/checksums, safe copied data, setting mapping,
  exact stock-Orca and Quack steps, expected results and observed differences.
- **Evidence:** geometry/G-code, project/profile round trips, material usage,
  workflow/error cases and relevant migration/accounting/safety contracts. Record
  software, live-service and physical acceptance separately; no automatic sends.
- **User trial:** concise German instructions for opening the fixture, locating
  the stock feature, choosing settings and judging the result. Record the user's
  result/preference separately from agent tests; use `pending` until provided.
- **Proposal:** `reuse_stock`, `reuse_with_thin_adapter`, `retain_custom` or
  `retirement_candidate`, with maintenance benefit and explicit losses/tradeoffs.
- **Decision and transition:** user's choice, required adapter, old custom key/data
  migration, historical compatibility, rollback and regression coverage. An
  accepted workflow difference must be explicit. Retain custom behavior until
  the decision is resolved; code removal requires the subsequent authorized change.

## Acceptance

- [ ] All Q groups are decomposed and evaluated against stock Orca.
- [ ] Every plausible replacement has a reproducible comparison kit/user trial.
- [ ] Partial matches, plugins and thin adapters are clearly distinguished.
- [ ] User choices and remaining gaps are recorded without assuming acceptance.
- [ ] Accepted replacements feed the integration tasks and compatibility tests.

No custom function has been retired. User replacement decisions remain pending.

## Beta source findings (2026-10-30)

Target: official 2.5.0 Beta, 345efec64c7b95a05ce5cf96df0dde66a158835c.
These are source-level assessments, not completed physical or user trials.

- Q1/Q2/Q4: the tagged tree has no Quack inventory/customer/order/HA backend
  implementation. Plugin/lifecycle APIs are extension points, not replacements.
- Q3: upstream provides general nozzle variants and preset/filament APIs; Quack
  supplies HA UUID authority, contextual profiles, local overrides and digest sync.
- Q5: standard print workflows/events do not implement the job-bound HA ownership,
  manual takeover, bounded preheat recovery and final inventory claim protocol.
- Q6: standard towers have new geometry and ordering, but the tag has no
  enable_prime_tower_by_object or prime_tower_object_positions options.
- Q7: Beta implements top_color_penetration_layers and
  bottom_color_penetration_layers. It has no color_penetration_mode Projected or
  mmu_surface_normal_penetration_depth. Reuse its ordinary layer-depth handling
  while keeping these extra modes. Compare Inwards against stock before any
  custom-code retirement decision.
- Q8: no wave_overhangs implementation/settings exist in the tagged stock tree.
  Belt support avoidance and ordinary bridging are different strategies.
- Q9/Q10: upstream contains additional color/mixed-slot remapping, grouped import,
  preset/published-project and organic-support fixes. Test each original regression;
  source overlap alone does not establish replacement of the full Quack contract.
- Q11: separate product/update/credential identity is intentional. The local build
  uses QUACK_LOCAL_TEST; stock Orca must not inherit or rotate Quack credentials.
- Q12: local unpublished fixes and research are not silently imported into this
  integration. Their source provenance remains separate from remote Quack main.

The official portable comparison package was downloaded and its SHA-256 checked
against GitHub's published digest:
e11df6023a91ae771399f9beaeb333473a2c4e4f50a0126ae0a2e25b88d8d0c2.

## Local runtime evidence

The common P2S/Generic PLA/0.20mm CLI coupon slice uses identical resolved stock
settings and input. Official Beta and the local Quack build emit the same 4,009
movement commands and calculated 2.40g usage. This validates one ordinary disabled-
custom baseline only; it does not qualify all Q7 modes or other contracts.
The standalone colored fixture uses the standard material-extension color group
so both importers expose its palette. See the [German trial guide](../ORCA_STANDARD_COMPARISON_TRIALS_DE.md)
and [local handoff](../QUACK_BETA_LOCAL_TEST_2026-10-30_DE.md).

### Native nozzle control adoption

The user's 2026-10-30 acceptance request explicitly authorizes correcting the
competing custom nozzle/flow optimization. The Quack flow selector in the Variant
card is removed in favor of Beta's native per-extruder Nozzle control. Its plate
mapping is used, while Quack's connected-printer hardware restriction and HA
roll/profile authority remain. GUI trials cover Standard/High Flow and 0.4/0.6 mm
with two distinct bound roll UUIDs and contextual profile changes. Existing
projects remain loadable; the saved project configuration retains native flow
and material mappings. This is a narrow adopted standard control, not retirement
of HA authority or other custom functions.

Other stock replacement user trials and retirement decisions remain pending.

## Orca 2.5.0 RC source check (2026-11-09)

Compared the official [RC tag](https://github.com/OrcaSlicer/OrcaSlicer/releases/tag/v2.5.0-rc)
with the released Quack 2.5.25 baseline and its local interface correction.
Home Assistant material management is a required part of any migration.

| Area | Reuse or plugin candidate | Remaining requirement |
| --- | --- | --- |
| Q1–Q4: material, orders, profiles | A plugin panel can present HA data with network permission. RC exposes current presets for reading and `orca.host.preset.bundles.reload_local` to load a prepared local bundle. | HA owns physical roll UUIDs, stock, reservations, settlement and the saved profiles. Native slot assignment, per-nozzle/flow variant selection and reconciliation need an explicit adapter or supported equivalent. The read APIs and reload alone do not perform these operations. |
| Q5: preheat and dispatch | Quack's Preferences switch `passive_chamber_preheat` originates in `ef9ea2cd04`. The official RC Preferences and PrintJob sources lack this passive bed-driven preheat path. | Keep the job-bound HA workflow while evaluating a replacement. Lifecycle callbacks are observational, and the native PrintJob finishes after the send operation. A separate printer agent is a transport integration, with P2S compatibility still to prove. |
| Q6–Q8: slicing features | Stock nozzle/flow selection is already adopted. RC top/bottom paint penetration is a standard candidate for ordinary layer-depth coloring. | RC has no matching options for Projected/normal-direction penetration, saved per-object tower locations or Wave Overhangs. Pipeline hooks allow experiments. Functional parity needs access to the relevant geometry, tool ordering and persisted settings. |
| Q9–Q10: import and support | Re-run the existing project-copy regressions against stock before retiring matching fixes. | RC still reserves one configured interface layer for base material when the filaments differ. Keep the interface-count correction in the slicing core while this behavior is present. |

Use the [preset API](https://www.orcaslicer.com/wiki/developer_reference/plugin_development/api_reference/host#local-bundle-reload)
for bundle experiments and the [event API](https://www.orcaslicer.com/wiki/developer_reference/plugin_development/api_reference/lifecycle_events)
for observation. A complete replacement requires preserving the final HA
reservation and dispatch checks and using a single settlement authority.
This review establishes source capabilities. Plugin prototypes and runtime
equivalence on stock RC remain open.

## Official Orca 2.5.0 comparison (2026-11-09)

The [stable release](https://github.com/OrcaSlicer/OrcaSlicer/releases/tag/v2.5.0)
is tagged at `40ecc66f78ed72dc368e0c1ebe40d77f00cc8a19`. The Windows x64
portable asset matched the published SHA-256
`e355d9f8ac0be4efa4b4f99ecae448d4e99def8dee97683847ede5aa0ccb9166`.
The Quack comparison binary is the local 2.5.25 interface correction at
`0017af001e` with the support patch. CLI runs used separate data directories.

| Quack behavior | Stock assessment | Evidence and disposition |
| --- | --- | --- |
| Nozzle diameter and Standard/High Flow control | Adopted | The upstream nozzle control already replaced Quack's duplicate selector. Retain the HA roll/profile association and connected-printer restrictions. |
| Ordinary Inwards top/bottom paint penetration | Equivalent for the tested flat faces | The painted coupon produced 5/4, 2/3 and 7/6 upper/lower layers in both builds. The first two cases matched all evaluated painted extrusion moves. At 7/6, counts matched and 32 outer-wall commands on Z 2.8 mm differed. Use the standard layer-count behavior and retain Projected, normal-direction depth and their project settings. [Orca segmentation source](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0/src/libslic3r/MultiMaterialSegmentation.cpp) |
| Auto brim with per-object small-perimeter speeds | Equivalent for the tested settings | Both builds produced the same 188 brim extrusion moves with the two overrides set to 50% or left nullable. Orca uses the typed float-or-percent accessor. Quack also has a defensive fallback for an unexpected type or empty vector, which these trials did not exercise. [Orca model source](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0/src/libslic3r/Model.cpp) |
| Color mixing | Native slicing feature | Orca provides the virtual mix itself. Keep Quack's accounting for real component rolls, purge and estimated versus measured consumption. |
| Standard 3MF import and project preset handling | Partial | Orca's CLI round trip preserved two objects, but exported one filament and assigned both objects to it. Quack 2.5.25 and the local test build exited with `0xC0000005` on the same config-free CLI export. This is a separate case from the resolved Generic-preset GUI issue. Keep grouped/color-aware import and the Keep current printer workflow pending a GUI comparison. |
| Interface count, material and organic lower contact | No equivalent for the tested cases | With three layers configured, normal and organic bridge slices printed the interface tool at Z 4.6/4.8 mm in Orca and Z 4.4/4.6/4.8 mm in Quack. The organic clip's lower interface had two layers at Z 2.12/2.24 mm in Orca and three at Z 2.36/2.48/2.60 mm in Quack. [Orca support parameters](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0/src/libslic3r/Support/SupportParameters.hpp) |
| Per-object Prime Towers, Wave Overhangs and job-bound HA preheat | No equivalent found | The stable source has no matching per-object tower options, Wave Overhangs controls or passive chamber preheat switch. Keep Quack's implementations. [Orca print settings](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0/src/libslic3r/PrintConfig.cpp) [Orca preferences](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0/src/slic3r/GUI/Preferences.cpp) |
| Flow Ratio calibration import validation | Partial | Orca offers Flow Ratio calibration. The stable code still parses modifier values from the object name without Quack's validation of modifiers and separate objects. Keep the import guard. [Orca calibration source](https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.5.0/src/slic3r/Utils/CalibUtils.cpp) |
| HA material and order workflows | No stock equivalent | HA profile authority, roll UUIDs, reservations, settlement, orders, costs, payments and history remain requirements. |

Local fixtures, output and the G-code comparison script are under
`build-test/stock-2.5.0-audit`. The reported equivalence is limited to the
recorded cases. GUI import parity, saved-project migration and physical printing
remain separate checks.

## Local Native Test

The 2026-11-11 request authorizes applying the verified standard paths in a new
local build while the previous test build is in use. The baseline is Quack
`068306e1be` with the tested 2.5.25 support-interface patch.

- Multi-material painting uses Orca's standard segmentation entry point for
  its layer-depth pipeline. Quack's Projected mode remains available within
  that pipeline. Surface-normal depth takes its existing specialized path
  using the same layer preprocessing. The project and preset keys stay compatible.
- Auto brim uses the native typed accessor for nullable speed overrides.
  Missing, empty and unexpected options retain their fallback speeds; legacy
  non-nullable vectors still work.
- Native nozzle/flow controls and color mixing were already in use. HA retains
  saved-profile and physical-roll authority, including allocations for virtual
  mixed filaments. HA preheat/dispatch, per-object towers, Wave Overhangs,
  grouped 3MF imports, calibration guards and support fixes remain in Quack.
- The Windows package identifies itself as `QuackSlicer Native Test`. It has a
  separate data directory, credential service and Windows app ID. Its launcher
  uses a credential-free snapshot of the installed profile and inventory.
- On Windows x64, 135 targeted C++ cases passed (2,341 assertions), covering
  support, painting, brim, mixing, Wave Overhangs, presets, 3MF regressions,
  credential namespaces and HA bindings. The HA suite passed 436 Python cases
  and 11 dashboard suites with local fixtures. Three profile-snapshot cases
  also passed.
- The packaged executable sliced five paint fixtures (three Inwards depth
  combinations, Projected, and 0.6 mm surface-normal depth) and two brim
  fixtures. The positive-extrusion XY moves for the paint fixtures and the
  brim moves for the brim fixtures matched the previous Quack test by tool,
  layer and feature, with feed rate excluded from the comparison.
  The normal and organic bridge both put the interface tool on Z 4.4, 4.6 and
  4.8 mm. The organic lower contact used Z 2.36, 2.48 and 2.60 mm. The saved
  Generic-profile project sliced and exported with a copy of the new profile.
- The launcher opened a separate Native Test window alongside the installed
  app. The startup log confirmed the selected Bambu Lab P2S 0.4 mm and its
  0.12 mm High Quality Smooth Surface process.

This is a local adoption of the paths above. The other pending comparisons
and the config-free 3MF CLI export issue are tracked separately.
