# Prompt: move an app to the shared LaunchMate UI

Paste the text below into a Claude Code session opened in the other app's folder
(RaceMate or TPMate). Replace `<APP>` with the app's name.

---

<APP> should get the same user interface as LaunchMate. LaunchMate lives in
`C:\Users\gstur\Documents\GitHub\LaunchMate`.

1. Read `docs/ui-design.md` in the LaunchMate folder. It describes the layout,
   colors, controls and rules (sidebar pages, status banner, cards, settings rows
   with toggles, saving immediately, small dialogs only for single items).
2. Copy LaunchMate's `src/ui` folder unchanged into this project's `src/ui`.
   Do not edit the copies; if a control needs to change, tell me, so it is changed
   in LaunchMate and copied again.
3. Look through this app's current windows and dialogs and propose a page
   structure for the sidebar: which pages, what goes into the status banner, which
   lists become cards or row lists, what stays a small dialog. Show me a mockup
   before you change code.
4. After I agree, rebuild the UI step by step, one commit per page, on a new branch.
   Keep every existing function and the settings file format. Build and run the
   tests after each step.
5. Set a git tag on the current state before you start, so we can go back.

LaunchMate's `src/MainWindow.cpp`, `src/SettingsPage.cpp` and
`src/RuleAppsPage.cpp` are good examples of how the controls are wired up.
