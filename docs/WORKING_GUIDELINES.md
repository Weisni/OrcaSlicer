# Working guidelines

These guidelines record user-provided project instructions for later work.
Consider them before every command, together with the applicable `AGENTS.md`.

- Write code and Git-facing project documentation in English unless requested otherwise.
- Write change summaries, conclusions, and diagnostic reports in German.
- Maintain the root `README.md`, including open tasks, progress, and findings.
- Record durable working instructions in this file; distinguish proposals in
  implementation plans from accepted project requirements.
- Follow the repository's C++ style, cross-platform requirements, and test
  conventions. Preserve compatibility with existing projects and profiles.
- Optional features must preserve existing behavior when disabled. Profile or
  format changes require an explicit migration strategy.
- Preserve unrelated local work. The non-planar slicing investigation is research
  and planning; its feature implementation and physical validation remain future work.
- For order archiving and UI improvements, implement reversible archiving that
  preserves print history and costs, verify existing database migration, and
  review the QuackSlicer management workflows for practical usability issues.
- Keep order filters adjacent to their results, with compact actions and a clear
  visual hierarchy. Show status as the first order column and sort primarily by
  status, then alphabetically by the displayed order name. Validate the rendered
  interface with representative synthetic data, including filtering and resizing.
- For the automatic bed-arrangement task, keep the original
  `Downloads/Bottle Thread Game V2.3mf` unchanged and inspect/test only a copy.
  The user withdrew the additional adaptive/grid arrangement feature on
  2026-09-26 after finding the existing tool. Restore the original arrangement
  implementation; retain order archiving, unrelated UI changes, and the startup
  correction. The earlier arrangement implementation request is superseded.
- For the non-planar slicing task, research public primary sources, inspect the
  QuackSlicer baseline, and document a concrete integration plan with evidence,
  dependencies, risks, validation, and unresolved questions.
- P2S research must distinguish explicit P2S experience from other printer
  models, vendor claims from independent results, and software defaults or
  proposed experiment settings from measured hardware limits.
- For support-interface corrections, verify contact presence, material, gap,
  and extrusion-volume intersection with the model. Layer/path counts alone
  do not establish correct placement. Preserve source project snapshots and
  recheck the latest saved project when its settings or instances change.
- For standard 3MF color-import corrections, preserve the original input file
  and check object-level filament defaults alongside volume and painted-face
  assignments. A complete filament list alone does not establish correct mapping.
- HA material profiles are authoritative saved baselines. Publish a newly
  associated saved profile before using its HA copy; retain unsaved local
  material edits until explicitly discarded or synchronized.
- Show the original human-readable material profile name in HA filament labels
  and editors. Keep content hashes internal, preserve selection by internal ID,
  and restore display metadata for already downloaded profiles after restart.
- For HA performance acceptance, launch the test EXE directly with live data,
  import the supplied Whinny project with Replace and all parts in one group,
  check profile identities and empty comparisons after repeated spool changes,
  and record UI responsiveness separately from network completion time.
- The 2026-10-10 HA extension deployment and Core restart were explicitly
  authorized with backups. Verify the active component and its aliases before
  deployment; preserve newer NFC, payment, consumption and order features.

Source: user instructions and the root `AGENTS.md`, recorded 2026-09-24.

- The 2026-10-15 HA recovery authorizes SSH installation and the required Core
  restart. Keep authenticated SSH access open while needed, compare the active
  integration with the prepared patch, and back up live files and the database
  consistently. Preserve newer features, history, profiles and inventory. Never
  infer zero consumption from an aborted warm-up or start a physical print for
  this recovery. Diagnose invalid imported values independently of stuck jobs.

- For project-load crash fixes, preserve original 3MF files and production profiles, reproduce with a separate data directory, and verify complete GUI import rather than treating a CLI import or waiting version dialog as acceptance. Refresh the imported edited printer preset without reselecting or discarding it.
- A separate data directory alone does not isolate Windows Credential Manager. A startable local test variant must use its own application identity and credential-service name; preserve production credentials and verify the exact runtime before handing it over.
- For the explicitly authorized 2026-10-14 crash-fix release and print, use a copy of the Threaded-Divider-V2 project. Put only FitThread and its test TensionNut at the center of a new Cool Plate SuperTack plate. The user confirmed the clean installed plate and 0.4 mm TCHF nozzle. Verify the only black PLA in AMS 2 Pro (expected slot 4) before starting one print on Duck Poop P2S. Publish only the reviewed crash fix after required CI passes at the exact reviewed head; preserve unrelated work. Stop the affected step if hardware, material identity, or access cannot be verified.

- The 2026-10-15 preheat investigation and extension are local preparation only.
  Preserve Cancel and bed restoration when adding manual chamber-wait skipping.
  Analyze chamber history together with bed temperatures, print status and air
  mode; distinguish a measured plateau from a fixed device limit or heat-soak
  acceptance. Do not heat, start a print or install a new slicer for these checks.
- A P2S door opening must invalidate plateau evidence, including short open/close
  events between polls. Require temperature recovery and a new full window before
  plateau release. HA-controlled preheating is an evaluated alternative, not an
  instruction to install or activate a heater controller in this task.
- The requested HA preheat concept must cover cancellation, connection failures,
  manual operation and recovery actions, Quack crashes, two Quack instances and
  HA restarts. The user's current print is already running; concept work does
  not authorize changing that print or deploying a preheat controller.

- The later 2026-10-15 implementation request authorizes local job-bound preheat without a queue and implementation/staging in HA. It supersedes concept-only scope. Preserve the running print: no heater commands, printer-mode changes or Core restart affecting it. Verify staged versus active deployment and retain SSH access while needed.

- The user authorized live changes to Duck Poop's Bedienung dashboard section on 2026-10-15: expose temperature/fan controls and manual preheat activation. Preserve existing print guards and unrelated dashboard content, back up first, save through HA's supported editor, and verify the installed export. Do not send heating or print commands as an installation test.
- The subsequent UI correction keeps Duck Poop's Bedienung compact: show actual/target temperatures with pencil actions, remove individual fan controls, and reveal preheat target inputs only when editing. Preserve the existing print guards and manual-preheat ownership/safety behavior.
- The subsequent local test-build request authorizes a runnable isolated QuackSlicer build containing the corrected material chamber target and job-bound HA preheat. Prepare copied profiles and separate credential storage, retain production installation/settings/associations, and cap compiler workers at two. Enable the matching HA job-preheat mode only after the runtime is ready and the printer is confirmed idle; never start a physical print or heat as a build test.
- For the reported manual HA preheat failure, diagnose browser, durable attempt and printer feedback before changing the controller. Preserve user-started attempts and temperatures. Verify the HTTP browser environment explicitly; install a narrow backed-up correction and leave physical heating acceptance to an explicit user start.
- The later 2026-10-15 preheat correction explicitly keeps heating an existing job with an open door; door openings still invalidate readiness and require recovery plus a fresh plateau window after closing. The user also explicitly chose automatic restoration of the job target after a reported zero bed target; stopping requires cancelling the preheat job. Keep protection enabled, enforce the original expiry, require fresh idle telemetry and a confirmed restored target, and do not revive already terminal attempts.
- For the subsequent follow-up, manual preheat is advance warming and a matching print must take ownership atomically. Preserve project material selections and repair readable HA names after reopen. HA failures must leave the send dialog recoverable. Use the released 2.5.23 baseline and accurate test-version resources. The user explicitly requested full available PC performance for the rebuild, superseding the earlier two-compiler limit for this task. Preserve the running test session/profile and do not initiate a print.
- The later deadline correction explicitly permits one automatic additional stabilization allowance of up to 15 minutes when fresh idle telemetry and confirmed heating remain valid. It supersedes the earlier strict original-expiry rule only for this bounded allowance. Never revive terminal attempts or renew manual holds automatically. Door events still invalidate readiness and require recovery plus a fresh window. The user also authorized temporarily stopping manual preheat for installation; verify cancellation, idle status and zero heater targets before the Core restart.
- An already reached chamber target bypasses chamber plateau waiting. Preserve bed conditioning and all fresh dispatch/safety checks. If target-only readiness loses its target before dispatch, revoke it; do not confuse it with a proven plateau. After closing the door, a fresh temperature still at or above target is sufficient, while lower-temperature plateau release still requires recovery and a new window. Failed pre-restart checks must stop the dependent restart; never issue it unconditionally after a diagnostic tool call.

- The 2026-10-22 correction authorizes editing the recorded HA print status for
  every source and state, including physically running prints. Treat this as an
  audited display correction: preserve the observer link, native stock lifecycle,
  reservations and existing consumption. Unknown consumption must remain unknown.
  Keep explicit settlement and consumption review available independently of the
  display status. Printer-panel Print Again attempts must export complete native
  job rows, including legacy rows, without rewriting historical values.


## Orca Alpha integration request (2026-10-30)

- Audit the new official Orca Alpha and plan complete availability in Quack,
  explicitly retaining all user-introduced features. Maintain root
  `AGENTS.md` with actionable project and integration instructions.
- This request authorizes audit/planning/documentation. It does not request
  source integration, builds, HA deployment, physical commands or publication.
- The full-tag merge is a proposed approach; use the design, T0-T9 plan and
  coverage/shared-path ledgers together. Refresh the pinned baseline before
  implementation and preserve unrelated work in every checkout/worktree.
- Require explicit coverage for virtual mixes versus physical stock,
  nozzle/HA variants, plugin/agent lifecycle versus accounting/preheat,
  per-object towers, projected paint, Wave Overhangs and existing fixes.
- Before porting custom features, assess how far unmodified official Orca's
  standards can replace them. Prepare reproducible stock-Orca user trials,
  document full/partial coverage, missing behavior and adapter/plugin needs,
  and let the user choose adoption or custom-code retirement. Preserve required
  outcomes and unresolved custom behavior; no removal is implied by planning.

- The 2026-10-30 follow-up authorizes executing the integration plan and preparing an isolated local test build. The user selected the newer Orca 2.5.0 Beta. Retain production app/configuration/credentials and all custom behavior pending replacement decisions. This does not authorize production HA deployment, heating, physical sends or publishing.

- The 2026-10-30 acceptance follow-up requests connected HA checks across material
  selection, profile synchronization, local edits, nozzle/flow context and restart.
  Test the complete user-visible chains in the isolated Beta package, using a
  loopback fixture for writes and fault injection. Preserve the user's active
  project in a separate copy before changing its test state. The duplicated
  custom nozzle/flow control may be replaced by the native Beta control while
  preserving device restrictions, plate mapping and HA UUID/profile authority.
- For the 2026-10-30 material acceptance project, the user explicitly chose A4
  for physical filament 1; retain A3 on filament 2 and the virtual 50/50 mix on
  filament 3. This replaces the earlier pending Copper-versus-ASA clarification
  for this test project only. Preserve a separate copy of the current user state.
- On 2026-10-30 the user authorized extensive application validation followed by
  release publication only after the final checks pass. This supersedes the
  earlier local-build-only publication restriction. It does not authorize live
  HA installation, physical heating/printing or changing production profiles.
- The release-channel reply explicitly selects QuackSlicer 2.5.25 as a regular release, superseding the provisional 2.5.25-beta preparation. Keep the Orca Beta source basis explicit in the release notes; final publication still requires the agreed checks.
- The user explicitly selected a direct fast-forward of remote main after the
  candidate platform CI passes, rather than a pull request review. This applies
  only to the current 2.5.25 release task; subsequent main-build and package
  checks still gate publication. Preserve the dirty primary checkout.
- For the overnight continuation of the 2.5.25 release task, monitor the account's
  weekly Codex consumption and stop immediately at 50 percent used, with no
  further work or reset redemption. Prefer scripted verification and compact
  CI polling; use manual GUI checks only when automation cannot establish the
  required behavior. Publication remains conditional on the agreed release gates.
