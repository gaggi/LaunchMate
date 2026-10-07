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
   `SetFonts` on each control.
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
| `NavBar` | The sidebar. Page items stay highlighted; non-page items only send a command. Icons are Segoe Fluent glyphs. |
| `StatusPanel` | The banner at the top: neutral, active (green) or busy (amber), title, detail line and one primary button. |
| `CardList` | Overviews of objects (rules, profiles, sessions): title, subtitle, status pill, summary chips, chevron. A click opens the object. |
| `RowList` | Settings-style lists: section headers with cards of rows; a row can have an icon, detail, pill, toggle with label, one text button and icon buttons. A row can expand to show edit controls below its top line (`expandHeight`, `ExpansionRect()`, `kLayoutChanged`). |
| `SegmentedControl` | Switching between 2–4 views of the same data (for example sources of a list). |
| `PageWindow` | Base class for an embedded page: owns its controls, handles their messages, deletes itself with its window. |
| `ScrollHost` | Container for the current page; scrolls when the window is smaller than the page. |
| `UiTheme` | Colors, brushes, `Apply()` for native controls in dialogs, `IconFontFace()`. |
| `BackgroundTask` | Runs slow work (process lists, disk scans) on a worker thread; poll the result from a timer. Cancelling never blocks the UI. |

All controls notify their parent with `WM_COMMAND(MAKEWPARAM(id, code), hwnd)`;
the control tells which row or item was used (`NotifiedRow()`, `FocusedIndex()` ...).

## Patterns

- **Sidebar pages** for places the user visits: overviews, settings, libraries.
  Sub-pages (one object, one section of it) get a back button with the parent's
  name ("← iRacing") next to the page title.
- **Expand a row for an item's settings** (an arrow on the right, or a click on the
  row). The most important option stays a switch in the row itself; the expanded
  area holds the rest. Times are entered in seconds. Use a small modal dialog only
  where a row cannot hold the fields (for example a multi-line JSON payload).
- **Save immediately.** Toggles and buttons save at once; typing is saved 0.5 s after
  the last keystroke. If something cannot be saved yet, the page says what is
  missing ("Not saved yet: …") instead of showing a message box.
- **Confirm only what destroys data** ("Remove the rule for iRacing? …").
- **Status in the banner**, not in message boxes: what the app is doing, since when.
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
