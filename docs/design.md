# Sprite: design ground rules

The rules Sprite's own screens follow, and that apps made for it should follow too. They were
settled on the OpenFrame 1's panel (800×480, single touch, Atom Z520 with no GPU) in November
2026. The numbers are the ones in the code (`src/shell.c`, `src/sprite.h`). Change them there
and here together.

## 1. Sprightly first

Sprite should surprise people with how quick it feels on 2008 hardware. **Anything that
moves runs at 60 frames a second, or it doesn't ship.** Where a look and smoothness
conflict, smoothness wins: the flat look was chosen over a glossy one because the glossy one
couldn't redraw a page at 60 fps.

- **The frame budget is 16.7 ms.** A full-page draw should take well under that. Flat pages
  draw in 5–15 ms.
- **No blur, drop shadows or gradients** in anything that scrolls or animates. Each one costs
  far more than a solid shape when drawn in software.
- **Draw in step with the display.** The shell draws from the compositor's frame event, one
  frame per refresh, and only what has changed. Nothing redraws when nothing changes:
  Sprite uses about 0.2% CPU at idle.
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
- **network icons** (20 px), ending 8 px before the gear, in this order:
  - **a warning**, an amber triangle, while of-netplan has something to say about the
    settings files (a fixed address in ethernet.txt that isn't one, say). It used to be on
    the login prompt, which Sprite covers; it stays until the file's put right and the
    unit restarted, and the Network tab in Settings has an amber dot beside its name;
  - **Wi-Fi**, unless it's switched off (interface down, or radio blocked) or missing. It shows
    the signal as a dot and up to three arcs, all faint while it isn't connected. The
    bars go by the signal in dBm, by the same rule as the list of networks: three arcs from
    −55, two from −67, one from −75, below that the dot alone;
  - **ethernet** while a cable is connected, faint until it has an address.

  - **an update sign**, left of them all: a spinner while updates are checked for, downloaded
    or installed (in the background too, so it catches the eye), and a restart arrow once one
    needs a restart (until the unit's had it, at about 4am or when the owner chooses), with
    an exclamation mark inside when that restart will flash new firmware. It's drawn like the
    network icons (a ring with one arrowhead, roomy enough for the mark). It opens Settings
    at Updates, where "Restart now" is, and the firmware's explanation.

  Together the network icons are **one touch target**, the bar's height and as wide as the icons showing
  plus 8 px each side (so it meets the gear's box), lit white at 20% when pressed. It opens
  Settings at the Network tab, from any screen, whichever tab was open last;
- **the Settings gear**: a 48 px-wide box, the bar's height, hard against the top and right
  edges, with the 18 px radius on all four corners like any other box.

The bar is the same on every Sprite screen. The **page dots are one touch target**, as tall as
the bar: each tap turns to the next page, wrapping round from the last to the first. They're
too small to tap one by one, and dragging is the main way of turning pages anyway.
**Settings is reached from the bar**, never from a tile, so it can't be pushed onto a later
page: from the gear, or the network icons for its Network tab.

## 4. Home grid

- **6 columns by 3 rows,** 18 apps a page, as OpenPeak's own home screen had (O2's had 5×3),
  on up to three pages. No folders: three pages are room enough, and pages make the groups.
- **Apps stay where they're put.** A long press lifts a tile (it fades, and a copy follows
  the finger, the cell it would land in lit like a pressed one); dropping it swaps it with
  what's there, so nothing else moves, and gaps stay gaps. Held 0.6 s within 40 px of either
  side, it turns the page, again every 0.6 s. While it's up, an empty page waits after the
  last (up to three). Held still for 0.5 s more, or let go without moving, and the
  long-press sheet opens instead, the finger still down in the first case.
- **Cells are 128×140,** with 16 px side margins and 10 px above the first row. The whole
  cell takes the touch.
- **Icons are 88×88** with 18 px corners, 10 px from the top of the cell: a white glyph, 40 px,
  on the app's colour. Glyphs are Lucide's (line icons, ISC licence), the ones listed in
  `src/icons.txt`; an app names one with `icon=`. Without one, it gets up to two letters.
- **Labels** start 6 px below the icon: 16 px text, centred, up to two lines (36 px), ending in "..."
  beyond that.
- **A running app** has a 12 px dot on its icon's top-right corner: green with a window,
  amber while starting. **The startup app** (there's only ever one) has a 3 px ring in the
  accent colour round its icon, 2 px out from it. It sits inside the cell, 5 px from its top,
  so the press highlight takes it in.
- **A press** lightens the whole cell (white at 20%, with the 18 px radius).

## 5. Pages and motion

- **Further apps go on further pages.** The pages sit side by side and are turned by
  dragging sideways. The dots in the bar turn them too.
- **The page follows the finger.** Past the first or last page it moves a third as far as
  the finger, so you can feel the end.
- **One page flicks too,** against both ends, and springs back: it shows there's paging to
  be had, without anything having to say so.
- **A touch becomes a page drag after 12 px sideways.** Until then it can still be a tap or a
  long press, and from then on it can't open an app.
- **A page turns after 60 px of drag, or a flick of 0.4 px/ms.** Otherwise it settles back.
- **The slide takes 180 ms, easing out.** Animations in general are short (at most about
  250 ms) and ease out. Nothing waits for an animation to finish.
- **Drags are smoothed:** the touchscreen reports 36 times a second, so each frame gets a
  position interpolated between the last two reports, one report (28 ms) behind.
- **Scrolling behaves the same as paging,** and Sprite does it, not LVGL, so there's one swipe
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

**One rule for corner radii, for every box** (icons, buttons, dialogs, banners, pressed
highlights): **the radius is the box's shorter side × 0.2045 (18/88, an app icon's
proportion), rounded, up to 18 px.** A fixed radius would make small boxes look round.

| Box | Shorter side | Radius |
|---|---|---|
| App icon | 88 px | 18 px |
| Gear, page-dot target | 40 px | 8 px |
| Banner | about 48 px | 10 px |
| Button | 48 px | 10 px |
| App cell highlight, dialog card | over 88 px | 18 px (the cap) |

Only circles and pills are rounder: dots, the home pill, switches and slider ends. In the code,
`rounded()` applies the rule now and whenever the box is resized. LVGL's buttons and message
boxes get it from Sprite's theme layer, so new ones can't drift from it.


| Role | Colour |
|---|---|
| Background | `#131920` |
| Status bar | `#050709` (a step darker than the background, to set it apart) |
| Text | `#e8edf2` |
| Dimmed text | `#8b97a3` |
| Accent (buttons, switches) | `#3d9be9` |
| Running dot | `#45d483` (amber `#f2b33d` while starting) |
| Pressed | white at 20% over whatever's beneath (cells, the gear, the page dots, list rows) |

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

## 7. Buttons and list screens

**Settings is tabs down the left**, not one long scrolling page: a rail 208 px wide, a step
darker than the background (`#0d1217`), with Home at its top and then a tab per group,
each 52 px tall with a 24 px glyph (Lucide, like the tiles) and its name. The chosen tab
is a lighter box (white at 10%) with its glyph in the accent colour; the panel beside it
holds that group's settings, and scrolls when they outgrow the screen (that's fine: it was
one long page of everything that wasn't). The groups: **Network** (Wi-Fi, Ethernet, remote
access), **Display** (brightness, touch), **Time** (the clock; the time zone to come),
**Updates** (what the automatic updates are doing, what's waiting, and asking for them now),
**About** and **Power**.
Sound will be a tab of its own. A status line in a panel is one line, ending in "...".
**While something slow runs, there are no buttons to press**: its status line says what's
happening ("Installing updates...", with "2 of 3" on the right), with a progress bar under
it when there's something to measure; the spinner is in the status bar. The buttons come
back when it's done. **What's new** opens from an update's row (the chevron): its notes for
owners, newest first, under each part's name and version, or "Bug fixes and improvements".
More than fits goes on a page of its own, opened by tapping a row that ends in a chevron
(the Wi-Fi row opens Wi-Fi's details, the Ethernet row Ethernet's). The chevron sits in a
32 px accent box, so it reads as something to touch; the whole row takes the touch, and
lights as a list row does, its highlight reaching 12 px past the text. Problems to put right are amber, at
the top of their tab.

**Where one address is shown alone** (to log in to, say), it's the wired one when there
are both: it's faster and steadier, so that's where people are pointed.

**Back buttons are always in one place and one size**, top left: 16 px from the left edge
and 20 px below the bar, 176 px wide (the width of Settings' tabs) and 48 px tall (Home in
Settings' rail, Settings on the Wi-Fi screen, and the rest), so moving between screens
they don't jump about. Their arrow and accent colour set them apart.

**Every button has one shape** (`button()`): **48 px tall** (the smallest touch target), as
wide as its label plus 24 px each side, and **never narrower than 160 px**. Text is 20 px.
Buttons in a row are 20 px apart.

- **Coloured by role:** the main action in the accent blue, others grey `#3a4a5a`,
  destructive ones red `#b3403a`.
- **Pressed, a button darkens.** It doesn't grow (LVGL's `LV_THEME_DEFAULT_GROW` is off): a
  grown button's corners poke out of its row and get cut off square.
- **Labels say what happens** in words anyone knows: "Reset", not "Standard".
- **An icon-only button** (showing a password) is a 48×48 square, grey.

**List screens** (Settings, and any like it) are a column of headings and rows, 12 px apart:
- **Every row of controls is 48 px tall.** A button fills it; a switch or slider sits in its
  middle.
- **Headings** (14 px, dimmed, capitals) get **16 px more space above them**, so a section
  starts 28 px below whatever came before it, whatever that was.
- **A hint under a setting's name** (14 px, dimmed) sits 6 px below it, and says what the
  setting does in plain words.
- **A list of choices** (the Wi-Fi networks) has rows 48 px tall but only 4 px apart, each
  pressable as a whole (white at 20% while pressed, with the radius rule), its content inset
  12 px: a 20 px icon, the name (20 px, ending in "..." if it's too long), then a note (16 px:
  "Connected" in the accent colour, others dimmed) and a lock for one that needs a password.
  Ones that can't be chosen are dimmed, but still say why when tapped.
- **Something that takes a moment** (looking for networks) says so in a dimmed line where
  its results will go, and the button that would start it again is disabled meanwhile.

## 8. Typing

**A screen for typing** has one field, and Sprite's own keyboard filling the bottom 276 px:

- **The top row:** a back button, and what's wanted beside it ("Password for ...").
- **The field** below it, 48 px tall, with a 2 px accent border and the cursor showing; then
  the eye button (passwords only) and the button that goes on ("Connect", "Next"), disabled
  until what's typed will do. A dimmed hint under the field says what will ("8 to 63
  characters").
- **Passwords are hidden** as they're typed, each character showing for a moment; the eye
  shows them all. Leaving the screen empties the field.
- **The keyboard:** five rows of keys 48 px tall and about 73 px wide, 6 px apart, on the
  status bar's colour. The top row is always the digits. One layer has the letters (with
  `, . -` and a space bar), the other every other printable ASCII character. Shift is for
  one letter; tapped twice, for all until it's tapped again; it's accent-coloured while on.
- **Keys act as they're pressed,** and only backspace repeats when held. A pressed key
  darkens, drawn as it's drawn rather than through LVGL's pressed style, which would redraw
  the whole keyboard (19 ms) on every press: only the key itself redraws (about 8 ms).

## 9. Dialogs

**Anything that asks something uses the one dialog** (`dialog_open()` and `dialog_button()`):
the long-press sheet, the Restart and Switch off questions, and the calibration result.

- **A card** (`#1c242c`) centred on the screen dimmed to 40%, with 24 px padding and 18 px
  between its parts: a **title** (28 px), **text** (20 px, dimmed, wrapping inside the card;
  never one long line), anything particular to it (the sheet's startup switch), then **a row
  of buttons**.
- **Buttons are the ordinary ones,** left-aligned, the main action first (Open, Save,
  Restart). The card is 460 px wide, or as wide as its buttons need.
- **A tap outside the card closes a dialog that only asks** (the sheet, the power questions),
  as Cancel would. One that needs an answer (the calibration result) waits for a button.
- Text says what happened or will happen in a sentence or two. Titles are short: the app's
  name, or the question.

## 10. Touch

- **Targets are at least 48 px in their smaller dimension.** Calibrated taps land within
  about 10 px of where you aim, and on the bare factory calibration up to 16 px out. Never
  split one control into halves smaller than about 30 px; HamClock's keyboard does, and it
  needs calibrating to be usable.
- **The bottom 35 px** (y ≥ 445, the middle 160 px) **belongs to the home pill** while an app is
  showing. A touch that starts there goes home. Apps shouldn't put controls in that strip.
- **A release is held for 64 ms,** to merge the touchscreen's contact bounces during fast
  strokes. Motion carries on through that wait, so nothing visibly stops.
- **No hover:** the touchscreen only reports while it's touched.

## 11. Apps

- **Full screen at 800×480,** drawn in software (no GL). Wayland apps connect directly; X11
  and SDL apps go through Xwayland. Or **below the bar** (`bar=yes`, 800×440): small apps
  that are part of the OpenFrame's everyday use (a calculator) keep the bar, with their name
  as its title, so they feel part of Sprite; whole-screen ones (a clock, HamClock) take it all.
- **A manifest** gives the name (keep it short; two lines of 16 px fit about 28 characters),
  a `color=` for the icon, and an icon glyph keyword.
- **`handle=faint` or `handle=hidden`** for apps whose content needs the bottom edge clear.
  The pill's touch zone still works.
- **In a sandbox,** with only what `allow=` asks for (see the README). Starting one costs
  about 0.8 s, most of it systemd's: worth watching, as the first thing seen after a tap.

## Still to settle

- **Icon artwork** for apps that bring their own (an 88×88 PNG, its corners rounded as it's
  loaded?), alongside the glyph tiles.
- **Light or dark,** and whether the look follows the time of day or the light sensor.
- **Fonts beyond ASCII,** for network and app names with accents or emoji: each size built
  in with more characters costs memory, so measure first.
