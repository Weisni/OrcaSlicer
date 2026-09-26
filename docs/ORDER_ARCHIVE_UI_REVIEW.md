# Order archiving and management UI review

Updated: 2026-09-26

## Behavior

In **Filaments > Orders**, select a completed or cancelled order and
choose **Archive order**. Orders with unresolved print jobs cannot be archived.
The default **Not archived** view hides archived orders. Choose **Archived** or
**All orders** to inspect them and use **Restore order** to return them to the
unarchived list. Restoration preserves the completed/cancelled status.

Search matches order number, title, notes, and customer name without regard to
case. A short debounce avoids rebuilding the lists on every keystroke. The list
shows the number of matching orders and explains empty results. Newly created
orders are selected in the unarchived view, with previous search and status
filters cleared. Customer administration is available under **Filaments > Customers**.

Archived orders remain readable through material details and invoice export.
Their rows retain the original status and add an archived marker. Double-click
opens invoice details. Editing, cost recalculation, deletion, and changes to
linked print jobs require restoration. New print assignments exclude archived
orders. Bulk recalculation scopes cover unarchived orders independently of the
current search/filter.

## Order workspace redesign (2026-09-26)

The previous combined page gave customers and orders competing tables and put
three toolbar rows between them. Default `wxWrapSizer` flags also stretch the
last control on every row, explaining the excessively wide buttons. The large
blank area had no explicit spacer; cached minimum heights in the nested wrapping
toolbars were the identified layout risk. The replacement separates the two
workflows and gives only the order results list a stretch proportion. Rendered
checks confirmed the filters sit directly above the table in small and maximized
windows. The header and selection footer are compact; name/customer columns gain
space on wider windows while filters and actions remain close together.

- **Orders** has a compact heading with Add order, Refresh, and Cost settings.
  Search, status, and archive filters sit directly above the table. Controls use
  the existing themed Button component; retained wrapping bars no longer stretch
  their last button across the available width.
- **Status** is the first column, displayed as a badge with a readable text label.
  Default sorting is **Active, Draft, Completed, Cancelled**, then the complete
  displayed order label (number and title) alphabetically without regard to case.
  Order ID is the final tie-breaker. Archived records retain their original status.
- Eight columns are visible initially: Status, Order, Customer, Print jobs,
  Duration, Internal cost, Calculated invoice, and Invoice. **Cost details** reveals
  seven additional columns: Weight, Material cost, Electricity estimate,
  Wear & reserves, Design & other, Discount, and Quoted.
- A footer identifies the selected order and groups its detail/invoice actions
  separately from lifecycle actions. Enablement continues to enforce the existing
  draft, open-job, completion, and archive rules. Selection is restored by order ID
  after refresh or filtering; sorting the backing vector keeps actions mapped to
  the displayed records.
- **Customers** has its own table and compact actions. Selecting a customer there
  prefills the next new order. Customer totals still include archived orders.

## Compatibility

Inventory schema 8 adds `customer_orders.archived`, defaulting to false. Existing
orders, IDs, statuses, job links, stock movements, and cost fields survive the
migration. Customer totals continue to include archived orders. Archiving itself
does not recalculate or remove any costs. Existing live filament-price behavior
remains unchanged; archiving is not an immutable accounting snapshot.

Project files, printer profiles, and slicing behavior are unchanged. Older
applications that support only inventory schema 7 cannot open the migrated
inventory; use a database backup if a downgrade is required.

## Reviewed workflows and improvements

| Workflow | Finding and result |
| --- | --- |
| Spool inventory and tags | Long action bar now wraps; stock/reservation semantics remain unchanged. |
| Open print jobs | Action bar wraps; selection survives service/manual refresh. |
| Stock history | Existing event journal reviewed; the 1,000-record display limit remains a follow-up. |
| Job history and editing | Selection survives refresh; archived order names remain visible; archived jobs are read-only until order restoration. |
| Customers and orders | Added archive/restore, filter/search, empty guidance, stable selection, and state-aware actions; redesigned as separate workspaces on 2026-09-26. |
| Material allocation | Existing matching/reservation logic reviewed; new order selectors exclude archived orders. |
| Material breakdown | Read-only access remains available for archived orders and jobs. |
| Customer/order/cost forms | Scrollable contents keep OK/Cancel reachable; initial sizes and minimums respect available monitor space. |
| Invoice form/export | Scrollable form keeps totals/actions visible; VAT choice disables while small-business mode is selected. |
| Navigation | Manager description now names customers, orders, and invoices; main Filaments navigation label retained for continuity. |

This is a source review of the fork's management workflows and their integration.
It does not claim exhaustive interactive coverage of inherited upstream slicing,
calibration, device, networking, or printer-specific features.

## Historical verification (2026-09-24)

- Regression tests cover completed/cancelled archive round trips, persistence,
  schema-7 migration, open-job restrictions, archived mutation guards,
  idempotent lifecycle callbacks, and retained stock/history/cost/invoice data.
- Inventory and lifecycle regression suite: 49 test cases / 695 assertions passed
  with randomized order and seed `24092026`. The service tests use the same
  `FilamentInventory` tag. Log: `build/order-archive-tests.log`.
- Windows Release build of `OrcaSlicer`, `OrcaSlicer_app_gui`, and
  `libslic3r_tests`: passed. The final UI rebuild after search debounce changes
  hit Windows virtual-memory exhaustion on its first attempt and passed with
  `/m:1 /p:CL_MPCount=2`. Final log: `build/order-archive-ui-build-limited.log`.
- Built application: `build/src/Release/quack-slicer.exe` with the adjacent
  `OrcaSlicer.dll` and runtime/resources. The installed app was not updated.
- Isolated GUI smoke checks: blocked at startup before the management screen
  could be exercised. A separate `--datadir build/order-archive-ui-data` profile
  selected stock P1P presets and contained no real customers, jobs, or printer
  connections. Both launch attempts exited before verification. The second
  returned `0xC0000005`; its crash stack is in `ntdll` allocation and NVIDIA
  `nvoglv64.dll` frames. Those frames did not establish the root cause. Subsequent
  investigation identified stale, incompatible build objects; see
  [Windows startup diagnosis](WINDOWS_STARTUP_DIAGNOSTIC.md). Historical evidence:
  `build/order-archive-ui-data/log/crash_Thu_Sep_24_17_22_10_0.log`.
- These checks did not visually verify management layouts or archive-button behavior.

## Current UI acceptance (2026-09-26)

The isolated profile at `build/order-ui-review/gui-data` contains only synthetic
inventory records: three customers, twelve orders (ten unarchived and two
archived), four jobs, and one spool. It covers mixed-case names, all four statuses,
long titles/notes, optional prices, and open-job restrictions. Setup is reproducible
with `build/order-ui-review/prepare_fixture.py`; exact IDs and expected results are
recorded in `build/order-ui-review/fixture.json`. SQLite integrity and foreign-key
checks passed during fixture preparation. This is fixture validation, not GUI
acceptance.

- [x] Build the revised workspace: Windows Release passed; the retained inventory
      suite passed 49 cases / 695 assertions, seed `26092026`.
- [x] Verify Status-first rendering and status/name order with the synthetic records.
- [x] Check combined status/archive/search filters, mixed-case and customer search,
      hidden-note search, empty results, and selection preservation after refresh.
- [x] Archive and restore synthetic order `ui-order-05` through the GUI. Verify
      completed status, all twelve orders, and its linked job survive the round
      trip; SQLite integrity remains valid. Check disabled archived edits,
      available invoice access, and disabled completion for unresolved jobs.
- [x] Check eight default columns, the seven-column Cost details toggle, long
      selected titles, and compact footer actions at 1280x720; verify tab switching
      and a maximized view. The final compact table needs no horizontal scrolling
      at 1280x720; status badges include the full archived label.
- [x] Check light and dark themes on Windows at the current display scale.
- [ ] Complete keyboard-only navigation, increased DPI, and repeated resize stress checks.
- [ ] Recheck the complete archived invoice/material dialog flows and exports.
- [ ] Complete macOS/Linux and additional-language visual checks.

Evidence: `build/order-ui-review/build-verified.log`, `inventory-tests.log`,
`archive-roundtrip.json`, and the `orders-light.png` / `orders-dark.png` previews.
Only synthetic profile databases were changed during UI checks. The user's
inventory and original Bottle Thread Game project were not used or modified.

## Follow-up tasks

- Expose archived customer/spool visibility and recovery as a separate workflow.
- Indicate history limits or provide paging/search for older records.
- Move technical spool identifiers behind stock information in the default table.
- Consider moving order/customer navigation outside Filaments if management grows.
- Measure filtering performance with large inventories before introducing caches.
- Review the allocation dialog's minimum size on smaller/high-DPI displays.
