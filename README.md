# sprite

A touch shell for OpenFrames running Debian Trixie, to replace OpenPeak's old UI. This is the
first prototype.

sprite is one program that is both the display server (a Wayland compositor built on wlroots)
and the shell: status bar, home screen and settings, drawn with LVGL. It runs on an OpenFrame
1's Atom Z520 with no GPU: everything is drawn in software, and frames go to the panel through
DRM page flips, so nothing tears. It shows one app at a time, full screen at 800x480. Native
Wayland apps connect directly, and X11 and SDL apps go through Xwayland, which starts when the
first one needs it.

Measured on the test OF1: 10.5 MB of RAM for sprite, and 0.2% of the CPU when nothing on screen
is changing. The display stack it needs (wlroots, Xwayland, SDL2) is about 10 MB on disk. See
`spike/README.md` for the measurements behind the design.

The ground rules for how it looks, feels and performs are in [docs/design.md](docs/design.md).

## Using it

- **Home screen:** tap a tile to open its app. A green dot means the app is running in the
  background, and tapping it brings it back as it was. An amber dot means the app is still
  starting.
- **Going home:** while an app shows, a small pill sits at the bottom centre of the screen.
  Touch it, or swipe up from it, to go home. With a USB keyboard, Alt+Esc also goes home, and
  Ctrl+Alt+Backspace quits sprite.
- **Long press on a tile** lifts it. Let go without moving and a sheet opens with Open, Close
  (when the app is running) and **Open at startup**. Close asks the app's window to close,
  then stops its sandbox 2 s later if it's still running.
- **Arranging:** a lifted tile follows the finger, and dropping it on a cell puts it there,
  swapping it with any app already in it. Held at either side, it turns the page; while a
  tile is up there's an empty page after the last, so apps can go on up to three pages,
  with gaps where you leave them. The layout is kept in `/var/lib/sprite/layout`; new apps
  take the first free cells.
- **The startup app** (one at most, with a blue ring round its icon) opens when sprite starts,
  and reopens 2 s after it closes or crashes, unless it was closed from the sheet. After three
  exits within a minute it stops reopening and says so. It's `startup=` in the settings
  file (see Settings below).
- **Settings:** Wi-Fi (choosing a network, typing its password on sprite's own keyboard,
  and forgetting it), remote access (the password for logging in as `of` over SSH, which
  images leave unset; it warns while a unit still has the old shared one), brightness (through `of-backlight`, including automatic brightness),
  the clock's format, touch calibration, information about the system, and Restart and
  Switch off.

## Apps

An app is a small manifest file named `<id>.app`, in `/opt/sprite/share/sprite/apps/` (sprite's
own) or `/usr/share/sprite/apps/` (installed by packages, such as
[openframe-hamclock](https://github.com/birdslikewires/openframe-hamclock)):

```
name=HamClock
exec=/usr/bin/hamclock -t 10 -b ohb.hamclock.app:80
icon=radio
color=b8860b
display=x11
allow=network
```

- `icon=`: a glyph, by its name in [Lucide](https://lucide.dev/icons) (those listed in
  `src/icons.txt`, which `scripts/make-icons.sh` makes into `src/icons_40.c`), or up to two
  letters
- `color=`: the tile's colour
- `display=x11`: for X11 and SDL apps; leave it out for native Wayland apps
- `handle=`: the home pill while the app shows, `normal` (the default), `faint` or `hidden`
- `allow=`: what the app may have beyond the display, `network` and `sound`, separated by
  spaces; without it, an app has neither

Comments go on lines of their own, starting with `#`. The package brings only Settings:
every other app comes in its own package, as HamClock does. sprite tells apps to draw
through X11 for SDL (`SDL_VIDEODRIVER=x11`), because SDL's Wayland support needs EGL, which
this hardware lacks.

### Sandboxes

Each app runs in a sandbox: `sprite-helper` starts it as a systemd unit,
`sprite-app-<id>.service`, under a user of its own (`DynamicUser=`) in the `sprite-apps`
group. That group can reach sprite's display sockets (`/run/sprite-apps/wayland-0`, and
Xwayland's, which sprite's X keeper opens to the group) and nothing else of sprite's. What
it runs comes from its manifest, never from sprite. It has:

- no sudo, no capabilities, and no way to gain privileges (`NoNewPrivileges=`), with a
  seccomp filter (`@system-service`)
- a read-only system, no `/home`, no `/boot` (where `wifi.txt` is, readable by anyone, as
  FAT has no permissions), its own `/tmp`, and no other processes to see
- one directory of its own, `/var/lib/sprite-apps/<id>` (its `$HOME`, kept between runs)
- no network unless `allow=network`, and no sound devices unless `allow=sound`
- a lower share of the CPU than sprite, and limits of 192 MB of memory, 64 MB of swap
  (zram, which is RAM too) and 64 processes; past the memory limit the kernel ends the app,
  not something else. The limits need a kernel with `MEMCG` and `CGROUP_PIDS`

It's bound to `sprite.service`, so it stops when sprite does. sprite knows an app's windows
by its unit's cgroup, and its exit by a pidfd. Starting one takes about 0.8 s on the OF1,
0.4 s of it systemd setting up the sandbox. `SPRITE_SANDBOX=0` in sprite's environment
starts apps the old way instead, as sprite's own user, for trying things out.

## Building

Builds run in an i386 Debian Trixie container, so they need Docker; on the Mac that's colima.
The repository has to be under `$HOME`, because colima only shares the home directory.

```
scripts/build.sh            # out/sprite
scripts/build.sh package    # out/openframe-sprite_<version>_i386.deb
```

The first build creates the `sprite-build` image, which compiles the Mesa-free display stack
(`build/stack.sh`). That takes a few minutes, and is cached after that. LVGL 9.3.0 is downloaded
into `third_party/` and its checksum verified (`scripts/fetch-lvgl.sh`).

CI (`.github/workflows/package.yml`) builds the package on every change to what goes into it,
installs it on a clean Trixie with the OpenFrame repository to test it, and publishes a
release. The version is year.month.N (`package/version.sh`).

## The package

`openframe-sprite` carries sprite and the display stack under `/opt/sprite`: wlroots,
Xwayland, SDL2 and sdl12-compat (for SDL apps), the root helper, the Settings app and a
template of the settings file. Its systemd units go in `/usr/lib/systemd/system`. None of it
needs Mesa: Debian's own wlroots, Xwayland and SDL2 do, at about 250 MB. It depends on
`openframe-scripts` (for `bootrw` and `of-backlight`).

The first time it's installed, it takes tty1 from the login prompt (marked in
`/var/lib/openframe-sprite`, so disabling sprite by hand afterwards sticks), and puts
`settings/sprite.txt` on OF-BOOT if there isn't one. Removing it brings the login prompt back.

## Running on an OpenFrame

```
scripts/build.sh package && OFH=of@<address> scripts/deploy.sh   # install the package
OFH=of@<address> scripts/deploy.sh noboot                        # back to the tty1 login
OFH=of@<address> scripts/deploy.sh boot                          # sprite at boot again
```

sprite reads manifests from `/opt/sprite/share/sprite/apps`, then `/usr/share/sprite/apps`
(packages), then `/usr/local/share/sprite/apps`, which is the place to try out an app by hand.

sprite runs as `sprite.service` (in `system/`), on tty1 in place of the login prompt, as
its own user, `sprite`, in a logind session, so libseat gets the display and input through
logind. `sprite` has no password, login or sudo, and keeps its state in `/var/lib/sprite`.
`of` is the person's account (with sudo), and nothing runs as it unless someone's logged
in: so an app that got the better of sprite or Xwayland, which take everything apps send
to the display, would have `sprite`, not root. `of` is in the `sprite-apps` group, so a
Wayland client started by hand can use `WAYLAND_DISPLAY=/run/sprite-apps/wayland-0`.
It starts after `of-flash`, `of-mac-gate`, `of-provision` and Plymouth, which all use tty1
first. If it keeps failing, the login prompt comes back (`OnFailure=getty@tty1.service`).
The log is in the journal: `sudo journalctl _COMM=sprite` (under the PAM session, sprite's
own lines aren't filed under the unit). `SPRITE_TRACE_INPUT=1` logs every press given to an
app.

A few things need root, and go through `sprite-helper` (`system/`), which systemd starts
for each connection to `/run/sprite-helper.sock` (group `sprite`). It takes one request and
validates it: `set <key> <value>` for `startup`, `calibration`, `clock` and `seconds` in the
settings file, `reboot` and `poweroff`, for Wi-Fi `wifi-scan`, `wifi-status`, `wifi-join`
and `wifi-forget` (see Wi-Fi below), `app-start` and `app-stop` for apps (see Sandboxes
above), and `ssh-status` and `ssh-password` for the login password (never giving out the
password or its hash). The header of `system/sprite-helper` describes each.

It also expects the touchscreen calibration in `spike/touch/` to be installed.

### Settings

`settings/sprite.txt` on the OF-BOOT volume, alongside `wifi.txt` and the rest, so it can be
edited from a PC:

```
startup=Clock      # the app to open at startup, by the name on its tile (or its id)
calibration=1.013455,-0.001630,-5.9928,-0.002758,1.047415,-8.2094
```

`calibration=` comes from Settings > Calibrate touch: nine crosses, tapped in turn, and an
affine fit of where the taps landed (x' = a x + b y + c, y' = d x + e y + f). sprite applies
it to all touch input, for apps as well as the shell, on top of the kernel's calibration
(`spike/touch/`). An empty value means none. HamClock's keyboard needed it: each key types a
different character in its top and bottom halves, 22 px each.

Setting "Open at startup" on the touchscreen writes the same file through the helper, which
makes `/boot` writable with `bootrw` just while it replaces the file with a synced copy.
`deploy.sh` puts a copy there if there isn't one. When a startup app is set, sprite keeps the
screen black until the app's window appears, rather than showing the home screen first.

### Wi-Fi

Wi-Fi is set up the OpenFrame way: `name=` and `password=` in `settings/wifi.txt` on OF-BOOT,
which `of-netplan` (openframe-linux) turns into a netplan configuration. sprite never handles
the file itself. Choosing a network in Settings sends its name and password to the helper,
which checks them (a name of 1-32 bytes, a password of 8-63 characters or 64 hex digits or
none, no control characters), writes them into `wifi.txt` in double quotes (so spaces,
quotes and backslashes are kept exactly), keeps any fixed address in the file, and starts
`of-netplan.service` to apply them. Settings then follows the connection, and says if it
doesn't work out (wpa_supplicant giving up on the network, most often for a wrong password,
or nothing after 45 s). Forget empties both.

Looking for networks uses `wpa_cli scan_results`: from netplan's wpa_supplicant when a
network is set up, or else from one the helper starts just for the scan (about 3 s), which
leaves the card down again afterwards. Networks needing WEP or an enterprise login are listed
but can't be joined from Settings (`wifi.txt` can't describe them; a `network.yaml` can).
sprite's fonts are ASCII only, so a network name with other characters shows gaps, though it
can still be joined.

## Not done yet

- Starting faster: 2.4 s pass between systemd starting sprite and sprite running, cold
- Letting apps change the home pill while they run (a small Wayland protocol), and screen
  blanking
- Sound and time zone in Settings
- The app store: an index repository of app manifests, added to by pull request, with apps
  as i386 `.deb` packages
- A CPU cap on apps in the background (`CPUQuota=`, which the kernel can do now)

## Licence

sprite is under the GNU General Public License, version 3 or (at your option) any later version
(see [LICENSE](LICENSE)). Copyright 2026 Andrew Davison.

What it's built on keeps its own terms: LVGL (MIT), downloaded into `third_party/` at build
time; the tile glyphs, from Lucide (ISC, in `src/icons_40.c`); and wlroots and Xwayland
(MIT/X11) and SDL2 and sdl12-compat (zlib), built from Debian's sources by `build/stack.sh`. The package carries all of their licences. `src/lv_conf.h` is
generated from LVGL's configuration template.
