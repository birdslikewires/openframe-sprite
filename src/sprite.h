// SPDX-License-Identifier: GPL-3.0-or-later
// sprite: OpenFrame touch shell. Compositor (wlroots, pixman) and LVGL shell in one process.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include "sprite_strings.h" // the on-screen text, STR_*: generated from src/strings/en_gb.txt (Makefile)

#ifndef SPRITE_VERSION // the package build sets it (package/version.sh)
#define SPRITE_VERSION "dev"
#endif
#define SPRITE_W 800
#define SPRITE_H 480
#define SPRITE_BAR_H 40 // the status bar, which an app can keep above it (manifest: bar=yes)

// Going home. While an app shows, a pill-shaped handle sits at the middle of the home edge
// (bottom by default; SPRITE_HOME_EDGE=top|bottom), and a touch that starts under it goes
// home at once. It's a touch rather than a swipe because the OF1's controller sends its
// first movement sample about 60 ms after contact: a quick flick often arrives as a touch at
// the edge and nothing more, and a quick tap looks the same. Distances are measured from the
// edge (see edge_dist in main.c).
#define SPRITE_EDGE_PX 20
// A touch starting a little further out goes to the app, but goes home instead if it then
// travels SPRITE_EDGE_SWIPE_PX away from the edge, so swipes that start short still work and
// taps near the edge of an app stay with the app.
#define SPRITE_EDGE_BAND_PX 32
#define SPRITE_EDGE_SWIPE_PX 30
// Only the middle of the edge counts: accidental bumps while using an app land as close to
// the edge as deliberate touches, but anywhere along it.
#define SPRITE_EDGE_HALF_W 80
#define SPRITE_HANDLE_W (2 * SPRITE_EDGE_HALF_W) // the handle shows the whole zone
#define SPRITE_HANDLE_H 6
#define SPRITE_HANDLE_Y 5  // gap between the handle and the screen edge
// Calibrated touch reaches y = 9 at the top but only y = 462 at the bottom, so the bottom
// edge is measured from y = 465. Touches aimed at the bottom bezel register at y = 458-462,
// and at the top bezel at y = 10-19.
#define SPRITE_BOTTOM_REACH 465
// Handle opacity. An app's manifest can ask for it faint or hidden; the touch zone works the
// same either way. It shows at normal strength when an app comes up, then fades.
#define SPRITE_HANDLE_NORMAL 0.35f
#define SPRITE_HANDLE_FAINT 0.10f
#define SPRITE_HANDLE_HOLD_MS 2000
#define SPRITE_HANDLE_FADE_MS 250
// The OpenFrame's resistive touchscreen briefly loses contact during fast strokes: it reports
// a release and then a new press 28-56 ms later, further along. A release is held this long,
// and a press within it is taken as the same finger still moving.
#define SPRITE_DEBOUNCE_MS 64
// ... and while one of the shell's sliders is held, longer: a finger working one loses contact
// for up to about a third of a second at a time, several times a second (traced on a unit).
// Past SPRITE_DEBOUNCE_MS, a press counts as the same stroke only within SPRITE_BRIDGE_PX up
// or down of where it left off; anywhere else it's a new touch, the slider let go first.
#define SPRITE_SLIDER_HOLD_MS 400
#define SPRITE_BRIDGE_PX 60

struct sprite_view;

struct sprite_app {
	char id[64];
	char name[64];
	char exec[512];
	char icon[32];      // a keyword (see shell.c) or up to two letters
	uint32_t color;     // tile colour, 0xRRGGBB
	bool builtin;       // handled inside sprite (settings), no process
	bool hidden;        // not shown on the home screen (adopted windows)
	bool gone;          // its manifest has gone (the app was removed): kept, as things may still point at it, but not shown or opened
	bool x11;           // needs Xwayland (manifest: display=x11)
	bool pending;       // waiting for Xwayland, or the helper, before it runs
	float handle;       // home handle opacity while this app shows (manifest: handle=normal|faint|hidden)
	bool bar;           // the status bar stays above its window (manifest: bar=yes); else full screen
	bool saver;         // a screensaver (manifest: kind=screensaver): no tile, chosen in Display
	pid_t pid;          // launched process, 0 when not running
	int pidfd;          // a sandboxed app's main process, watched for its exit (-1 if not)
	struct wl_event_source *exit_src;
	struct sprite_view *view; // the app's main mapped window, if any
	// closing and restarting
	bool closing;       // closed from the shell: don't restart it
	bool reopening;     // closed to be opened again at once (apps_restart)
	bool close_on_start; // closed while it was still starting: stopped once it's running
	bool reopen_behind; // and its window to come back behind what's showing, not in front
	int close_stage;
	struct wl_event_source *close_timer, *restart_timer;
	int quick_exits;    // exits within a minute of the first, for the startup app
	uint32_t first_exit_ms;
};

// apps.c
void apps_load(const char *dirs);             // colon-separated directories of manifests, watched from then on
void apps_rescan(void);                       // read the manifests again (an app installed, updated or removed)
int apps_count(void);
struct sprite_app *apps_get(int i);
struct sprite_app *apps_for_pid(pid_t pid);  // the app a process belongs to (its unit, or parents)
bool apps_sandboxed(void);                    // apps start through the helper, in sandboxes
void apps_launch(struct sprite_app *app);
bool apps_reap(void);                         // collect exited apps; true if any app changed
void apps_set_env(const char *wayland_display, const char *x_display);
void apps_x11_ready(bool ready);              // the X keeper has (or no longer has) opened X to apps
void apps_close(struct sprite_app *app);      // SIGTERM, then SIGKILL, unless it goes by itself
void apps_end_all(void);                      // at exit: end every app sprite launched
struct sprite_app *apps_autostart(void);      // the app opened at startup, or NULL
void apps_restart(struct sprite_app *a, bool behind); // close it and open it again
struct sprite_app *sprite_foreground(void);   // the app on screen, NULL for the shell
void apps_set_autostart(struct sprite_app *app, bool on);

// main.c, for the shell
void sprite_open_app(struct sprite_app *app); // show it, launching it if needed
void sprite_go_home(void);
void sprite_close_app(struct sprite_app *app); // ask its windows to close, then end it
// Keep the screen black, rather than show the home screen, until this app's window appears
// (the startup app, at startup and when it's reopened). NULL shows the home screen again.
void sprite_hold_for(struct sprite_app *app);
void sprite_lvgl_kick(void);                  // run the LVGL timer handler soon
void sprite_request_frame(void);              // the shell has something to draw at the next refresh
struct wl_event_loop *sprite_event_loop(void);

// shop.c: what the Shop offers and what's installed (apps.openbeak.net's ofapp-* packages), and the helper's job installing or removing one
#define SHOP_MAX 64
struct shop_app {
	char id[48], package[56];       // its id (its manifest is <id>.app) and its package, ofapp-<id>
	char name[64], icon[32], summary[128], description[2048]; // the description's paragraphs a line each
	char author[128], licence[64], source[256], community[256], allow[64];
	uint32_t color;
	bool saver;                     // a screensaver (kind=screensaver)
	char version[64];               // the newest the Shop offers (or, if it doesn't offer it, what's installed)
	char installed[64];             // the version installed, or "" if it isn't
	bool listed;                    // the Shop offers it (not, if it was installed by hand or has left the Shop)
	long size_kb;
};
void shop_load(void);                         // read the lists and dpkg's status again
int shop_count(void);
struct shop_app *shop_get(int i);             // by name
struct shop_app *shop_find(const char *id);
int shop_vercmp(const char *a, const char *b); // Debian's version order: <0, 0 or >0
long shop_lists_age(void);                    // seconds since the lists were fetched, -1 if never
enum shop_job_state { SHOP_JOB_NONE, SHOP_JOB_RUNNING, SHOP_JOB_DONE, SHOP_JOB_FAILED };
struct shop_job {
	enum shop_job_state state;
	bool remove;                    // a removal (else an install)
	char id[48];
	int percent;
	char reason[96];                // when it failed: "error: helper.shop_*"
};
void shop_job_read(struct shop_job *j);

// shell.c
void shell_init(void *pixels);                // pixels: SPRITE_W x SPRITE_H XRGB8888
void shell_pointer(int x, int y, bool pressed);
uint32_t shell_release_hold_ms(void);        // how long a lift is held before it's a release
void shell_apps_changed(void);                // running state or list changed
void shell_set_visible(bool visible);
void shell_set_bar_app(struct sprite_app *app); // an app is in front below the bar (NULL: none)
void shell_frame(void);                       // the display wants a frame: draw what has changed
void shell_notice(const char *text);          // a banner on the home screen
void shell_wake(void);                        // display sleep: a touch woke the screen
bool shell_dark(void);                        // the screen's off in the dark (automatic brightness)
void shell_screensaver_tap(void);             // a touch while the screensaver's up: it goes
bool shell_clock_24(void);                    // the clock's a 24-hour one (Settings, Display)
bool shell_time_known(void);                  // the time's been synced this boot (till then, zeros)
// screensaver.c: Sprite's own screensaver, the split-flap clock
void screensaver_build(void);
void screensaver_start(void);
void screensaver_stop(void);
bool screensaver_active(void);
// main.c, for display sleep: how long since a touch or key, whether a surface asks for no
// sleep (idle-inhibit), and marking the screen asleep (the next touch only wakes it)
uint64_t sprite_idle_ms(void);
bool sprite_sleep_inhibited(void);
void sprite_set_asleep(bool asleep);
void sprite_set_covered(bool covered); // the shell over everything, an app's too (the screensaver)
void sprite_saver_start(struct sprite_app *app); // a screensaver app over everything, once it shows
void sprite_saver_end(void);                  // and closed (a touch, or the screen going off)

// main.c: performance trace (SPRITE_TRACE_PERF=1), one line a second while anything moves
void perf_render(double ms);                  // the shell drew a frame, taking ms

// main.c provides this for shell.c's flush callback
void sprite_shell_damage(int x, int y, int w, int h, bool commit);

// settings.c: settings/interface.txt on OF-BOOT
void settings_load(void);
const char *settings_get(const char *key);        // NULL when unset or empty
void settings_set(const char *key, const char *value); // in memory now, on disk via the helper

// main.c: touch calibration, an affine correction applied to all touch input on top of the
// kernel's (hwdb) calibration: x' = m0 x + m1 y + m2, y' = m3 x + m4 y + m5.
void sprite_set_calibration(const double m[6]);
void sprite_get_calibration(double m[6]);
void sprite_calibrating(bool on);                 // pass input uncorrected, for measuring

// sysinfo.c
struct sprite_net {
	bool eth_up, wifi_up;
	bool eth_present;     // there's a wired interface (every OpenFrame has one)
	int eth_mbps;         // its link speed, with a cable in; 0 when unknown
	bool wifi_present;    // there's a Wi-Fi interface
	bool wifi_enabled;    // ... and it's switched on (interface up, radio not blocked)
	char eth_ip[48], wifi_ip[48];
	int wifi_dbm;         // the signal of the network joined, in dBm; 0 when unknown
	bool notes;           // of-netplan has something to say about the settings files
};
void sysinfo_net(struct sprite_net *out);
// Where an address is shown alone (to log in to, say), it's the wired one when there's both:
// faster and steadier, so that's where people are pointed.
static inline const char *sprite_net_best_ip(const struct sprite_net *n) {
	return n->eth_ip[0] ? n->eth_ip : n->wifi_ip;
}
// The details behind them, for Settings' network page: read-only, from the kernel,
// systemd-resolved and of-netplan (never the settings files, which hold the Wi-Fi password).
struct sprite_netif {
	char ip[48];          // the IPv4 address with its prefix, 192.168.1.50/24, or ""
	bool fixed;           // set in ethernet.txt or wifi.txt (a permanent address), not handed out
	char gateway[16];     // the default gateway through it, or ""
	char mac[18];
};
#define NET_DNS_MAX 3
struct sprite_netinfo {
	char host[65];
	struct sprite_netif eth, wifi;
	char dns[NET_DNS_MAX][46];  // the DNS servers asked, IPv4 first
	int n_dns;
	char notes[640];      // of-netplan's problems with the settings files, a line each, or ""
};
void sysinfo_netinfo(struct sprite_netinfo *out);
int wifi_signal(const char *ifname);          // dBm of the network joined (nl80211), 0 if unknown
int sysinfo_backlight(void);                  // 0-100, -1 if unknown
int audio_volume(const char *card, const char *ctl);               // 0-100 (as it sounds), -1 if unknown
bool audio_set_volume(const char *card, const char *ctl, int pct); // audio.c: ALSA's mixer, in-process
bool audio_test_sound(void);   // left, then right, then both (false if it's already playing)
int audio_test_state(void);    // 0 idle, 1 left, 2 right, 3 both, -1 it couldn't play
void audio_test_seen(void);    // a "couldn't" has been said
struct audio_dac {
	char name[80], speed[24];    // "Audient iD4", "high speed"
	int bits_min, bits_max, rate_min, rate_max, channels;
	bool async;                  // it keeps the time itself (an asynchronous USB endpoint)
	bool running;                // playing now, with
	int run_bits, run_rate;      // the format in use and the momentary rate
	bool hw_known;               // the kernel has hw_params in /proc (6.18.65 on): then, as it plays,
	int hw_bits, hw_rate;        // the bits and the rate it was opened at (0 while closed)
};
bool audio_dac_info(const char *card, struct audio_dac *d); // a USB DAC's connection (/proc)
int sysinfo_backlight_max(void);              // the hardware's highest level, -1 if unknown
bool sysinfo_backlight_auto(void);
void sysinfo_set_backlight(const char *arg);  // "0".."100" or "auto", runs of-backlight
bool sysinfo_backlight_request(const char *req); // "show N", "resume", a level..., straight to its service
void sysinfo_name(char *name, size_t n, char *host, size_t hn); // as written (pretty), and the hostname
// The About tab's details, each ready to show (sysinfo_about); empty where there's none
struct sprite_about {
	char model[48], firmware[96], cpu[64], memory[48], storage[80];
	char eth[64], wifi[96], up[40], sprite[24], system[32], kernel[72], crystalhd[48];
	bool wifi_present, test_unit;
};
void sysinfo_about(struct sprite_about *a);
void sysinfo_run(const char *cmd);            // fire and forget
#define HELPER_SOCKET "/run/sprite-helper.sock"
// Ask sprite's root helper to do something (one line, e.g. "reboot"), without waiting.
void sysinfo_helper(const char *line);
// wifi.c: a request to the helper without waiting; done() gets the whole reply (NULL if the
// helper couldn't be reached) from the event loop. helper_reply_status() splits off the
// reply's last line, "ok" or "error: <reason>", and returns it.
void helper_request(const char *text, void (*done)(char *reply, void *ctx), void *ctx);
const char *helper_reply_status(char *reply);
const char *helper_reason(const char *st);   // an error reply as a sentence to show (en_gb.txt)

/* wifi.c: Wi-Fi through the helper, which owns settings/wifi.txt. Each call answers later,
   from the event loop; error is NULL on success, or the reason. */
#define WIFI_SSID_MAX 32
#define WIFI_NETS_MAX 40
struct wifi_net {
	char ssid[WIFI_SSID_MAX + 1];   // UTF-8, as it should be shown
	int dbm;                        // signal
	bool secured;                   // needs a password
	bool supported;                 // joinable from wifi.txt: not WEP or enterprise-only
};
struct wifi_status {
	bool present;                   // there's a Wi-Fi interface (and the helper answered)
	bool connected;                 // joined, with an address
	bool trouble;                   // wpa_supplicant has given up on the network for now
	char state[24];                 // wpa_supplicant's: COMPLETED, SCANNING, ... or empty
	char ssid[WIFI_SSID_MAX + 1];   // the network joined
	char saved[WIFI_SSID_MAX * 4 + 1]; // the network in settings/wifi.txt, if any
	char address[16];
};
typedef void (*wifi_scan_cb)(const struct wifi_net *nets, int n, const char *error, void *data);
typedef void (*wifi_status_cb)(const struct wifi_status *s, const char *error, void *data);
typedef void (*wifi_done_cb)(const char *error, void *data);
void wifi_scan(wifi_scan_cb cb, void *data);       // strongest first, one per name
void wifi_status(wifi_status_cb cb, void *data);
void wifi_join(const char *ssid, const char *password, wifi_done_cb cb, void *data);
void wifi_forget(wifi_done_cb cb, void *data);
int wifi_level(int dbm);                          // 1-4 bars
