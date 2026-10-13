# Project-load crash correction

Loading a saved P2S 3MF could crash while refreshing the printer tab after import.
The deferred callback selected the printer preset again even though import had
already populated its edited configuration. That could open an unsaved-changes
dialog during a progress update and discard the imported settings.

The callback now refreshes the printer tab with `update_tab_ui()`. Import has already selected the preset;
`load_current_presets(false, false)` has built its pages. Sidebar refresh and physical-printer deselection remain in place.

## Validation and remaining work

- Static review found no additional required preset selection in this callback.
- An initial GUI diagnostic loaded the unchanged Threaded-Divider-V2 project:
  nine objects, three plates, and two material slots; the process remained alive.
- The freshly rebuilt isolated Windows application loaded the original project
  completely and remained alive for 90 seconds. Imported 0.4 mm High Flow,
  PLA/TPU materials and colors were retained.
- The original project, production configuration and Windows file/URL
  associations remained unchanged; synthetic credential isolation checks passed.
- Fresh regression executables are still being built; their suite and CI
  remain mandatory checks before merge.
- Required cross-platform CI must pass at the reviewed commit before merge.
- Release 2.5.23 must target the verified main build commit; verify its assets
  before publishing.

The local diagnostic app identity, credential-service isolation, and logging
are test-only changes and are excluded from the production patch.
