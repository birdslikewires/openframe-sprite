// SPDX-License-Identifier: GPL-3.0-or-later
// sprite: OpenFrame touch shell. Compositor (wlroots, pixman) and LVGL shell in one process.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

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

struct sprite_view;

struct sprite_app {
	char id[64];
	char name[64];
	char exec[512];
	char icon[32];      // a keyword (see shell.c) or up to two letters
	uint32_t color;     // tile colour, 0xRRGGBB
	bool builtin;       // handled inside sprite (settings), no process
	bool hidden;        // not shown on the home screen (adopted windows)
	bool x11;           // needs Xwayland (manifest: display=x11)
	bool pending;       // waiting for Xwayland, or the helper, before it runs
	float handle;       // home handle opacity while this app shows (manifest: handle=normal|faint|hidden)
	bool bar;           // the status bar stays above its window (manifest: bar=yes); else full screen
	pid_t pid;          // launched process, 0 when not running
	int pidfd;          // a sandboxed app's main process, watched for its exit (-1 if not)
	struct wl_event_source *exit_src;
	struct sprite_view *view; // the app's main mapped window, if any
	// closing and restarting
	bool closing;       // closed from the shell: don't restart it
	int close_stage;
	struct wl_event_source *close_timer, *restart_timer;
	int quick_exits;    // exits within a minute of the first, for the startup app
	uint32_t first_exit_ms;
};

// apps.c
void apps_load(const char *dirs);             // colon-separated directories of manifests
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

// shell.c
void shell_init(void *pixels);                // pixels: SPRITE_W x SPRITE_H XRGB8888
void shell_pointer(int x, int y, bool pressed);
void shell_apps_changed(void);                // running state or list changed
void shell_set_visible(bool visible);
void shell_set_bar_app(struct sprite_app *app); // an app is in front below the bar (NULL: none)
void shell_frame(void);                       // the display wants a frame: draw what has changed
void shell_notice(const char *text);          // a banner on the home screen

// main.c: performance trace (SPRITE_TRACE_PERF=1), one line a second while anything moves
void perf_render(double ms);                  // the shell drew a frame, taking ms

// main.c provides this for shell.c's flush callback
void sprite_shell_damage(int x, int y, int w, int h, bool commit);

// settings.c: settings/sprite.txt on OF-BOOT
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
bool sysinfo_backlight_auto(void);
void sysinfo_set_backlight(const char *arg);  // "0".."100" or "auto", runs of-backlight
bool sysinfo_backlight_request(const char *req); // "show N", "resume", a level..., straight to its service
void sysinfo_name(char *name, size_t n, char *host, size_t hn); // as written (pretty), and the hostname
void sysinfo_summary(char *buf, size_t len);  // multi-line system text
void sysinfo_run(const char *cmd);            // fire and forget
#define HELPER_SOCKET "/run/sprite-helper.sock"
// Ask sprite's root helper to do something (one line, e.g. "reboot"), without waiting.
void sysinfo_helper(const char *line);
// wifi.c: a request to the helper without waiting; done() gets the whole reply (NULL if the
// helper couldn't be reached) from the event loop. helper_reply_status() splits off the
// reply's last line, "ok" or "error: <reason>", and returns it.
void helper_request(const char *text, void (*done)(char *reply, void *ctx), void *ctx);
const char *helper_reply_status(char *reply);

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
