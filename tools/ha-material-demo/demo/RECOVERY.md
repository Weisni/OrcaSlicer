# Inventory and material-profile recovery

Home Assistant is the writable authority. Quack's inventory database is a replaceable cache, but its pending request journals and project-local filament profiles must be preserved. A successful print books the sliced estimate once; it does not establish a weighed consumption measurement.

## Export

1. In HA, open **Filament & Orders → Synchronization → Export recovery bundle** as an administrator. The JSON contains the ledger, UUIDs, assignments, profile payloads, permanent request receipts and print observations from one database transaction. It excludes HA configuration and credentials.
2. Close Quack before capturing its user-data. Retain your project `.3mf` files separately.
3. Combine the HA export with Quack's filament profiles and pending journals:

   ```sh
   python tools/filament_workflow_recovery.py create --ha recovery.json --quack-data /path/to/quack-user-data --output workflow-recovery.zip --quack-closed
   ```

Only `user/*/filament/**/*.json` and these four HA pending-request journals are included from Quack:

- `ha-explicit-pending.json`
- `ha-project-material-pending.json`
- `ha-authority-inventory-pending.json`
- `ha-authority-job-pending.json`

Account, network and application settings are excluded. The archive remains private: it contains inventory, customer/order history and user-written profiles or G-code. Keep a separate HA configuration backup for integration settings and credentials. The operating system credential store is not exported.

## Verify and restore in isolation

```sh
python tools/filament_workflow_recovery.py restore workflow-recovery.zip new-recovery-directory
```

This checks member paths and hashes, restores the ledger into a new SQLite database, runs database integrity and reference checks, and verifies nonnegative available stock. It restores profiles and journals into a separate `quack` folder without loading presets or replaying requests. An existing destination is never replaced. A failure is not a successful restore; preserve the source archive and inspect the reported error before using any output.

For a ledger-only export:

```sh
python tools/restore_filament_recovery.py recovery.json new-inventory.sqlite3
```

Before switching live data, stop the integration, back up its current database/configuration and compare roll UUIDs, balances, outstanding reservations, dispatches and profile hashes. Configure the original supported printer and slots; the recovery file deliberately does not carry the physical printer connection or access settings. Preserve pending request keys exactly. Never manufacture a new key to retry an uncertain write or repeat a physical print.

## Review and provenance

- A correlated successful printer completion settles the reserved sliced quantities once and releases reservations even while Quack is closed.
- A confirmed failed attempt can be reconciled from **Prints** by entering consumption for each originally allocated roll, including explicit zero, and selecting **Estimated** or **Measured**. Material/profile/price history remains attached to the original print.
- An idle transition alone or an ambiguous send is not proof of success or zero usage. Such attempts remain pending until their physical outcome is known.
- Reconnection refreshes Quack from HA. Offline inventory writes cannot commit; uncertain requests retain their durable identity for safe reconciliation.
- Profile settings, including G-code, transfer only through explicit selection/synchronization. Restoring a file does not execute it.

## Validation status

Automated restore tests cover reserved jobs, once-only completion after restore, corrupted payloads, missing/extra archive members, path traversal, preserved profile/journal bytes and refusal to overwrite existing destinations. Live acceptance must additionally compare the exported production ledger with its isolated restore; a local test database is not a production restore exercise.
