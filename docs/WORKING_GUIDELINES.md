# Working guidelines

These guidelines record user-provided project instructions for later work.
Consider them before every command, together with the applicable `AGENTS.md`.

- Write software and project documentation in English unless requested otherwise.
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
- For upstream synchronization, review open pull requests first and prioritize
  official stable OrcaSlicer and Bambu Studio releases over development-branch
  snapshots. Present interesting development-branch changes for an explicit
  decision unless they were already approved for the next update.
- Keep local Windows builds below full machine saturation. Use at most two
  parallel MSBuild processes for sync and release validation; rely on GitHub
  Actions for the other supported platforms and publication artifacts.

Source: user instructions and the root `AGENTS.md`, recorded 2026-09-24.

- For the requested 2.5.21 release, merge the scoped HA callback repair through
  reviewed, passing cross-platform CI; publish only artifacts matching the final
  main commit. Preserve unrelated arrangement work and user profiles. Physical
  print dispatch and the nozzle-match indicator remain separate validation.
