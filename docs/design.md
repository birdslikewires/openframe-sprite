# sprite: design ground rules

The rules sprite's own screens follow, and that apps made for it should follow too. They were
settled on the OpenFrame 1's panel (800×480, single touch, Atom Z520 with no GPU) in November
2026. The numbers are the ones in the code (`src/shell.c`, `src/sprite.h`). Change them there
and here together.

## 1. Sprightly first

sprite should surprise people with how quick it feels on 2008 hardware. **Anything that
moves runs at 60 frames a second, or it doesn't ship.** Where a look and smoothness
conflict, smoothness wins: the flat look was chosen over a glossy one because the glossy one
couldn't redraw a page at 60 fps.

- **The frame budget is 16.7 ms.** A full-page draw should take well under that. Flat pages
  draw in 5–15 ms.
- **No blur, drop shadows or gradients** in anything that scrolls or animates. Each one costs
  far more than a solid shape when drawn in software.
- **Draw in step with the display.** The shell draws from the compositor's frame event, one
  frame per refresh, and only what has changed. Nothing redraws when nothing changes:
  sprite uses about 0.2% CPU at idle.
- **Measure every change that moves.** Run `SPRITE_TRACE_PERF=1` (or `2` for each late frame),
  and drive it with `scripts/touchsim.py`, which drags like the real touchscreen. Report
  frames per second and late frames, before and after.

## 2. The screen

| Area | Size |
|---|---|
| Status bar | the top 40 px, full width |
| Content | 800×440 below it |
| Apps | the whole 800×480, full screen, one at a time |

**Every screen starts its content at the same point:** 36 px from the left, 20 px below the
bar, at (36, 60). The home grid's first icon and Settings' first control both start there. The
right margin matches, 36 px, and screens that scroll keep 20 px below their content too.

## 3. Status bar

Everything in the bar is centred on its midline. Its ends are symmetrical: the time starts as
far from the left edge as the gear's glyph ends from the right (about 14 px, worked out from the
glyph). From left to right:
- **the time** (20 px), then **the date** (14 px, dimmed), 10 px apart. The time is 24-hour by
  default, or 12-hour, and without seconds by default (`clock=`, `seconds=` in the settings
  file, and two switches in Settings). Its label is as wide as the widest time its format can
  show, so nothing after it moves as the time changes. It redraws only when what it shows
  changes, just after each second begins;
- **the page dots** in the centre, only when there's more than one page. In Settings, the
  title goes there instead;
- **network icons** (20 px), ending 8 px before the gear:
  - **ethernet** while a cable is connected, faint until it has an address;
  - **Wi-Fi**, unless it's switched off (interface down, or radio blocked) or missing. It shows
    the signal as a dot and up to three arcs, all faint while it isn't connected;
- **the Settings gear**: a 48 px-wide box, the bar's height, hard against the top and right
  edges, with the 18 px radius on all four corners like any other box.

The bar is the same on every sprite screen. The **page dots are one touch target**, as tall as
the bar: each tap turns to the next page, wrapping round from the last to the first. They're
too small to tap one by one, and dragging is the main way of turning pages anyway.
**Settings is reached only from the gear**, never from a tile, so it can't be pushed onto a
later page.

## 4. Home grid

- **6 columns by 3 rows,** 18 apps a page, as OpenPeak's own home screen had (O2's had 5×3).
- **Cells are 128×140,** with 16 px side margins and 14 px above the first row. The whole
  cell takes the touch.
- **Icons are 88×88** with 18 px corners, 6 px from the top of the cell.
- **Labels** start 6 px below the icon: 16 px text, centred, up to two lines, ending in "..."
  beyond that.
- **A running app** has a 12 px dot on its icon's top-right corner: green with a window,
  amber while starting. **The startup app** has a small power mark at the bottom-left.
- **A press** lightens the whole cell (white at 20%, with the 18 px radius).

## 5. Pages and motion

- **Further apps go on further pages.** The pages sit side by side and are turned by
  dragging sideways. The dots in the bar turn them too.
- **The page follows the finger.** Past the first or last page it moves a third as far as
  the finger, so you can feel the end.
- **A touch becomes a page drag after 12 px sideways.** Until then it can still be a tap or a
  long press, and from then on it can't open an app.
- **A page turns after 60 px of drag, or a flick of 0.4 px/ms.** Otherwise it settles back.
- **The slide takes 180 ms, easing out.** Animations in general are short (at most about
  250 ms) and ease out. Nothing waits for an animation to finish.
- **Drags are smoothed:** the touchscreen reports 36 times a second, so each frame gets a
  position interpolated between the last two reports, one report (28 ms) behind.
- **Scrolling behaves the same as paging,** and sprite does it, not LVGL, so there's one swipe
  behaviour everywhere:
  - an up/down drag takes over after 12 px (LVGL's own scrolling waits for 24, so it never
    starts first);
  - the content follows the finger, a third as far past either end;
  - a flick glides on as far as 300 ms at its speed would take it, easing out over 600 ms,
    and stops at the ends;
  - past an end, it springs back in 180 ms;
  - a touch stops a glide.

  No scrollbars.

## 6. The look: flat

**One corner radius, 18 px, for every box:** icons, buttons, cards, banners, the message box and
pressed highlights. Only circles and pills are rounder: dots, the home pill, switches and slider
ends. LVGL's buttons and message boxes get the radius from sprite's theme layer, so new ones
can't drift from it.


| Role | Colour |
|---|---|
| Background | `#131920` |
| Status bar | `#050709` (a step darker than the background, to set it apart) |
| Text | `#e8edf2` |
| Dimmed text | `#8b97a3` |
| Accent (buttons, switches) | `#3d9be9` |
| Running dot | `#45d483` (amber `#f2b33d` while starting) |

- **Icons** are a solid rounded square in the app's own colour (`color=` in its manifest)
  with a white glyph. Bright, saturated colours that hold white well are best.
- **Type** is Montserrat, built into the shell, at a fixed set of sizes:

| Size | Used for |
|---|---|
| 14 px | small print, status |
| 16 px | labels |
| 20 px | body text and buttons |
| 28 px | titles |
| 40 px | icon glyphs |

## 7. Touch

- **Targets are at least 48 px in their smaller dimension.** Calibrated taps land within
  about 10 px of where you aim, and on the bare factory calibration up to 16 px out. Never
  split one control into halves smaller than about 30 px; HamClock's keyboard does, and it
  needs calibrating to be usable.
- **The bottom 35 px** (y ≥ 445, the middle 160 px) **belongs to the home pill** while an app is
  showing. A touch that starts there goes home. Apps shouldn't put controls in that strip.
- **A release is held for 64 ms,** to merge the touchscreen's contact bounces during fast
  strokes. Motion carries on through that wait, so nothing visibly stops.
- **No hover:** the touchscreen only reports while it's touched.

## 8. Apps

- **Full screen at 800×480,** drawn in software (no GL). Wayland apps connect directly; X11
  and SDL apps go through Xwayland.
- **A manifest** gives the name (keep it short; two lines of 16 px fit about 28 characters),
  a `color=` for the icon, and an icon glyph keyword.
- **`handle=faint` or `handle=hidden`** for apps whose content needs the bottom edge clear.
  The pill's touch zone still works.

## Still to settle

- **Icon artwork** for apps that bring their own (an 88×88 image in the manifest?), alongside
  the glyph tiles.
- **Light or dark,** and whether the look follows the time of day or the light sensor.
- **Settings' layout** as it grows: Wi-Fi, sound, time zone.
