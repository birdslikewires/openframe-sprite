# Sprite

A touch shell for OpenFrames running Debian Trixie, to replace OpenPeak's old UI. This is the first prototype.

Sprite is one program that is both the display server (a Wayland compositor built on wlroots) and the shell: status bar, home screen and settings, drawn with LVGL. It runs on an OpenFrame 1's Atom Z520 with no GPU: everything is drawn in software, and frames go to the panel through DRM page flips, so nothing tears. It shows one app at a time, full screen at 800x480. Native Wayland apps connect directly, and X11 and SDL apps go through Xwayland, which starts when the first one needs it.

Measured on the test OF1: 10.5 MB of RAM for Sprite, and 0.2% of the CPU when nothing on screen is changing. The display stack it needs (wlroots, Xwayland, SDL2) is about 10 MB on disk. See `spike/README.md` for the measurements behind the design.

The ground rules for how it looks, feels and performs are in [docs/design.md](docs/design.md).

## Using it

- **Home screen:** tap a tile to open its app. A green dot means the app is running in the background, and tapping it brings it back as it was. An amber dot means the app is still starting.
- **Going home:** while an app shows, a small pill sits at the bottom centre of the screen. Touch it, or swipe up from it, to go home. With a USB keyboard, Alt+Esc also goes home, and Ctrl+Alt+Backspace quits sprite.
- **Long press on a tile** lifts it. Keep holding it still for half a second more, or let go without moving, and a sheet opens with Open, Close (when the app is running) and **Open at startup**. Close asks the app's window to close, then stops its sandbox 2 s later if it's still running.
- **Arranging:** a lifted tile follows the finger, and dropping it on a cell puts it there, swapping it with any app already in it. Held at either side, it turns the page; while a tile is up there's an empty page after the last, so apps can go on up to three pages, with gaps where you leave them. The layout is kept in `/var/lib/sprite/layout`; new apps take the first free cells.
- **The startup app** (one at most, with a blue ring round its icon) opens when Sprite starts, and reopens 2 s after it closes or crashes, unless it was closed from the sheet. After three exits within a minute it stops reopening and says so. It's `startup=` in the settings file (see Settings below).
- **The Shop,** a tile of its own, is where apps and screensavers come from, all of them free, from apps.openbeak.net ([openframe-apps](https://github.com/birdslikewires/openframe-apps)). Apps lists what it offers, with screensavers under a heading of their own, and Installed what's been installed, each with Get, Open, Update or (a screensaver) Choose, and a page of its own: what it is, what it can use (the network, sound, the microphone), a QR code to where its community answers questions, as OpenBeak doesn't support the apps itself, and its version, size, licence and source, with Remove once it's installed. The helper installs and removes them (its `shop-*` requests), with progress as it goes, and a tile appears or goes as soon as it's done. The plan, and what's built of it, is [docs/shop.md](docs/shop.md).
- **The power pull-down:** a drag down from anywhere on the status bar (not just its edge, which the resistive screen reads poorly) slides a panel over the screen with Screen off (Sprite's display sleep, now, until a touch), Restart and Switch off, which ask first as they always have. A tap outside it, or choosing one, puts it away. It isn't there over an app.
- **Settings,** in tabs down the left, in alphabetical order, then About under a line: Services (each service installed, with its switch, what it's doing and its memory, and while it runs, the address of its own web page with a QR code to it), Audio (where the sound goes, the OpenFrame's own or a USB DAC, through openframe-scripts' `of-audio` and `settings/audio.txt`: a DAC plugged in for the first time becomes the output at a low volume, and has a Direct to DAC switch that takes the OpenFrame's volume control out of its way, the audio reaching it as it is, with what it's connected at and playing at shown; the volume, set in-process through ALSA's mixer, as it sounds), Network (Wi-Fi: choosing a network, typing its password on Sprite's own keyboard, and forgetting it; Ethernet: whether a cable's in, its speed and address; tapping either opens its details, read-only: its address and whether the router or a settings file set it, the gateway, MAC address, the DNS servers and the unit's name; of-netplan's complaints about the settings files show in amber at the top of the tab; and remote access: the password for logging in as `of` over SSH, which images leave unset, with a warning while a unit still has the old shared one, and a Turn off that takes every way in away again: the password, the keys, ssh.txt's keys, and anyone logged in), Display (brightness, through `of-backlight`, including automatic brightness, whose slider then sets the lowest and highest it goes to, with a switch for the screen going off in the dark or staying at its lowest: `backlight.txt`'s `min`, `max` and `off_below`; touch calibration; and the clock's format), Updates (openframe-scripts' automatic updates: what they're doing, with progress, what's waiting, OpenBeak's first, each with its "What's new", checking, updating or restarting now, and the time of day they install), About (its name, as written and as the hostname made from it, in openframe-scripts' `settings/name.txt`; the system, what the hourly ping sends, and Erase everything, which puts the unit back as it came for its next owner: openframe-scripts' `of-reset`) and Power (Restart and Switch off).

## Apps

An app is a small manifest file named `<id>.app`, in `/opt/sprite/share/sprite/apps/` (Sprite's own) or `/usr/share/sprite/apps/` (installed by packages, such as [openframe-hamclock](https://github.com/birdslikewires/openframe-hamclock)):

```
name=HamClock
exec=/usr/bin/hamclock -t 10 -b ohb.hamclock.app:80
icon=radio
color=b8860b
display=x11
allow=network
```

- `icon=`: a glyph, by its name in [Lucide](https://lucide.dev/icons) (those listed in `src/icons.txt`, which `scripts/make-icons.sh` makes into `src/icons_40.c`), or up to two letters
- `color=`: the tile's colour
- `display=x11`: for X11 and SDL apps; leave it out for native Wayland apps
- `handle=`: the home pill while the app shows, `normal` (the default), `faint` or `hidden`
- `bar=yes`: the status bar stays above the app, with its name in it, and the app's window is the rest of the screen (800×440), for small apps that feel part of Sprite, like a calculator. Without it, an app has the whole screen. The bar's buttons work as usual, taking Sprite to the front; the pill does too. Only for apps that fit the window they're given: one that always draws 800×480 loses its bottom 40 pixels
- `kind=screensaver`: a screensaver rather than an app. It has no tile; it's chosen in Settings, Display (where it then appears, alongside the built-in flip clock), and runs over everything once the OpenFrame's been left untouched for "Screensaver after", until a touch, which goes no further. The flip clock covers the screen while it starts, and if it quits. It's stopped when the screen goes off, and started again when a touch lights it (the screen wakes at the screensaver; another touch ends it)
- `kind=service`: a service, which runs in the background whatever's on the screen, such as a music server. It has no tile. sprite-helper starts it at boot (`sprite-services.service`) in the same sandbox as an app, as `sprite-service-<id>.service`, which carries on while Sprite restarts, and again 10 s after it fails; Settings, Services switches it on and off, and says what it's doing and the memory it's using
- `web=`: a service's own web page, on this port: Services shows its address and a QR code to it, while it runs, as it's set up from a phone or a computer
- `memory=`: the memory a service needs, in MB, from 64 to 320 (otherwise 192, as an app has), past which it's ended rather than slowing the OpenFrame down
- `allow=`: what the app may have beyond the display, separated by spaces: `network`, `sound` (to play), `microphone` (to record as well) and `music` (to read `/media`, where openframe-scripts mounts USB drives and network shares, read-only); without it, an app has none of them, and can't see `/media` at all

Comments go on lines of their own, starting with `#`. The package brings only Settings: every other app comes in its own package, as HamClock does. Sprite tells apps to draw through X11 for SDL (`SDL_VIDEODRIVER=x11`), because SDL's Wayland support needs EGL, which this hardware lacks.

### Sandboxes

Each app runs in a sandbox: `sprite-helper` starts it as a systemd unit, `sprite-app-<id>.service`, under a user of its own (`DynamicUser=`) in the `sprite-apps` group. That group can reach Sprite's display sockets (`/run/sprite-apps/wayland-0`, and Xwayland's, which Sprite's X keeper opens to the group) and nothing else of Sprite's. What it runs comes from its manifest, never from sprite. It has:

- no sudo, no capabilities, and no way to gain privileges (`NoNewPrivileges=`), with a seccomp filter (`@system-service`)
- a read-only system, no `/home`, no `/boot` (where `wifi.txt` is, readable by anyone, as FAT has no permissions), its own `/tmp`, and no other processes to see
- one directory of its own, `/var/lib/sprite-apps/<id>` (its `$HOME`, kept between runs)
- no network unless `allow=network`, and no sound devices unless `allow=sound` (playing only: the capture devices there as it starts are hidden) or `allow=microphone` (those too)
- a lower share of the CPU than Sprite, and limits of 192 MB of memory, 64 MB of swap (zram, which is RAM too) and 64 processes; past the memory limit the kernel ends the app, not something else. The limits need a kernel with `MEMCG` and `CGROUP_PIDS`

What the sandbox doesn't do, by choice: X11 apps share one Xwayland, so they can see each other's windows and input (Wayland apps can't); and the `sprite` user, which Sprite runs as, can do root's work through `sprite-helper` (set the SSH password, say). The helper is the boundary: apps never reach it, only Sprite does. Isolating apps further isn't planned.

It's bound to `sprite.service`, so it stops when Sprite does. Sprite knows an app's windows by its unit's cgroup, and its exit by a pidfd. Starting one takes about 0.8 s on the OF1, 0.4 s of it systemd setting up the sandbox. `SPRITE_SANDBOX=0` in Sprite's environment starts apps the old way instead, as Sprite's own user, for trying things out.

## Building

Builds run in an i386 Debian Trixie container, so they need Docker; on the Mac that's colima. The repository has to be under `$HOME`, because colima only shares the home directory.

```
scripts/build.sh            # out/sprite
scripts/build.sh package    # out/openframe-sprite_<version>_i386.deb
```

The first build creates the `sprite-build` image, which compiles the Mesa-free display stack (`build/stack.sh`). That takes a few minutes, and is cached after that. LVGL 9.3.0 is downloaded into `third_party/` and its checksum verified (`scripts/fetch-lvgl.sh`).

CI (`.github/workflows/package.yml`) builds the package on every change to what goes into it, installs it on a clean Trixie with the OpenFrame repository to test it, and publishes a release. The version is year.month.N (`package/version.sh`).

## On-screen strings

Everything Sprite says on screen is in [src/strings/en_gb.txt](src/strings/en_gb.txt), one message a line (`key = text`), each with a note saying where it appears, so wording can be changed without touching the C. The build turns it into `build/gen/sprite_strings.h` (`scripts/make-strings.awk`), a `#define STR_<KEY> "text"` for each, so the code still gets string literals and the compiler checks every `printf`-style use against the message's placeholders (`-Werror=format`): a `%s` changed to `%d`, or one left out, stops the build. `scripts/check-strings.sh` checks that every message is used. sprite-helper's reasons for refusing something (`helper.*`) are in the file too: the helper sends the key, and Sprite shows its text.

Another language, or another English, is a file of its own beside it (`en_us.txt`, say) with only the messages it changes, built with `make STRINGS_LANG=en_us`. Its placeholders must match en_gb's. Dates and the clock come from the system, not the file.

## What's new: notes for owners

A commit that owners should hear about says so in a `News:` trailer, a plain sentence about what they'll notice, one per line, beside the `Co-Authored-By:` lines:

```
News: Tap the Wi-Fi or Ethernet line in Settings to see the network's details.
```

`package/version.sh --news` turns them into the notes for every version (the version each commit made, worked out as the version itself is), and the release carries them as `openframe-sprite.news`. openframe-linux publishes them beside the package (`news/openframe-sprite.news`), where `of-update` fetches them when it looks for updates, so Settings can show what's new before anything's installed. Most commits need none: a version without notes is "Bug fixes and improvements".

## The package

`openframe-sprite` carries Sprite and the display stack under `/opt/sprite`: wlroots, Xwayland, SDL2 and sdl12-compat (for SDL apps), the root helper, the Settings app and a template of the settings file. Its systemd units go in `/usr/lib/systemd/system`. None of it needs Mesa: Debian's own wlroots, Xwayland and SDL2 do, at about 250 MB. It depends on `openframe-scripts` (for `bootrw` and `of-backlight`).

The first time it's installed, it takes tty1 from the login prompt (marked in `/var/lib/openframe-sprite`, so disabling Sprite by hand afterwards sticks), and puts `settings/interface.txt` on OF-BOOT if there isn't one (moving one from its old name, `sprite.txt`). Removing it brings the login prompt back.

**It tells openbeak.net that the unit is running** (`sprite-ping`): once after it starts, when its clock is set, and then every hour after that (`sprite-ping.timer`), so each unit has a minute of its own and the server knows when to expect it next. It sends the Wi-Fi card's and Ethernet's MAC addresses (the ones the stock software reported, so a unit can be matched to what it was before), the model, the firmware, the versions of Sprite, openframe-scripts, the firmware package, the kernel (and the running kernel's build) and Debian, when an update's kernel last failed to start and the previous one was put back (openframe-scripts' `of-kernel-check`), whether it starts from internal memory or USB, its memory, free and total storage, and swap in use, and how long it's been up. Nothing else. The server keeps it without the address it came from. About in Settings says so too. It's part of Sprite: removing the package removes it, and `sudo systemctl disable --now sprite-ping.timer` stops it (enabled once per system, so that sticks). It runs as a throwaway user that can only read the system and reach the network, and writes nothing to storage.

## Running on an OpenFrame

```
scripts/build.sh package && OFH=of@<address> scripts/deploy.sh   # install the package
OFH=of@<address> scripts/deploy.sh noboot                        # back to the tty1 login
OFH=of@<address> scripts/deploy.sh boot                          # sprite at boot again
```

Sprite reads manifests from `/opt/sprite/share/sprite/apps`, then `/usr/share/sprite/apps` (packages), then `/usr/local/share/sprite/apps`, which is the place to try out an app by hand.

Sprite runs as `sprite.service` (in `system/`), on tty1 in place of the login prompt, as its own user, `sprite`, in a logind session, so libseat gets the display and input through logind. `sprite` has no password, login or sudo, and keeps its state in `/var/lib/sprite`. `of` is the person's account (with sudo), and nothing runs as it unless someone's logged in: so an app that got the better of Sprite or Xwayland, which take everything apps send to the display, would have `sprite`, not root. `of` is in the `sprite-apps` group, so a Wayland client started by hand can use `WAYLAND_DISPLAY=/run/sprite-apps/wayland-0`. It starts after `of-flash`, `of-mac-gate` and Plymouth, which all use tty1 first. If it keeps failing, the login prompt comes back (`OnFailure=getty@tty1.service`). The log is in the journal: `sudo journalctl _COMM=sprite` (under the PAM session, Sprite's own lines aren't filed under the unit). `SPRITE_TRACE_INPUT=1` logs every press given to an app.

A few things need root, and go through `sprite-helper` (`system/`), which systemd starts for each connection to `/run/sprite-helper.sock` (group `sprite`). It takes one request and validates it: `set <key> <value>` for `startup`, `calibration`, `clock` and `seconds` in the settings file, `reboot` and `poweroff`, for Wi-Fi `wifi-scan`, `wifi-status`, `wifi-join` and `wifi-forget` (see Wi-Fi below), `app-start` and `app-stop` for apps (see Sandboxes above), and `ssh-status`, `ssh-password` and `ssh-off` for the login password, `name-set` for the name (never giving out the password or its hash). The header of `system/sprite-helper` describes each.

It also expects the touchscreen calibration in `spike/touch/` to be installed.

### Settings

`settings/interface.txt` on the OF-BOOT volume (`sprite.txt` until 26.11.122), alongside `wifi.txt` and the rest, so it can be edited from a PC. Owners never see the name Sprite: to them it's the OpenFrame's interface.

```
startup=Clock      # the app to open at startup, by the name on its tile (or its id)
calibration=1.013455,-0.001630,-5.9928,-0.002758,1.047415,-8.2094
```

`calibration=` comes from Settings > Calibrate touch: nine crosses, tapped in turn, and an affine fit of where the taps landed (x' = a x + b y + c, y' = d x + e y + f). Sprite applies it to all touch input, for apps as well as the shell, on top of the kernel's calibration (`spike/touch/`). An empty value means none. HamClock's keyboard needed it: each key types a different character in its top and bottom halves, 22 px each.

Setting "Open at startup" on the touchscreen writes the same file through the helper, which makes `/boot` writable with `bootrw` just while it replaces the file with a synced copy. `deploy.sh` puts a copy there if there isn't one. When a startup app is set, Sprite keeps the screen black until the app's window appears, rather than showing the home screen first.

### Wi-Fi

Wi-Fi is set up the OpenFrame way: `name=` and `password=` in `settings/wifi.txt` on OF-BOOT, which `of-netplan` (openframe-linux) turns into a netplan configuration. Sprite never handles the file itself. Choosing a network in Settings sends its name and password to the helper, which checks them (a name of 1-32 bytes, a password of 8-63 characters or 64 hex digits or none, no control characters), writes them into `wifi.txt` in double quotes (so spaces, quotes and backslashes are kept exactly), keeps any fixed address in the file, and starts `of-netplan.service` to apply them. Settings then follows the connection, and says if it doesn't work out (wpa_supplicant giving up on the network, most often for a wrong password, or nothing after 45 s). Forget empties both.

Looking for networks uses `wpa_cli scan_results`: from netplan's wpa_supplicant when a network is set up, or else from one the helper starts just for the scan (about 3 s), which leaves the card down again afterwards. Networks needing WEP or an enterprise login are listed but can't be joined from Settings (`wifi.txt` can't describe them; a `network.yaml` can). Sprite's fonts are ASCII only, so a network name with other characters shows gaps, though it can still be joined.

## Not done yet

- Starting faster: 2.4 s pass between systemd starting Sprite and Sprite running, cold
- Letting apps change the home pill while they run (a small Wayland protocol), and screen blanking
- Sound and time zone in Settings
- The Shop's donations page, and options an app can have set from its tile's sheet (see [docs/shop.md](docs/shop.md))
- A CPU cap on apps in the background (`CPUQuota=`, which the kernel can do now)

## Licence

Sprite is under the GNU General Public License, version 3 or (at your option) any later version (see [LICENSE](LICENSE)). Copyright 2026 Andrew Davison.

What it's built on keeps its own terms: LVGL (MIT), downloaded into `third_party/` at build time; the tile glyphs, from Lucide (ISC, in `src/icons_40.c`); and wlroots and Xwayland (MIT/X11) and SDL2 and sdl12-compat (zlib), built from Debian's sources by `build/stack.sh`. The package carries all of their licences. `src/lv_conf.h` is generated from LVGL's configuration template.
