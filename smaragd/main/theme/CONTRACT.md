# app/theme — the widget style

**Layer:** `app_ui`. **Depends on:** nothing in `app/` (a LEAF), and `tw/core`
for `TW_LOG` only.

## Purpose

One warm, compact, dark widget style that looks the SAME on Windows, macOS and
Linux, and the one function that installs it. Three files:

| Public header | What it is |
|---|---|
| `app/theme/sthemecolors.h` | `SThemeColors` — the colour TOKENS, by role, plus `palette()`, the projection of those roles onto Qt's 20. `brownProDark()` is the shipped set. |
| `app/theme/sbrownprostyle.h` | `SBrownProStyle` — a `QProxyStyle` over **Fusion** that progressively replaces Fusion's elements. |
| `app/theme/stheme.h` | `STheme::resolve()` / `STheme::apply()` — which style, and installing it. |

## Invariants

1. **The base style is Fusion, never `QCommonStyle`.** `QCommonStyle` is the
   shared arithmetic concrete styles are built on, not a style you can ship:
   deriving from it means every element not yet written paints as an unshaded
   rectangle or not at all, so an eight-element style makes the other two hundred
   widgets WORSE than the platform default. Fusion is in every Qt build we ship
   against, draws almost everything out of `QPalette`, and is what the elements
   below REPLACE — so coverage grows one element at a time with the app looking
   whole at every commit.

2. **`theme` includes no other `app/<module>` header, and gains none.** Not
   `servicesui` for its own option key and not `shell` for `SSettings`: the
   caller reads the preference and passes the resolved name in. A style pulled
   into the app's dependency cycle could not be reasoned about — or unit-tested —
   without an INI, an `SApplication` and a window. Enforced by
   `tools/check_layering.py` (`'theme': set()`).

3. **The palette travels WITH the style**, via `polish(QPalette&)`. There is no
   second `QApplication::setPalette()` to keep in step, and switching back to a
   platform style restores that style's palette instead of leaving ours behind.

4. **`apply()` runs before the first widget exists.** `QApplication::setStyle()`
   re-polishes what is already built, and a widget polished by two styles can
   keep the first one's palette. The call site is `main.cpp`, after the font
   block and before `new SMainWindow`.

5. **No colour literal outside `sthemecolors.cpp`.** `SBrownProStyle` holds an
   `SThemeColors` by value and names colours by ROLE, which is what makes a
   second palette a new factory function rather than a second style class.

6. **Every `sizeFromContents` adjustment is a FLOOR, never a cap.** A minimum
   keeps a compact metric from producing a control too small to hit; a cap would
   clip a long label or a large font.

7. **A 1 px outline is stroked on the half-pixel inset** (`crisp()`), or it lands
   on a pixel boundary and reads as a 2 px blur.

8. **The headless default is `system`.** `STheme::resolve(configured, testCase)`
   defaults to `"system"` under `--test-case`, so no committed pixel expectation
   (`assert-track-head`, `assert-lane-alignment`, `meter_levels`' head PNGs,
   `folder_sum_preview`'s canvas gate) moves without a theme gate to say which
   moves were intended. `SMARAGD_UI_THEME` still wins, which is how a future
   theme case asks for the real style.

## What this style paints, and what is still Fusion's

Add to this table when you add an element — it is the coverage map.

**Replaced here**

| Group | Elements |
|---|---|
| Buttons | `PE_PanelButtonCommand`, `PE_PanelButtonBevel`, `PE_PanelButtonTool` |
| Check / radio | `PE_IndicatorCheckBox` (incl. tri-state), `PE_IndicatorRadioButton` |
| Text entry | `PE_PanelLineEdit`, `PE_FrameLineEdit` |
| Frames | `PE_Frame`, `PE_FrameGroupBox`, `PE_FrameTabWidget`, `PE_FrameDockWidget`, `PE_FrameStatusBarItem`, `PE_FrameFocusRect`, `CE_ShapedFrame` |
| Item views | `PE_PanelItemViewItem`, `PE_PanelItemViewRow` (selected / hovered only), `PE_IndicatorBranch` |
| Menus | `PE_PanelMenu`, `PE_FrameMenu`, `CE_MenuBarItem`, `CE_MenuBarEmptyArea` |
| Toolbars / chrome | `PE_PanelToolBar`, `CE_ToolBar`, `PE_IndicatorToolBarHandle`, `PE_IndicatorToolBarSeparator`, `CE_Splitter`, `CE_DockWidgetTitle` |
| Arrows | `PE_IndicatorArrow{Up,Down,Left,Right}`, `PE_IndicatorSpin{Up,Down,Plus,Minus}`, `PE_IndicatorButtonDropDown`, `PE_IndicatorHeaderArrow` |
| Progress | `CE_ProgressBarGroove`, `CE_ProgressBarContents`, `CE_ProgressBarLabel` |
| Tabs | `CE_TabBarTabShape`, `CE_TabBarTabLabel` |
| Headers | `CE_HeaderSection`, `CE_HeaderEmptyArea` |
| Complex | `CC_ScrollBar` (paint **and** `subControlRect` — no steppers), `CC_Slider`, `CC_ComboBox`, `CC_SpinBox` |

**Still Fusion's, deliberately for now:** `CE_MenuItem` (Fusion's layout is
correct and reads well from our palette — the highlight is `Highlight`, i.e.
the accent), `CC_ToolButton` (its panel is ours via `PE_PanelButtonTool`),
`CC_GroupBox`, `CE_PushButtonLabel` / `CE_ComboBoxLabel` / `CE_ToolButtonLabel`
(overriding those would drop icon handling for a colour the palette already
supplies), `CE_ItemViewItem`, `CE_SizeGrip`, `CE_RubberBand`, tooltips, and
every `subElementRect`.

## Two things this style does NOT do

- **It does not paint the arranger.** Clips, lanes, waveforms, meters and the
  ruler are custom `paintEvent()` code with their own colours, some of it under
  pixel gates (proposals 34 and 39). A `QStyle` is the CHROME around that.
- **It does not reach a widget that carries its own `setStyleSheet()`.** Qt
  routes such a widget through `QStyleSheetStyle` instead. The call sites today
  are `timeline/src/ssmvmixercontrol.cpp` (the M/S/arm/takes/group/instrument
  glyph buttons), `timeline/src/sstdmixerview.cpp` (the drop indicator),
  `timeline/src/strackdetailpanel.cpp` and `pluginui/src/splugineffectstrip.cpp`.
  Migrating those to these tokens is the obvious next step and is NOT done.

## Threading

Main thread only. `QStyle` methods are called from `paintEvent`, which is the GUI
thread by construction; nothing here holds state across a call except the
immutable `SThemeColors`.

## How to test

`SBrownProStyle` has no gate. `STheme::resolve()` is a pure function of
`(configured, testCase, SMARAGD_UI_THEME)` and is the piece worth a unit test
first. A PIXEL gate would need a verb that builds a specific widget off screen
and grabs it — `assert-track-head` and `assert-lane-alignment` are the existing
shape — plus `SMARAGD_UI_THEME=brownpro` on that one CTest entry.

## Known debt

- No gate of any kind (above).
- No options-page control; `ui/theme` is INI-only.
- One theme. `SThemeColors` is built for a second (a light one, a user accent)
  and nothing consumes that yet.
- The stylesheet call sites listed above are unmigrated.
- `CE_MenuItem` is Fusion's, so a menu's checkmarks and submenu arrows are
  Fusion's shapes rather than the chevron used everywhere else.
