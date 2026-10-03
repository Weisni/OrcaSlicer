# Home Assistant filament inventory

This package contains the optional Home Assistant custom integration used by
QuackSlicer's material provider, its loopback test host, regression tests and
recovery tools. The domain `quack_material_demo` is retained for compatibility;
`mode: pilot` creates an unseeded persistent inventory for actual use.

HA owns physical-roll UUIDs, stock, slot assignments, customer/order history,
print attempts and published material profiles. Quack keeps a replaceable read
cache and durable pending request identities. Printer communication still uses
Quack's ordinary native connection.

## Install and configure

1. Back up HA configuration and the existing inventory before upgrading. Copy
   `demo/custom_components/quack_material_demo` into HA's `custom_components`.
   Do not copy a development database, generated labels or another user's files.
2. Merge `demo/configuration-fragment.yaml` into configuration using your actual
   printer identity, HA user ID and sensor entity IDs. Use a dedicated user for
   Quack inventory access; administrator-only assignment/recovery/NFC enrollment
   actions remain separate. The example contains placeholders, not credentials.
3. Run HA's configuration check, restart Core and check integration diagnostics.
   Preserve the prior component/configuration and database backup for rollback.
4. Add `/quack-material-demo/inventory.js?v=0.9.0` as a JavaScript module in
   dashboard Resources. Copy `demo/quack-material-inventory-dashboard.yaml` to
   the HA configuration folder and register it as a YAML dashboard if desired.
5. Connect Quack through its HA connection dialog using
   `http://homeassistant.local:8123/api/quack_material_demo/materials` or your
   own server's equivalent. Quack can keep the token in the OS credential store
   or for this session only. Do not put tokens into YAML, projects or scripts.

The supported slot topology is A1-A4, one HT1 and EXT on the paired P2S. Set
`enabled_slots` to the subset actually present; default is all six. Unassign
occupied slots and resolve pending jobs/metadata writes before disabling them.
Additional printer models or AMS address maps are not automatically supported.

Coordinated printer metadata assignment is opt-in through `printer_assignment`.
It validates actual slot entities and idle status, updates the verified printer
substitute and real color, and retains the real HA material/profile/UUID. It
does not load or unload filament. Current substitute support is PLA/PETG and
TPU on the external slot; unsupported material/slot combinations stay blocked.

## Profiles, labels and recovery

Complete filament profiles transfer only through explicit selection or checked
synchronization. Their digests include effective settings and G-code; profiles
remain subject to printer/nozzle compatibility checks. Background polling reads
small profile summaries and never installs or applies settings.

QR and NFC share physical roll UUID identity. Link each Companion device in
Scan & slots before relying on device-specific NFC navigation. Write NFC opens
the official iOS app's writer; confirm writing there and test the physical tag.
A scan opens the assignment review without assigning or loading filament.

Use [RECOVERY.md](demo/RECOVERY.md) to export and verify a complete ledger,
profile and pending-request backup. Run its commands from this package folder,
where `demo/` and `tools/` are siblings. Recovery data contains private business
and material information and must stay outside source control.

## Orders, costs and invoices

The original customer/order chooser remains part of Quack's print-start flow.
HA-preflighted physical roll UUIDs remain fixed while selecting an order or
creating a customer/order. Cancelling does not reserve or dispatch a print.

After printing, use Quack's print-history editor or HA **Prints > Change order**
to assign, move or unassign a job. Both use HA as the writable authority. An
outdated edit is rejected rather than overwriting a more recent change.
Assignments never book material consumption again or alter the recorded
quantities, runtime or cost-rate snapshots. Reopen a closed target order and
restore archived orders before moving jobs. Personal/unassigned prints stay
outside customer invoices until explicitly assigned.

HA **Costs & invoices**, or an order's **Costs / invoice** action, shows the
same material, electricity, wear, maintenance, repair, design, other-cost,
waiver and discount calculations as Quack. Deliberate compatible-currency roll
price corrections affect current cost previews in both clients; historical
material identity and stored accounting rows are retained. Mixed currencies
are reported instead of silently combined. Estimated consumption and open-job
costs remain provisional.

**Create invoice** requires explicit issuer, recipient, number, dates and tax
treatment. HA stores an immutable snapshot with a unique invoice number and
safe retry identity; later order corrections do not rewrite it. Reopen saved
invoices to download HTML/text or use **Print / Save as PDF** in a supporting
browser. This does not mark an order paid or replace its separately recorded
invoice amount. The recovery bundle includes invoice snapshots and issuer
defaults. Quack's existing native PDF exporter remains available.

## Development checks

Python 3.12+ and Node.js are used by the regression suite. From this package
folder, install `demo/requirements.txt` in a virtual environment, then run:

```sh
python -m pip install -r demo/requirements.txt
PYTHONPATH=demo python -m unittest discover -s demo/tests -p 'test_*.py'
```

PowerShell equivalent for the test command:

```powershell
$env:PYTHONPATH = 'demo'
python -m unittest discover -s demo/tests -p 'test_*.py'
Get-ChildItem demo/tests/test_*.cjs | ForEach-Object { node $_.FullName }
```

The loopback host is started with `python demo/server.py`; use a separate
temporary database for tests. It deliberately does not expose a public network
listener. Never use its seeded demonstration stock as a production inventory.

## Progress and open checks

- Implemented: central stock/job authority, row/field conflict detection,
  permanent receipt identities, paged native snapshots, complete profile
  payloads, supported slot subsets and isolated recovery verification.
- Regression tests cover inventory, lifecycle, transport, access boundaries,
  profiles, browser controls and recovery. Native client tests live in the
  repository's `tests/libslic3r` suite.
- Cross-platform Quack builds, release assets and each installation's migration
  require separate verification. Physical NFC writing, cold starts, two-phone
  isolation and printer firmware interoperability require device acceptance.

The package intentionally excludes private deployment scripts, real inventory
snapshots, generated labels and uncalibrated demonstration filament presets.
