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
An explicit selection downloads that exact revision and installs a distinct
content-addressed user profile. An unrelated local profile with the same name
is preserved. Background refreshes only read summaries. Profiles with a
different settings schema or incompatible printer/nozzle are rejected.

For older name-only associations, Quack uses the exact compatible installed
profile. If it is absent, a compatible installed Generic profile of the same
material type is used. That derived fallback is not silently published as a
new authoritative association. Filament/material profiles are separate from
print/process profiles.

The synchronization dialog supports selected HA slots or all occupied slots.
Additional materials append without deleting surplus existing project indices.
Project-to-HA synchronization lists profile and color differences individually;
unchecked fields are preserved. Creating rolls or changing stock requires the
inventory workflow and an explicit quantity; a profile cannot imply grams.

## Printing and inventory authority

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
Paged snapshots are imported only when every table belongs to the same server
revision. Interrupted or stale reads leave the previous local graph intact.
Current client budgets are 64 MiB and 250,000 rows per complete graph; this is
not an unlimited-memory import.

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
workflow recovery also includes the HA database, profile files, pending request
journals and configuration. Follow the component's recovery guide, keep the
active writers stopped during restoration, and verify the restored ledger
before resuming printing. Never publish these recovery files or access tokens.

## Validation and remaining checks

Automated coverage includes UUID persistence, stock reservations, slot mapping,
profile digest/compatibility checks, stale reads, large ledgers, selective
updates and idempotent receipts. Native GUI and hardware behavior still depend
on the target OS, printer firmware and Companion app. Release CI, supported
platform builds, user-data migration and physical NFC acceptance remain
separate checks; passing unit tests does not establish those outcomes.
