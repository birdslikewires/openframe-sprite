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

## Using it

- **Home screen:** tap a tile to open its app. A green dot means the app is running in the
  background, and tapping it brings it back as it was. An amber dot means the app is still
  starting.
- **Going home:** while an app shows, a small pill sits at the bottom centre of the screen.
  Touch it, or swipe up from it, to go home. With a USB keyboard, Alt+Esc also goes home, and
  Ctrl+Alt+Backspace quits sprite.
- **Long press on a tile** opens a sheet with Open, Close (when the app is running) and
  **Open at startup**. Close asks the app's window to close, then sends SIGTERM after 2 s and
  SIGKILL after 4 s if it's still running.
- **The startup app** (one at most, labelled "Startup" on its tile) opens when sprite starts,
  and reopens 2 s after it closes or crashes, unless it was closed from the sheet. After three
  exits within a minute it stops reopening and says so. The choice is kept in
  `/var/lib/sprite/autostart`.
- **Settings:** brightness (through `of-backlight`, including automatic brightness),
  information about the system, and Restart and Switch off.

## Apps

An app is a small manifest file in `/opt/sprite/share/sprite/apps/`, named `<id>.app`:

```
name=Clock
exec=xclock -update 1
icon=clock        # a keyword (see icon_text in src/shell.c) or up to two letters
color=2a6f97      # tile colour
display=x11       # X11 or SDL apps; leave out for native Wayland apps
handle=faint      # home pill: normal (default), faint or hidden while the app shows
```

Apps run as the `of` user. sprite tells them to draw through X11 for SDL
(`SDL_VIDEODRIVER=x11`), because SDL's Wayland support needs EGL, which this hardware lacks.
`apps/` has the demo apps the prototype ships with.

## Building

Builds run in an i386 Debian Trixie container, so they need Docker; on the Mac that's colima.
The repository has to be under `$HOME`, because colima only shares the home directory.

```
scripts/build.sh           # out/sprite (and out/sprite.stripped, which is what gets deployed)
scripts/build.sh stack     # also out/sprite-stack.tgz: wlroots, Xwayland and SDL for /opt/sprite
```

The first build creates the `sprite-build` image, which compiles the Mesa-free display stack
(`build/stack.sh`). That takes a few minutes, and is cached after that. LVGL 9.3.0 is downloaded
into `third_party/` and its checksum verified (`scripts/fetch-lvgl.sh`).

## Running on an OpenFrame

The unit needs the stack in `/opt/sprite` (unpack `out/sprite-stack.tgz` there), and the
runtime libraries it uses from Debian:

```
sudo apt install --no-install-recommends libseat1 libdisplay-info2 libliftoff0 libinput10 \
	libxcb-composite0 libxcb-ewmh2 libxcb-icccm4 libxcb-render0 libxcb-res0 libxcb-xfixes0 \
	libxcb-errors0 libxfont2 libxcvt0 libxshmfence1 x11-xkb-utils xkb-data
```

None of these pulls in Mesa. Check any package you add for libgbm1, libegl1 or libgl1, which
do.

Then:

```
OFH=of@<address> scripts/deploy.sh run
```

That copies sprite, `scripts/sprite-run` and the apps to the unit, and starts sprite on the
panel in place of the tty1 login, as a transient systemd unit. `sudo systemctl stop sprite`
stops it and brings the login back. The log is in `/run/sprite.log`.

For the prototype, sprite runs as root with libseat's builtin seat, and drops to `of` for
apps. It also expects the touchscreen calibration in `spike/touch/` to be installed.

## Not done yet

- Starting at boot, and running as `of` through logind rather than as root
- Letting apps change the home pill while they run (a small Wayland protocol), and screen
  blanking
- Wi-Fi setup, sound, time zone
- The app store: an index repository of app manifests, added to by pull request, with apps
  as i386 `.deb` packages
