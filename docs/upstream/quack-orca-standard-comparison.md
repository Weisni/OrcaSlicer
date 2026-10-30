# Quack custom features versus Orca standards

Status: comparison planned; no replacement equivalence or retirement decision
has been established. See T0 in the
[integration plan](../superpowers/plans/2026-10-30-orca-alpha-integration.md).

## Purpose and method

Evaluate how far official Orca can cover the user's custom workflow outcomes.
Prepare trials in unmodified Orca so the user can decide whether to adopt its
standard implementation/workflow, keep a small adapter, or retain Quack code.
Preserving a required capability does not require preserving duplicate code.

Use the pinned Alpha and Quack baselines in the design; record any later version
as a separate comparison. Stock trials must use the official application without
Quack patches or custom plugins, with copied/synthetic data and isolated profiles
and credentials. Label third-party/custom-plugin alternatives separately.

## Initial coverage inventory

Split these contract groups into independently testable behaviors during T0.
Every row starts unevaluated; this table makes no claim of upstream replacement.

| Contract | Behavior to compare | Assessment | Decision |
| --- | --- | --- | --- |
| Q1 | Physical UUID inventory, stock, reservation and idempotent settlement | not_yet_evaluated | pending |
| Q2 | Customer/order management, job editing, archive, costs and invoices | not_yet_evaluated | pending |
| Q3 | HA profile authority, variants, overrides, readable names and offline checks | not_yet_evaluated | pending |
| Q4 | HA sync, immutable profiles, QR/NFC, payment and reconciliation | not_yet_evaluated | pending |
| Q5 | Manual/job preheat, takeover, readiness, recovery and final dispatch | not_yet_evaluated | pending |
| Q6 | Per-object towers and stored positions versus upstream tower modes | not_yet_evaluated | pending |
| Q7 | Projected/inwards color penetration and layer depths | not_yet_evaluated | pending |
| Q8 | Wave Overhangs, coverage/support fallback and floor layers | not_yet_evaluated | pending |
| Q9 | Grouped 3MF, color mapping, retained project edits and refresh fixes | not_yet_evaluated | pending |
| Q10 | Organic bottom interface contact, material, gaps and collision fixes | not_yet_evaluated | pending |
| Q11 | Product/update/data/credential identity; distinguish product choices from duplicates | not_yet_evaluated | pending |
| Q12 | Newer local fixes and research; compare only identified implemented behavior | not_yet_evaluated | pending |

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

This file is the comparison deliverable template. The present documentation
update does not run trials, adopt a stock feature or remove custom functionality.
