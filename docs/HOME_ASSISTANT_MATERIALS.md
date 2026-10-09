# Home Assistant materials

The optional Home Assistant provider connects QuackSlicer's existing filament
dropdown and inventory manager to one authoritative Home Assistant inventory.
It does not replace the normal Bambu printer connection or send sliced files
through Home Assistant.

## Connect

1. Install and configure the [companion custom component](../tools/ha-material-demo/README.md).
   Pair its configured physical printer with the printer selected in Quack.
2. Open the HA connection control beside filament synchronization. Enter the
   component's materials URL, for example
   `http://homeassistant.local:8123/api/quack_material_demo/materials`, and an
   access token for a dedicated Home Assistant user.
3. Choose session-only credentials or storage in the operating system's
   credential store. Tokens are scoped to that server and are not written into
   projects, application configuration or synchronization journals.
4. Connect using the ordinary native printer login or LAN configuration as well.
   The HA material source and the physical printer must describe the same device.

The existing component/API domain contains `demo` for compatibility. The
provider option enables the normal native print workflow; the separate legacy
simulation option continues to block printer sends.

## Select and synchronize

Mounted usable rolls appear under Home Assistant by slot. Inventory contains all
active rolls with remaining stock, including unmounted rolls. The physical UUID
is saved independently of the project's filament profile and color. Changing a
local profile does not silently update the HA roll or remove its identity.

A profile published to HA contains the complete effective filament settings,
including compatibility conditions and material G-code, with a content digest.
The first refresh downloads all published profiles and nozzle variants into a
persistent content cache. Later refreshes reuse those digests and download only
missing contents. Compatible profiles are installed without changing the active
or edited preset. Equal effective settings share one managed profile regardless
of roll or display name; unrelated local profiles remain available. A profile
with a different settings schema is rejected, and an incompatible variant cannot
be selected for the current printer/nozzle.

Temporary local material adjustments remain possible without saving the project
or publishing to HA. Background refresh preserves them. When explicitly loading
a new HA base for the edited material, Quack overlays the settings changed from
the previous base, while unchanged settings follow the new base. Those changes
stay local until an explicit publication. Selecting a local profile for a roll
without a compatible HA variant uploads its saved baseline first, waits for
the committed HA association, and then uses the acknowledged HA-managed copy.
Closing the settings dialog does not discard those temporary settings. If
several project filaments share one edited preset and only some are replaced,
the remaining filaments retain that edit buffer. Explicitly replaced slots use
the saved HA settings. When the entire shared group is replaced, migrating its
overrides requires one common new base; conflicting replacements preserve the
project and report the ambiguity before any profile association is uploaded.
Multiple initial associations are published in one acknowledged batch after
all profile choices, so cancelling a later choice cannot partially upload them.
The profile chooser filters full names with a case-insensitive search field.

Stored profile identity checks use the saved preset rather than its selected
edit buffer. Temporary changes therefore do not make a downloaded HA profile
appear missing or modified. Project-to-HA publication still captures effective
settings, including temporary changes, when explicitly selected.

For older name-only associations, Quack resolves the compatible local profile
as a suggestion. Explicit HA roll selection offers initial full-profile storage
before adopting the acknowledged HA copy. Matching names alone do not prove
matching settings. Initial storage is labeled separately in the synchronization
dialog and unchecked by default. An unchanged complete HA base profile is not
an upload difference merely because its nozzle variant is absent. Changed
settings can create that variant using a null variant concurrency baseline.
Filament/material profiles are separate from
print/process profiles.

The synchronization dialog supports selected HA slots or all occupied slots.
Additional materials append without deleting surplus existing project indices.
Project-to-HA synchronization lists profile and color differences individually;
unchecked fields are preserved. Creating rolls or changing stock requires the
inventory workflow and an explicit quantity; a profile cannot imply grams.

## Printing and inventory authority

The standard print-start dialog still lets you select a customer order or
create a customer/order. Its physical roll rows are locked to the checked HA
UUIDs; local filament-profile overrides do not discard that physical identity.
The order and reservation must be acknowledged by HA before dispatch continues.

Use the native print-history editor or HA **Prints > Change order** to correct
an assignment later. This moves cost attribution without booking consumption
again. HA rejects stale edits, closed targets and currency mismatches. Reopen or
restore an order when required; a previously unassigned print is never assigned
by guessing its filename.

HA **Costs & invoices** shows matching job/order/customer calculations and
creates centrally saved invoice snapshots with explicit billing details.
Browser HTML/text export and Print / Save as PDF complement Quack's native PDF
export. Saved HA invoices remain fixed when assignments or current roll prices
are corrected later; current order totals update. No automatic paid status or
invoice-amount override is applied.

Quack checks the configured physical printer, fresh HA roll UUIDs, assigned
slots, material types, available stock and current native tray mapping before
sending a print. An unmounted inventory roll must first be assigned physically
and in HA. Selecting it for a project alone does not load filament.

Current native mapping support targets a P2S with AMS A1-A4, one AMS HT slot and
the external slot. Configure only the slots present on the printer. Additional
AMS units, other address maps and additional printer models need validation;
they are not inferred from names. The printer may report an approximate color;
the inventory's actual color remains authoritative.

HA commits shared inventory edits before the native cache changes. Same-row or
selected-field conflicts preserve newer data, while unrelated edits can merge.
Current components return a generation/revision marker from `/sync`. An
unchanged read does not scan or export the inventory. A changed read captures
one SQLite transaction and sends stable-key row upserts/deletions; Quack merges
and imports the complete result atomically, writing only changed local rows.
Server restart, generation mismatch or missing older delta history triggers a
full resynchronization. Components without `/sync` retain the earlier bounded
page protocol, which requires one revision across all pages.

Background HTTP runs outside the GUI thread with one request flow in flight.
Endpoint changes or explicit refreshes invalidate older results. A GUI timer
completes ready results even when no printer telemetry arrives; an open material
picker delays its list rebuild. An interrupted or stale read leaves the previous
local graph and temporary edits intact. Cached inventory and complete profile
contents can restore the roll list after an offline restart; printing still
requires the existing fresh HA checks and acknowledged reservations.

The graph budget is 64 MiB and 250,000 rows; provider metadata is limited to
4 MiB and a native row/profile payload to 256 KiB. The server retains at most
eight delta baselines and 64 MiB of Python objects. The profile memory cache
holds at most 64 payloads, with the remaining contents available on disk.

An uncertain network result keeps its durable request for reconciliation. A
retry resolves that request instead of starting another physical print. HA
retains reservation and job evidence while Quack is closed. Estimated and
measured consumption remain distinct; unknown quantities require review.

## QR, NFC and recovery

QR and NFC resolve the same physical roll UUID. In the official iOS Companion
app, Write NFC requests its native writer. Opening that dialog does not prove
that a tag was written. Link each phone explicitly in Scan & slots so a scan
only opens its own assignment view. Scanning does not itself load filament or
change stock. Physical NFC writing, cold starts and two-phone behavior require
device acceptance testing.

Back up inventory, projects and user presets before changing installations.
The first authoritative import preserves a local original-inventory JSON next
to the Quack user data; treat that backup as private operational data. Complete
workflow recovery requires the HA database, profile files, pending request
journals and a separate backup of the HA configuration. The recovery bundle
includes the database, profiles and journals, but deliberately excludes
configuration and credentials. Follow the component's recovery guide, keep the
active writers stopped during restoration, and verify the restored ledger
before resuming printing. Never publish these recovery files or access tokens.

## Validation and remaining checks

Automated coverage includes UUID persistence, stock reservations, slot mapping,
profile digest/compatibility checks, immutable downloads, asynchronous result
invalidation, unsaved local overrides, stale reads, large ledgers, selective
updates and idempotent receipts. Focused Windows compilation covers the changed
GUI translation units, rather than a complete linked application. Rendered
profile selection, offline restart and telemetry-independent refresh remain
acceptance checks. Native GUI and hardware behavior still depend
on the target OS, printer firmware and Companion app. Release CI, supported
platform builds, user-data migration and physical NFC acceptance remain
separate checks; passing unit tests does not establish those outcomes.
