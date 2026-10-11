# QuackSlicer 2.5.22 release preparation

## Changes

- HA reads compare revisions and stable table/row hashes, apply only changed
  data, and retain the last complete cache on failed or interrupted refreshes.
- Saved HA profiles are authoritative and cached by content identity. A newly
  selected saved baseline is published before use. Unsaved material adjustments
  remain local to the selected project filament until explicitly synchronized.
- Preserve original profile names in filament labels, editors and Save as;
  filter available profiles by name. Internal hashes do not replace display names.
- Keep material selection and profile upload in one loading sequence. Network
  requests run off the GUI thread; parent windows stay disabled between upload
  phases. Cancellation and deferred inventory callbacks remain supported.
- Reuse DNS resolutions for HA requests with bounded cold-resolution timeouts,
  concrete transport errors and durable request identities for uncertain writes.
- Remap standard 3MF object filament defaults alongside volume and painted-face
  assignments when importing with Replace.

## Compatibility and deployment

The optional bundled HA integration adds incremental read and immutable profile
content routes while preserving older full-snapshot behavior. Keep existing HA
configuration/database backups when upgrading. Live installations may use a
different component domain or newer NFC/order features; port only the required
incremental/profile changes rather than replacing an installation indiscriminately.
No HA credentials, private inventory snapshots or runtime binaries belong in
this PR. Existing project/profile formats remain compatible.

## Validation and release gates

- Local focused native suites, Python integration tests and the native Windows
  loading-window fixture are rerun for this preparation; PR CI provides clean
  supported-platform builds and the full repository suite.
- Earlier direct-EXE live tests checked grouped Whinny Replace assignments,
  profile names, per-filament local edits, Save as, repeated spool selection and
  HA comparisons. These tests are recorded in README progress entries.
- Latest upload continuity regression: 29 native checks, including one show and
  one hide across three requests and no intermediate parent re-enabling.
- Latest direct-EXE live upload acceptance remains open: the scoped test runtime
  ended during startup without exposing a targetable window; the retry was
  stopped by the user's Escape key. Its root cause is not established. The
  native loader/export check passed, but that does not establish GUI startup.
- The scoped local runtime retains generated 2.5.20 metadata and baseline
  archives. It is not a clean 2.5.22 release artifact.
- Keep the PR in draft until clean cross-platform CI and remaining live upload
  acceptance pass. Create/publish a release only after a separately authorized
  merge and verification of assets built from the exact release commit.

## Progress

- Updated source version from 2.5.21 to 2.5.22.
- Incorporated current main without modifying the dirty primary checkout.
- Release publication, tags and physical printer dispatch are not performed by
  this preparation.
