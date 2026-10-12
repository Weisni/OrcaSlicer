# Incremental HA inventory and authoritative material profiles

The user approved implementation of the proposed HA authority/cache design on
2026-10-09 and explicitly requires temporary local filament-setting adjustments
to remain possible without saving a project or publishing them to HA.

## Behavior

- HA owns material profiles and committed roll/profile associations. Local
  profile edits are temporary overrides, never automatically uploaded, and
  survive background refresh and changing HA profile revisions.
- A committed new association uploads the complete effective profile first,
  waits for HA acknowledgement, then installs/uses that acknowledged content.
  Failed or uncertain publication preserves the previous selection.
- On first connection, fetch all published HA profile contents into a persistent
  cache. Repeated content shared by several rolls is downloaded once. Thereafter
  download only missing content hashes. Compatibility still governs selection.
- Identical content uses the same managed cache profile, independent of display
  name or roll UUID. Existing ordinary user/system profiles remain intact.
- Profile contents are immutable and addressable by SHA-256 independently of
  the current roll association. This removes mutable-association download races.
- Refresh must not block ordinary GUI painting, dropdown opening or timers.
  Only one background refresh per endpoint is in flight; older results cannot
  replace a newer acknowledged state or a newly configured endpoint.
- Unchanged inventory requires only a small revision request, without exporting
  the local graph or constructing the full server snapshot. Changed graphs use
  row identities to transfer/merge only upserts and deletions, then commit a
  complete validated result atomically. Unchanged native rows remain untouched.
- Database generation plus revision detects server restart/reset/restore.
  Missing older history or generation mismatch triggers a bounded full resync;
  a matching current revision remains unchanged even without retained history.
  Network failure leaves the last complete cache usable. Existing print-start
  freshness, stock and reservation requirements remain enforced.

## Protocol

Keep existing endpoints compatible. Add authenticated GET `/sync` accepting
optional `generation` and `revision`. Return `sync_version: 1`, `generation`,
`revision`, `unchanged`, `reset`. Unchanged responses contain no graph. Changed
responses contain `snapshot` (the provider material view) and `native_delta`
with `schema_version: 8` and `tables`; each table has `upserts` and `deletes`.
Delete identities use the same canonical JSON-array keys as C++ `row_key`.
For reset responses, upserts contain the complete graph and deletes are empty.
Revision comparisons and the captured graph come from one read transaction.
History is bounded; complete replies obey the existing 64 MiB/250,000-row limit.

Add authenticated GET `/profile_content?sha256=<digest>` for immutable content.
The existing profile/variant mutation path persists content by digest in the
same transaction as the association. Existing profiles are backfilled locally.
The provider snapshot supplies all profile summaries, including variants;
clients preload distinct missing digests. An older HA component retains the
existing read path until upgraded.

## Validation

Tests cover O(1) unchanged responses (snapshot is not called), single-row
updates, deletions, generation changes and expired history; malformed deltas,
duplicate identities and interrupted reads must preserve the cache. Profile
tests cover deduplication, immutable reads after reassignment, digest integrity,
compatibility, restart caching and temporary overrides. Compare transferred
bytes/request counts using representative synthetic data. Native compilation
and local tests do not establish live HA deployment or rendered GUI acceptance.

## Scope

No live HA deployment, printer commands, release, push or merge is included.
Work from the current HA-enabled 2.5.21 source in an isolated managed worktree;
preserve unrelated changes in the primary checkout.
