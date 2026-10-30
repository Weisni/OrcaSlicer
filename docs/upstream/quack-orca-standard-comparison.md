# Quack custom features versus Orca standards

Status: initial Beta source comparison recorded; runtime equivalence and user
replacement decisions remain pending. See T0 in the
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

All stock replacement user trials and retirement decisions remain pending.
