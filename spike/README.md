# sprite: display stack spike, 2026-11-21

Hardware checks before building the sprite shell. The question was whether a Wayland compositor
drawn entirely in software, with LVGL for the UI and Xwayland for X11 and SDL apps, works on an
OpenFrame 1 and fits its 512 MB RAM and 1 GB MMC.

Test unit: OF1 at 192.168.11.131, Atom Z520, kernel 6.18.64-openframe, Debian Trixie i386,
openframe-scripts 1:26.11.112, booted from a USB stick. At idle after boot it used 56 MB of RAM
and 448 MB of disk.

## Results

**wlroots drives the gma500 panel tear-free.** It uses DRM dumb buffers and legacy modesetting,
with no GBM or Mesa. Every frame came back from `wp_presentation` with VSYNC, HW_CLOCK and
HW_COMPLETION, at intervals of 16.67 ms.

| Test, under cage with the pixman renderer, 600 frames | Frame rate | Compositor CPU | Compositor RSS |
|---|---|---|---|
| Full-screen repaint every frame | 60.0 fps, p99 16.72 ms, 0 late | 12-14% of one thread (about 2 ms/frame) | 10-11 MB |
| 64x64 square moving over a still screen | 60 fps | 4% | 10-11 MB |
| Idle, nothing changing | - | 0% | 9-10 MB |

The numbers were the same on Debian's wlroots and on our Mesa-free build.

**Debian's wlroots, Xwayland and SDL2 pull in all of Mesa.** `libwlroots-0.18` depends on
libgbm1, libegl1, libgles2 and libvulkan1. `xwayland` depends on libgbm1, libgl1 and libepoxy0.
`libsdl2-2.0-0` depends on libgbm1. Installing them added about 250 MB: libllvm19 143 MB,
mesa-libgallium 44 MB, libz3 29 MB, and so on. None of it can be used on this GPU.

**A Mesa-free build is about 10 MB on disk.** We build it ourselves in an i386 Trixie container
on the Mac (`stack/`):

- wlroots 0.18.2 built with `-Drenderers= -Dallocators=`, which leaves pixman only: 1.45 MB.
- Xwayland 24.1.6 built with `-Dglamor=false -Dglx=false`: 2.4 MB.
- cage 0.2.0 (kiosk compositor, a stand-in for the sprite compositor): 77 KB.
- SDL2 2.32.4 with X11 and ALSA only, plus sdl12-compat 1.2.68.
- Runtime libraries from Debian: about 6.5 MB, of which about 3.3 MB were new on this image.
  They are libinput, pixman, libseat, libdisplay-info, libliftoff, the xcb extensions, libXfont2,
  libxcvt, libxshmfence and xkbcomp.

With Mesa, LLVM, GBM, EGL and Vulkan purged from the unit, everything below ran.

**X11 and SDL 1.2 apps work through Xwayland.** Xwayland starts on demand when the first X
client connects.

| Client | Result | Xwayland |
|---|---|---|
| `x11test` (shaped like HamClock): 800x480 window, full XPutImage + XSync every frame | 56.7 fps; X connected 351 ms after launch | 12 MB RSS |
| `sdl12test` (shaped like SqueezePlay): SDL 1.2 software surface, fill + flip every frame | 73 fps (SDL_Flip isn't vsynced) | 13 MB RSS |

These ran while an LVGL build was using the CPU, so they slightly understate the hardware.

**SDL2's Wayland backend needs EGL.** The build won't enable it without `egl` and `wayland-egl`.
So SDL apps on a GPU-less system draw through X11 (Xwayland) with XShm. That's fine, since
SqueezePlay is SDL 1.2.

**LVGL 9.3.0's software renderer is fast enough.** Measured with `lv_demo_benchmark` straight to
`/dev/fb0` (so it can tear), 2 render threads, 16 ms refresh period:

| | Average over all scenes | Widgets demo | Containers with overlay |
|---|---|---|---|
| fps | 59 | 50 | 51 |
| Render time | 5 ms | 15 ms | 17 ms |

With 1 render thread instead of 2: 8 ms average render time, 22 ms for the widgets demo. A second
draw unit is worth having on the Z520, which has hyperthreading.

**LVGL's stock Wayland driver is slow here.** Running as a Wayland client under cage, 2 threads:

- Average 46 fps, 10 ms render plus 7 ms flush per frame.
- Widgets demo at 17 fps: 45 ms render plus 13 ms flush.
- Full-screen scenes spend 15-16 ms per frame in flush.

The driver in 9.3 supports only partial render mode on shm, with a single buffer. Each flush
copies the frame and waits on the compositor, and the copying and the compositor's own CPU use
also slow down rendering. Two conclusions:

- The sprite shell should render LVGL straight into the compositor's own buffers, in the same
  process, not as a Wayland client.
- First-party LVGL apps will need a better Wayland flush than LVGL's stock one: direct mode, two
  shm buffers, damage only.

**Touch.** The panel's touch controller (`AmSC OpenPeak Touchscreen Hyup05_3`, USB 04b4:1974)
reports as an absolute-position mouse: BTN_LEFT, BTN_RIGHT and BTN_MIDDLE, with ABS_X and ABS_Y
both 0-32639, and udev tags it ID_INPUT_MOUSE. libinput and wlroots map it onto the output as an
absolute pointer, so a single-touch shell treats press, motion and release as touch.

The factory axis range overshoots the visible screen. With the range as reported, finger taps on
five targets were accurate at the centre, but landed further out towards the edges: about 2%
horizontally and 6% vertically, up to 16 px at the corners. A fitted range applied through udev
hwdb fixes it: `touch/61-openframe-touchscreen.hwdb`, installed in `/etc/udev/hwdb.d/` on the
test unit. Run `systemd-hwdb update` after installing it, then retrigger the device.

| Target | Before (average of taps) | After (one tap) |
|---|---|---|
| 400,240 | 401,245 | 394,238 |
| 20,20 | 32,14 | 10,28 |
| 780,20 | 789,8 | 775,13 |
| 20,460 | 6,476 | 16,459 |
| 780,460 | 791,476 | 788,459 |

After calibration the worst error was 10 px and the errors had no common direction, which looks
like finger scatter. With the new range, raw 0 and 32639 map about 8-10 px inside the screen
edges, so edge gestures must start within about 20 px of an edge.

Still open:
- Only one unit was measured, with one or two finger taps per target.
- Check another OF1, and the OF2's touchscreen, before shipping the range in openframe-linux.
- Consider a calibration screen in sprite's settings, in case units differ.

To repeat the test, run `runapp.sh SDL_VIDEODRIVER=x11 ./sdl12test 1 60`. It shows five
targets and prints where each tap lands.

## Other findings

- This image keeps the apt package cache in RAM: `/var/cache/apt` is a 256 MB tmpfs. Installing
  about 250 MB of packages left 177 MB of `.deb` files there, which showed up as "shared" memory,
  until `apt-get clean`. sprite's store must run `apt-get clean` after every install, or the
  image should cap or move that tmpfs.
- wlroots logs `Failed to parse EDID` for the LVDS panel. It's harmless, but the output has no
  make or model.
- `cage` 0.2 has no `-D` debug flag.
- `libseat`'s builtin backend forks a child process that is also named `cage`. Use `pgrep -o`,
  not `pgrep -n`, to measure the compositor itself.

## Files

- `wlbench/`: Wayland shm client that records `wp_presentation` feedback. Usage:
  `wlbench <frames> full|small|idle [secs]`.
- `runcage.sh`, `runapp.sh`: run a client under cage on the panel as root, and report the CPU
  and peak RSS of cage and Xwayland. Both stop `getty@tty1`. Afterwards run
  `systemctl start getty@tty1`, and put the cursor back with
  `echo 1 > /sys/class/graphics/fbcon/cursor_blink`.
- `apptests/`: `x11test.c` and `sdl12test.c`.
- `lvbench/`: LVGL 9.3.0 config and runner (`lvbench fb|wl bench|widgets`). The build expects
  LVGL's `src`, `demos/benchmark` and `demos/widgets` in `lvbench/lvgl/`.
- `stack/`: container build scripts, run with
  `docker run --rm --platform linux/386 -v <dir>:/w i386/debian:trixie ...`. The `<dir>` must be
  under `$HOME`, because colima only shares the home directory. Unpack sources inside the
  container's own filesystem: SDL's tarball fails to extract onto the shared folder.

## Left on the test unit

- Our stack in `/opt/sprite`.
- Test programs and logs in `~/sprite-spike`.
- gcc, make, and the Wayland, libinput and DRM dev headers. The package list from before this
  work is in `~/pkgs-before-sprite.txt`.
- The runtime libraries listed above.
- The touchscreen calibration in `/etc/udev/hwdb.d/61-openframe-touchscreen.hwdb`.

Debian's wlroots, cage, Xwayland and Mesa have been purged again.
