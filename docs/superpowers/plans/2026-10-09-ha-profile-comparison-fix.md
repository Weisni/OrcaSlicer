# HA profile comparison correction

## Evidence and scope

Fresh HA selections were still listed as profile uploads with matching names.
A read-only provider inspection confirmed affected legacy rolls stored a profile
name but no complete settings or nozzle variants. No HA data was written during
diagnosis. Same names cannot establish settings equality.

Two independent native regressions also reproduced comparison defects:

- An unchanged complete base profile was offered when its nozzle variant was
  absent. The selection guard then compared the absent variant baseline against
  the base digest, incorrectly rejecting a legitimate variant creation.
- Default preset lookup returned the selected edit buffer. Temporary settings
  could fail stored-profile identity checks or leak into initial HA storage.

## Correction

- Compare unchanged base fallback content by digest, retaining real edits.
- Validate upload selections against the exact variant baseline; first variant
  creation uses null expected digest, including the outgoing request.
- Resolve immutable HA identities and initial uploads from exact saved presets.
  Keep explicit project publication based on effective settings.
- On explicit selection of a name-only roll, offer initial storage and adopt
  the acknowledged HA copy. Preserve temporary changes over that base.
- Distinguish optional initial storage from different material settings in the
  synchronization dialog. Initial storage is unchecked by default.

## Verification

The defects were reproduced with failing tests before correction. The corrected
preset-linked native suite passes 303 assertions in 30 test cases, including
saved versus edited identity, absent-variant creation, identical downloaded
profiles, name-only legacy storage, and actual same-name setting differences.

An isolated Windows runtime was built outside existing installations (19 GUI,
2 core and 1 application translation unit). Windows successfully loaded its
fresh DLL and found the application entry export. The runtime retains the old
generated 2.5.20 version display; it is a scoped test build, not a clean release.
Rendered GUI behavior and live HA round-trip acceptance remain separate checks.
No live HA write, printer operation, installation, merge or release is included.
