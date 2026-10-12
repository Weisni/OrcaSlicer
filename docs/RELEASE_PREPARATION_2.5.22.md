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
- Loading regression: 34 native Windows checks pass, including continuous
  loading across three requests, background work, responsive paint/cancellation,
  owner restoration and modal review completion. Both foreground regressions
  failed before their corrections. Restore owners before hiding loading, and
  finish loading before ending its modal review owner. Independent review
  approved both lifetime corrections.
- Direct-EXE live tests now reach the GUI and confirm a complete PETG profile
  publication for the P2S 0.4 high-flow context, readable names, an empty
  comparison after publication, and a reversible price change (30 to 31 and
  back). HA read-back confirms all unrelated rolls and physical slots unchanged.
  The foreground corrections still require their final live repetition.
- GitHub had automatically disabled this fork's workflows. They were re-enabled
  without changing allowed Actions permissions. HA CI passed at the preceding
  revision; clean supported-platform checks must pass on the final PR revision.
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

## Preparation verification

- Passed: 258 Python HA integration tests and all bundled browser CJS tests.
- Passed: both configured HA CTest targets; 359 profile/binding assertions in
  37 cases, 71 transport assertions in 16 cases, 1,003 inventory assertions in
  51 cases, and 20 import assertions in one case.
- Passed: 34 native Windows loading-window checks and 10 profile-search checks.
- Independent review found that internal managed HA presets/project copies
  were offered as saved local templates. The fallback picker now excludes them
  using the existing managed-identity validator, so their hashes cannot become
  publication names. Normal saved local profiles remain searchable; linked HA
  profiles still resolve/download through the existing authority path.
- The chooser correction is verified by compiling its production GUI consumer
  and rerunning the native search/profile-binding regressions. Full GUI/live
  acceptance remains an explicit release gate.
