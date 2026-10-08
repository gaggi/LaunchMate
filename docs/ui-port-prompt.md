# Prompt: move an app to the shared LaunchMate UI

Paste the text below into a Claude Code session opened in the other app's folder
(RaceMate or TPMate). Replace `<APP>` with the app's name.

---

<APP> should get the same user interface as LaunchMate. LaunchMate lives in
`C:\Users\gstur\Documents\GitHub\LaunchMate`.

1. Read `docs/ui-design.md` in the LaunchMate folder. It describes the layout,
   colors, controls and rules (sidebar pages, status banner, cards, settings rows
   with toggles, rows that expand for their settings instead of dialogs, pick lists
   as menus, checks as rows with pills, program-specific settings on their own
   sub-page, saving immediately, building inside the repository).
2. Copy LaunchMate's `src/ui` folder unchanged into this project's `src/ui`.
   Do not edit the copies; if a control needs to change, tell me, so it is changed
   in LaunchMate and copied again.
3. Look through this app's current windows and dialogs and propose a page
   structure for the sidebar: which pages, what goes into the status banner, which
   lists become cards or row lists, which settings go into expandable rows. Show me
   a mockup before you change code.
4. After I agree, rebuild the UI step by step, one commit per page, on a new branch.
   Keep every existing function and the settings file format. Build and run the
   tests after each step. Build into a folder under `build\` in this repository,
   not into `%TEMP%`; Defender quarantines exes run from there.
5. Set a git tag on the current state before you start, so we can go back.

Good examples in LaunchMate of how the controls are wired up:

- `src/MainWindow.cpp`: sidebar, status banner, pages, sub-page buttons
- `src/SettingsPage.cpp`: toggle rows and button rows
- `src/RuleAppsPage.cpp`, `src/WebhooksPage.cpp`, `src/PerformancePage.cpp`:
  expandable rows with `RowEditors`
- `src/RuleDisplayPage.cpp`: "Choose..." rows with `ChooseFromMenu`
- `src/IRacingPage.cpp`: program-specific sub-page, background check with pills,
  explicit "Save" row for another program's file
