# CLAUDE.md

OrcaSlicer — open-source C++17 3D slicer. wxWidgets GUI, CMake build system.

## Build Commands

```bash
# macOS
cmake --build build/arm64 --config RelWithDebInfo --target all --

# Linux
cmake --build build --config RelWithDebInfo --target all --

# Windows (replace %build_type% with Debug/Release/RelWithDebInfo)
cmake --build . --config %build_type% --target ALL_BUILD -- -m
```

## Testing

Catch2 framework. Tests in `tests/`; see [tests/AGENTS.md](tests/AGENTS.md) for where a new test belongs and the conventions to follow.

```bash
cd build && ctest --output-on-failure           # all tests
ctest --test-dir ./tests/libslic3r              # individual suite
ctest --test-dir ./tests/fff_print
```

## Code Style

- C++17, selective C++20. PascalCase classes, snake_case functions/variables
- `#pragma once` for headers. Smart pointers and RAII preferred
- Parallelization via TBB — be mindful of shared state
- Always use `SetSizerAndFit(sizer)` instead of `SetSizer(sizer)` on top level window. Unless `SetSizer` must be called before the full layout is built, call `sizer->SetSizeHints(window)` afterwards in this case.

## Key Entry Points

- App startup: `src/OrcaSlicer.cpp`
- Slicing pipeline: `src/libslic3r/Print.cpp`
- All print/printer/material settings: `src/libslic3r/PrintConfig.cpp`
- GUI: `src/slic3r/GUI/`
- Core algorithms: `src/libslic3r/` (GCode/, Fill/, Support/, Geometry/, Format/, Arachne/)
- Printer profiles: `resources/profiles/[manufacturer].json`

## Critical Constraints

- **Backward compatibility required** for .3mf project files and printer profiles
- **Cross-platform** — all changes must work on Windows, macOS, and Linux
- Profile/format changes need version migration handling
- Dependencies built separately in `deps/build/`, then linked to main app

## Code review focus areas

- Changes must not cause regressions in existing functionality, defaults, profiles, or project compatibility.
- Features gated by options must not affect existing behavior when those options are disabled.
- Changes should follow the existing code style and architecture. Architectural changes should be justified in code comments and the PR description.
- Add helper functions or utilities only when existing code cannot reasonably be reused. Avoid duplication.
- Keep code concise and clear. Manually simplify AI generated bloated codes before review.
- Include targeted tests or documented verification for behavior changes, especially in slicing logic, profiles, formats, and GUI defaults.
- For translation changes (`localization/i18n/**/*.po`), check that recurring terms match the [Localization glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/guides/localization_glossary.md) for that language.

## Localization & translations

Catalogs live in `localization/i18n/<lang>/OrcaSlicer_<lang>.po`; the template is `OrcaSlicer.pot`.
See the [Localization guide](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/guides/localization_guide.md) for the human-facing version of these principles.

### Terminology

- Use the [Localization glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/guides/localization_glossary.md) as the source of truth for recurring terms, so the same English term is always rendered the same way within a language, and terms that must stay in English (brand/product names, acronyms, materials, file formats, G-code tokens, macros/variables/identifiers) are not translated.
- If a term's established translation changes, update both the affected `.po` files and the glossary (`localization_glossary.tsv`, then regenerate) so they stay in sync.
- Translate the *meaning*, not the words. Check what the string actually controls before translating it — English reuses one word for different things. `Flow ratio` (multiplier), `Flow Rate` (throughput) and `Flow Dynamics` (pressure compensation) are three different terms; `extruder` may mean the toolhead, the feeder motor, or the nozzle depending on the string.
- Reuse one template per recurring message shape (`Failed to connect to …`, `Are you sure you want to …?`), even where the English wording varies.

### Editing rules

- Only edit `msgstr` — **never** change `msgid`, and never "fix" wrong English in the translation alone. Report the source string instead.
- Preserve exactly: placeholders (`%s`, `%d`, `%1%`, `%zu`, `%%`), every `\n` (count *and* position, including leading/trailing), leading/trailing spaces, HTML tags, `℃`, and the file's encoding and line endings.
- **Never reorder positional arguments** in a `c-format` string. If the msgid is `%d` then `%s`, that order must hold — swapping them breaks at runtime.
- `msgctxt` separates homonyms — always read it. `Back`/`Camera View` is the rear view of the 3D navigator, while `Back`/`Navigation` is the go-back button; `Top` exists in the *Alignment*, *Layers* and *Camera View* senses.
- When a string needs disambiguating, add context in the source (`_L_CONTEXT`/`_u8L_CONTEXT`), don't work around it in the translation.
- A literal `%` inside a string xgettext flagged `possible-c-format` will fail `msgfmt`. Fix it with a `// xgettext:no-c-format, no-boost-format` comment above the string in the source — do not mangle the translation or use `%%` in text that is never passed through printf.
- Plural entries: read `nplurals` from the catalog's `Plural-Forms` header (it is **not** always 2 — ja/ko/zh/th/vi use 1, ru/cs/pl/lt use 3, uk uses 4). Each form must be genuinely inflected for its quantity; repeating one sentence across all forms is a bug in Slavic/Baltic languages, though it is correct for Turkish and Hungarian.
- An entry whose `msgstr` equals its `msgid` is untranslated even though it is not empty; a plural entry with any empty form is likewise incomplete.
- Mark machine-produced translations with an `# AI Translated` translator comment. Don't add it to a human translation you didn't actually rewrite.
- Don't reflow or re-wrap unrelated entries — keep the diff limited to the strings you changed.

### Verifying

- `scripts/run_gettext.bat --full` (Windows) regenerates the template, merges every catalog and compiles the `.mo` files. It must exit 0.
- Or check a single catalog with `msgfmt --check-format -o <out>.mo localization/i18n/<lang>/OrcaSlicer_<lang>.po`.
- Fuzzy entries are not shown to users. If you correct one, clear its `fuzzy` flag, otherwise the fix never ships.


## QuackSlicer-specific agent instructions

This repository is QuackSlicer, the independent `Weisni/OrcaSlicer` fork.
Read this entire file, [working guidelines](docs/WORKING_GUIDELINES.md),
[README](README.md), and applicable nested `AGENTS.md` files before acting.
Consider these instructions before every command. Preserve the upstream
engineering/localization rules above; do not replace them with a generic template.

### Start and scope

- Inspect `git status --short`, remotes, branch/worktrees, current HEAD and
  current remote main before choosing a baseline. A version number does not
  prove upstream feature parity. Do not pull/reset/clean a dirty checkout.
- Keep unrelated tracked and untracked work and other worktrees intact.
  Feature integration belongs in an isolated `codex/` branch/worktree.
- Read the relevant source and tests before changing behavior. Code and Git
  documentation are English; user-facing findings/change summaries are German.
- Maintain README progress, open tasks and findings. Record new durable user
  instructions in `docs/WORKING_GUIDELINES.md`; distinguish proposals from
  accepted requirements and task-scoped historical permissions.
- Keep authenticated SSH sessions open while an authorized task needs them.
  Never write secrets to documentation, logs, fixtures or commits.
- Planning/research does not authorize integration, HA installation, physical
  heating/printing, merge, tags or publication. Follow the current user's scope;
  historical one-time deployments/prints are not standing authorization.

### Orca 2.5.0 Alpha integration handoff

Read these together before implementation:

1. [Design and complete feature/preservation matrix](docs/superpowers/specs/2026-10-30-orca-alpha-integration-design.md)
2. [Ordered implementation plan T0-T9](docs/superpowers/plans/2026-10-30-orca-alpha-integration.md)
3. [351-entry Alpha coverage ledger](docs/upstream/orca-2.5.0-alpha-coverage.csv)
4. [136 shared-path review ledger](docs/upstream/orca-2.5.0-alpha-shared-paths.csv)

Audit date: 2026-10-30. Proposed base is Quack 2.5.24 at
`2e38319f4e0762bc4cf44d46a28160e0fa2ae3f5`; target is upstream
`v2.5.0-beta` at `345efec64c7b95a05ce5cf96df0dde66a158835c` (user-selected replacement for the Alpha target).
Refresh remote main/open PRs/releases before execution. Regenerate the ledgers
if the base changes. The older primary checkout is not the implementation base.
The recommended approach is a complete tagged merge with reviewed Quack
adaptations; it remains a proposal until the implementation design is accepted.

- Integrate the entire tagged source/dependency/resource/profile tree. The 351
  release highlights do not enumerate every commit. Do not substitute nightly
  main, silently omit smaller features, or resolve files with blanket ours/theirs.
- Every shared path needs semantic review even if Git merges it automatically.
  Each coverage row needs tested disposition and evidence at the reviewed SHA.
- Deliver plugins/runtime/permissions, CAD, Speed Dial, mixing, texture geometry,
  belt/parallel/multi-nozzle modes, CLI, profiles and all fixes with working
  entry points, persistence and packaged dependencies on supported platforms.
- Preserve experimental labels and device restrictions. Any unsupported
  interaction is a documented blocker; do not claim full parity while hiding it.
- Before porting custom code, execute T0 and maintain the
  [Orca standard comparison matrix](docs/upstream/quack-orca-standard-comparison.md).
  Compare every custom behavior with unmodified official Orca and supply
  reproducible trials the user can run there. Distinguish full/partial stock
  coverage, thin adapters, custom plugins and missing equivalents. Preserve
  required outcomes; retire custom code only after verified comparison and the
  user's decision, with migration/rollback and regression coverage. Unresolved
  cases retain current behavior. Custom plugin migration is a separate choice.

### Quack behavior that must survive

- Physical-roll UUID identity, stock/reservations, QR/NFC mappings and idempotent
  print settlement; one roll may serve multiple material indices.
- Customer/order tracking, reversible archive/restore, editable jobs, costs,
  payment/invoice history and stable management selection/filtering.
- HA saved-profile authority, printer/nozzle/flow variants, digest/CAS/incremental
  sync, readable names, local unsaved overrides and legacy capability fallbacks.
- Job-bound preheat and atomic manual takeover, fresh final dispatch guards,
  cancellation/recovery, door invalidation, target-only versus plateau readiness,
  bed conditioning and one bounded additional 15-minute stabilization allowance.
  Never retry an uncertain physical dispatch or use plugin events as a second
  settlement authority.
- `enable_prime_tower_by_object`, `prime_tower_object_positions`,
  `color_penetration_mode` including `projected`, and all `wave_overhang*`
  settings. Optional paths must preserve existing disabled behavior.
- Grouped standard-3MF imports, correct object/volume/painted-face material
  assignments, preserved project/printer edits and deferred refresh crash fixes.
- Organic bottom-support interface contact/material/gap/collision corrections.
- Quack branding, data and credential namespaces, secure signed updater,
  release channel/URLs, executable versions and installer/portable resources.
- Newer local HA display-status/Print Again work must be reconciled explicitly:
  preserve observer identity, native accounting/reservations and settlement
  review; unknown actual consumption stays unknown.
- Non-planar work remains research unless separately requested. ZAA, belt
  transforms, Wave Overhangs and arbitrary conformal/multi-axis printing are
  different capabilities.

Virtual mixed filaments are not physical stock rows. Expand their real source
usage, support and purge into UUID allocations without double counting; preserve
units, rounding, estimates versus measured values, and historical accounting.
Nozzle-context changes must not silently pick a different roll/profile.

### Validation and handoff

- Follow `tests/AGENTS.md` and existing helpers. Add targeted behavioral
  regressions for slicing, formats, migrations and lifecycle changes; exercise
  enabled and disabled custom options. Register new tests in suite CMake files.
- Use clean dependency/app builds after ABI/toolchain changes. The Alpha uses
  bundled Python 3.12.13, CGAL 6.2.1, OCCT 8.0.1 and matching
  `SLIC3R_CAD=ON` dependency/application builds; use its `build_win.bat`
  path rather than assuming older Windows scripts still exist after integration.
- Run relevant native CTest suites plus the HA Python and JS UI checks in
  `.github/workflows/ha-materials-tests.yml`. Verify copied old/new projects,
  profiles and database migrations, and rendered GUI/package behavior.
- Use isolated data AND a distinct credential-service identity for test apps;
  `--datadir` alone does not isolate Windows Credential Manager.
- Keep software, GUI, external cloud/agent, installed HA and physical acceptance
  evidence separate. Source inspection/builds do not prove hardware safety,
  P2S compatibility, print quality or performance improvements.
- Preserve Windows/macOS/Linux and all supported architecture/package lanes.
  Full CI must pass at the reviewed head before release qualification. If
  publication is authorized later, verify the matching main build, tag,
  embedded executable versions and every delivered asset/hash.
- Update README, release notes and both ledgers with actual results/blockers.
  Never mark planned work or unperformed live/device tests as complete.


## Execution authorization and Beta target

On 2026-10-30 the user authorized implementation and an isolated local test build, then explicitly selected OrcaSlicer 2.5.0 Beta (345efec64c7b95a05ce5cf96df0dde66a158835c, published 2026-10-27). This supersedes the earlier planning-only scope and Alpha implementation target. Historical Alpha audit counts remain labeled snapshots. Keep the Alpha coverage ledger and additionally evaluate the Beta ledger and regenerated shared-path ledger. No production installation, live HA deployment, physical print/heating or release publication is authorized. No custom feature is retired without its comparison and user decision.
