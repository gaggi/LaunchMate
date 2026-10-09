# UI design guide

LaunchMate, RaceMate and TPMate share one look: native Win32, no UI framework, a
sidebar with pages, white cards on a light gray background, and changes that save
themselves. This guide describes that look and the reusable controls in `src/ui`.

## Using the kit in another app

1. Copy the folder `src/ui` into the app's `src/ui`. The headers depend only on each
   other, the Windows SDK and the C++20 standard library.
2. Link `comctl32`, `uxtheme`, `shell32` and `gdi32` (most apps already do) and enable
   Common Controls 6 in the manifest.
3. Make the process per-monitor DPI aware (`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`).
   Every control scales with `GetDpiForWindow`; layout code works in DIPs (96 DPI units).
4. Create two fonts and pass them to the controls: text `Segoe UI` 9 pt regular,
   headings `Segoe UI` 10 pt semibold. Recreate them on `WM_DPICHANGED` and call
   `SetFonts` on each control. Embedded pages, their lists and row editors keep the
   font handles they were created with: recreate the shown page with the new fonts
   before deleting the old ones.
5. Keep source files ASCII and write other characters as escapes, for example
   `L"\u00B7"` for a middle dot or `L'\uE711'` for an icon glyph, and compile with
   `/utf-8`. Otherwise MSVC reads UTF-8 sources in the system code page and shows
   garbage such as "Â·" or broken icons.
6. Keep `src/ui` identical across the apps. Improve a control in one app, then copy it
   to the others, instead of letting copies drift apart.

## Window layout

```
+--------------+-----------------------------------------------------------+
| App name     |  [ Status banner: state, detail .......... [Primary] ]    |
|              |                                                           |
| > Page 1     |  Page title                                               |
|   Page 2     |  One sentence that explains the page                      |
|              |                                                           |
|   Page 3     |  +--- card ------------------------------------------+    |
|   Settings   |  | Row title                          (pill) [toggle]|    |
|              |  | Detail line                                       |    |
|              |  +---------------------------------------------------+    |
| Version x.y  |                                                           |
+--------------+-----------------------------------------------------------+
```

| Element | Size (DIP) |
| --- | --- |
| Sidebar width | 184 |
| Content margin left / right / bottom | 24 / 24 / 20 |
| Status banner | top 20, height 56 |
| Page content starts at | y = 92 |
| Page title + hint | 24 + 20, content below at +56 |
| Gap between columns | 16–20 |
| Minimum window size | 900 x 600 |

The window is resizable and remembers size, position and the maximized state.
Lists and cards take the available width; text-heavy pages (settings) stop at
about 760 DIP so lines stay readable.

## Colors

Defined in `UiTheme.h`; use these instead of new values.

| Use | Color |
| --- | --- |
| Window background | `UiTheme::Background` RGB(246, 247, 249) |
| Cards, inputs | `UiTheme::Surface` white |
| Text | `UiTheme::Text` RGB(40, 47, 58) |
| Secondary text | RGB(110, 112, 118) |
| Muted / disabled text | RGB(136, 135, 128) |
| Hairlines, card borders | RGB(220, 222, 225) |
| Sidebar | RGB(236, 238, 241) |
| Accent (focus, toggles on, selection) | RGB(55, 138, 221) |
| Active / success pill | fill RGB(234, 243, 222), text RGB(39, 80, 10) |
| Warning pill | fill RGB(250, 238, 218), text RGB(133, 79, 11) |
| Neutral pill | fill RGB(241, 239, 232), text RGB(95, 94, 90) |
| Accent pill | fill RGB(230, 241, 251), text RGB(12, 68, 124) |

Green means "running / active", amber means "needs attention / disabled",
gray means "idle". Do not use color as the only signal; pills always carry text.

## Controls in `src/ui`

| Control | Use it for |
| --- | --- |
| `NavBar` | The sidebar. Page items stay highlighted; non-page items only send a command. Icons are Segoe Fluent glyphs. `SetFooter(text, highlight)` changes the version line, for example to "Update available: 0.4.0" in the accent color. |
| `StatusPanel` | The banner at the top: neutral, active (green) or busy (amber), title, detail line and one primary button. |
| `CardList` | Overviews of objects (rules, profiles, sessions): title, subtitle, status pill, summary chips, chevron. A count (`trailing`) is a gray pill centered next to the chevron. A click opens the object. |
| `RowList` | Settings-style lists: section headers with cards of rows; a row can have an icon, detail, pill, toggle with label, one text button and icon buttons. A row can expand to show edit controls below its top line (`expandHeight`, `ExpansionRect()`, `kLayoutChanged`). Rows of one card share their icon button columns, so toggles line up; a `0` glyph is an empty slot. |
| `SegmentedControl` | Switching between 2–4 views of the same data (for example sources of a list). |
| `RowEditors` | The edit controls inside an expanded `RowList` row: `Begin(row)`, then `Label`, `Edit`, `Seconds`, `Check`, `Combo`, `Button` at DIP offsets inside the expansion (width 0 stretches); `Position()` after scrolling or a layout change, `Clear()` when the row collapses. `SecondsText` / `ParseSeconds` convert milliseconds and accept "1,5" as well as "1.5". |
| `PageWindow` | Base class for an embedded page: owns its controls, handles their messages, deletes itself with its window. `ChooseFromMenu(items, checked, disabled)` shows a pick list as a popup menu at the cursor (empty item = separator) and returns the chosen index or -1. |
| `ScrollHost` | Container for the current page; scrolls when the window is smaller than the page. |
| `UiTheme` | Colors, brushes, `Apply()` for native controls in dialogs, `IconFontFace()`. |
| `BackgroundTask` | Runs slow work (process lists, disk scans) on a worker thread; poll the result from a timer. Cancelling never blocks the UI. |

All controls notify their parent with `WM_COMMAND(MAKEWPARAM(id, code), hwnd)`;
the control tells which row or item was used (`NotifiedRow()`, `FocusedIndex()` ...).

## Patterns

- **Sidebar pages** for places the user visits: overviews, settings, libraries.
  Sub-pages (one object, one section of it) get a back button with the parent's
  name ("← iRacing") next to the page title.
- **Controls line up in columns.** Within a card, toggles, text buttons and icon
  buttons of every row sit in the same columns, even when only some rows can expand.
  `RowList` reserves as many icon button slots in each row as the row with the most
  has. `iconButtons[0]` is the rightmost slot; give the same button the same index in
  every row and fill gaps with `0`. For example, with move arrows on all rows and an
  expand arrow on some: `{expand, down, up}` on rows that expand and `{0, down, up}`
  on the others.
- **Expand a row for an item's settings** (an arrow on the right, or a click on the
  row). The most important option stays a switch in the row itself; the expanded
  area holds the rest, including multi-line fields such as a JSON payload. Times
  are entered in seconds. Only one row is expanded at a time; the arrow flips
  (`\uE70D` / `\uE70E`), a remove button (`\uE711`) sits right of it. Pages have no
  edit dialogs any more.
- **One kind of row per decision.** A yes/no setting is a toggle row; a choice
  among a few named options is a row with a "Choose..." button that opens
  `ChooseFromMenu` (current option checked, an explanation as a disabled item when
  the list is empty); numbers and text go into the expanded area. Group rows under
  section headers that say when they apply ("When iRacing starts", "When iRacing
  exits") or what they belong to ("Microsoft Defender").
- **Settings of one specific program get their own sub-page**, opened from a
  button in the object's header ("iRacing settings"), shown only when the object
  is that program. Generic pages (power plan and priorities) stay generic.
- **Checks are rows, not text boxes.** Each finding is a row with a pill: green
  for "fine" ("Highest refresh rate", "Excluded"), amber with the fix for
  "needs attention" ("144 Hz available", "X3D: try Balanced"). A "Check again" row
  at the end reruns the check in the background.
- **Files of other programs are saved explicitly.** When saving writes outside the
  app's own settings (a game's ini file) or needs a condition (the game is
  closed), use a "Save" row: the button is enabled only when there are changes and
  saving is possible, an "Unsaved" pill shows pending changes, and the detail line
  says why saving is blocked ("Close iRacing first") or what happens ("A backup is
  made first"). Results appear in that detail line, not in a message box.
- **Save immediately.** Toggles and buttons save at once; typing is saved 0.5 s after
  the last keystroke. If something cannot be saved yet, the page says what is
  missing ("Not saved yet: …") instead of showing a message box.
- **Confirm only what destroys data or weakens the system** ("Remove the rule for
  iRacing? …", "Stop Microsoft Defender from scanning these folders?"). The
  question names what changes, the risk, and that Windows asks for administrator
  approval if it does.
- **Status in the banner**, not in message boxes: what the app is doing, since when.
- **Updates without dialogs.** The version row in Settings shows the state: "Check now",
  "Checking GitHub...", an "Up to date" pill, or an "Update available" pill with an
  "Install update" button and an icon button for the release page. A check at startup
  only changes that row and the sidebar footer; a failed startup check stays silent.
- **Pick, don't type.** Offer lists to choose from (installed apps, running
  processes) with a search box and an "Add" button per row; "Browse…" is the fallback.
- **Explain in one line.** Every page has a one-sentence hint; every setting row a
  detail line that says what it does.
- **Long work runs in the background** (`BackgroundTask`) and the list shows
  "Loading…" meanwhile; the UI thread never waits for slow system calls.

## Writing

- English UI, sentence case ("Start with Windows", not "Start With Windows").
- Buttons start with a verb: "Add", "Check now", "Detect current".
- Short and concrete: "Asks to close, ends it after 3 s" rather than
  "Graceful close with forced termination after timeout".
- No "please", no exclamation marks, no "successfully".

## Building and trying a build

- Build exes that you or the user will run inside the repository, for example
  `cmake -S . -B build\<name> -G Ninja` and `cmake --build build\<name>`, never in
  `%TEMP%` or a tool's scratch folder. Microsoft Defender quarantined LaunchMate
  builds run from `%TEMP%` (`Behavior:Win32/Execution.A!ml`,
  `Trojan:Win32/Bearfoos.A!ml`): an exe in a temp folder that an elevated "start
  with Windows" logon task points to looks like malware persistence. The build
  then seems to vanish ("the exe is gone").
- A build that registers "Start with Windows" rewrites the logon task to its own
  path. After trying a test build, switch the option off and on again in the
  version that is normally used, so the task points to a stable location.
- Never add Defender exclusions for build folders to work around this.
- Code that writes files with non-ASCII text (tools, scripts) can turn `\u`
  escapes back into literal characters; check that sources are still ASCII
  before building.
