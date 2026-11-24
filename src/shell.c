// SPDX-License-Identifier: GPL-3.0-or-later
// The shell: status bar, home screen of app tiles, settings. LVGL, rendered in software
// into the buffer the compositor shows whenever no app is on screen.
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/lvgl_private.h" // lv_theme_t, to layer sprite's rules over LVGL's default theme
#include "sprite.h"
#include "icons_table.h"
#include "ui_icons.h"

#define BAR_H SPRITE_BAR_H
// Corner radii follow one rule for every box (icons, buttons, cards, banners, highlights): in
// proportion to the box's shorter side, as an app icon's 18 px is to its 88, up to 18 px. A
// fixed radius would make small boxes look round. Only circles and pills (dots, the home pill,
// switches, slider ends) are rounder. See rounded().
#define UI_RADIUS 18                                        // the most any box gets
#define SLIDER_KNOB_PAD 4 // how far a slider's knob reaches past the bar, all round
#define SLIDER_KNOB_GROW 3 // and how much more while it's held
#define BL_SLIDER_H 24    // the brightness slider's bar
#define BAR_VOL_H 16      // the volume slider the bar's speaker opens, in the bar
#define BAR_VOL_MS 2000   // and how long it stays once it's let go
// Room for a slider's knob, held, past either end of a slider h high: half its height, what
// the knob reaches past it, what it grows by held, and 2 to spare. In the padding of what holds
// it: a slider's margin in a flex row wasn't kept, and the held knob was clipped.
#define SLIDER_ROOM(h) ((h) / 2 + SLIDER_KNOB_PAD + SLIDER_KNOB_GROW + 2)
#define BAR_VOL_ROOM SLIDER_ROOM(BAR_VOL_H)
#define RADIUS_RATIO (18.0 / 88.0)
// Where content starts on every screen, below the status bar: the home grid's first icon and
// Settings' first control sit at the same point, (CONTENT_X, BAR_H + CONTENT_TOP), and the
// clock in the bar lines up with them.
#define CONTENT_X 36
#define CONTENT_TOP 20
// Every screen's back button (Home in Settings' rail, Settings on the Wi-Fi screen, and so
// on) is in the same place, top left: BACK_X from the left, CONTENT_TOP below the bar, and the
// one button height and one width (that of Settings' tabs), so it doesn't jump about from
// screen to screen.
#define BACK_X 16
#define BACK_W (RAIL_W - 2 * BACK_X) // 176: the width of Settings' tabs
#define NET_ICON 20                                         // the network icons in the bar
#define GEAR_W 40                                           // the Settings gear's touch area (and each bar icon's space)
#define CLOCK_PAD 8                                         // the clock's touch target, either side of it
#define BAR_EDGE 2                                          // the gear in from the bar's right end, and the clock from its left
// The home grid: 6 columns by 3 rows of apps, as OpenPeak's own home screen had. Each app has a
// cell (the whole cell takes the touch) with an icon and a label of up to two lines below it.
#define GRID_COLS 6
#define GRID_ROWS 3
#define GRID_PAD_X 16                                       // 16 + 6 x 128 + 16 = 800
#define GRID_PAD_TOP 10                                     // 40 + 10 + 3 x 140 = 470 of 480
#define TILE_W ((SPRITE_W - 2 * GRID_PAD_X) / GRID_COLS)  // 128
#define TILE_H 140
#define ICON_SIZE 88
#define ICON_RADIUS 18                                      // ICON_SIZE x RADIUS_RATIO
#define ICON_TOP 10                                         // room for the startup ring inside the cell
#define LABEL_GAP 6
#define LABEL_H 36                                          // two lines of 16 px Montserrat (18 px each)
#define STARTUP_RING_W 3                                    // the startup app's ring
#define STARTUP_RING_PAD 2
_Static_assert(ICON_TOP - STARTUP_RING_W - STARTUP_RING_PAD >= 5, "the startup ring sits inside the press highlight");
_Static_assert(STARTUP_RING_W + STARTUP_RING_PAD < LABEL_GAP, "the startup ring clears the label");
_Static_assert(ICON_TOP + ICON_SIZE + LABEL_GAP + LABEL_H <= TILE_H, "the cell holds its icon and label");
#define PER_PAGE (GRID_COLS * GRID_ROWS)                    // further apps go on further pages
_Static_assert(GRID_PAD_X + (TILE_W - ICON_SIZE) / 2 == CONTENT_X, "the grid's first icon is at CONTENT_X");
_Static_assert(GRID_PAD_TOP + ICON_TOP == CONTENT_TOP, "the grid's first icon is at CONTENT_TOP");

#define C_BG 0x131920
#define C_BAR 0x050709
#define C_TEXT 0xe8edf2
#define C_DIM 0x8b97a3
#define C_ACCENT 0x3d9be9
#define C_WARN 0xf2b33d    // something to put right (and an app starting)

static lv_display_t *disp;
static lv_indev_t *indev;
static int ptr_x, ptr_y;
static bool ptr_pressed;
static lv_obj_t *bar_clock, *bar_date, *bar_title, *net_eth, *net_wifi, *net_warn, *net_box, *net_tab_dot, *upd_box, *upd_tab_dot;
static lv_obj_t *bar_left, *aud_box, *bar_vol, *bar_vol_slider; // the bar's speaker, and its volume slider
static lv_obj_t *page_home, *pager, *strip, *page_settings, *page_wifi, *page_entry, *page_netinfo, *page_news, *toast;
enum { NETINFO_ETH, NETINFO_WIFI };
static int netinfo_kind; // the interface page_netinfo is about: the row tapped
static lv_obj_t *dots, *gear;         // page dots and the Settings gear, in the status bar
static int cur_page, n_pages;
// Settings' tabs (see build_settings)
#define RAIL_W 208
#define RAIL_PAD BACK_X
#define TAB_H 50
#define C_RAIL 0x0d1217   // a step between the bar and the background
enum { TAB_AUDIO, TAB_DISPLAY, TAB_NETWORK, TAB_UPDATES, TAB_ABOUT, TAB_POWER, N_TABS };
static lv_obj_t *tab_btn[N_TABS], *tab_icon[N_TABS], *tab_panel[N_TABS];
static int settings_tab;
static int settings_tab_first = -1; // the top of the rail: where the gear opens Settings
static void settings_show_tab(int t);
static void aud_refresh(void);
static void bar_aud_update(void);
static void bar_aud_icon(int v);

// Paging: the pages sit side by side on a strip, which follows a sideways drag and then
// slides to a page. A touch becomes a page drag once it has moved PAGE_DRAG_START sideways
// (until then it can still be a tap or long press on an app); it turns the page if it went
// PAGE_TURN_PX, or was moving at PAGE_FLICK_SPEED when it lifted.
#define PAGE_DRAG_START 12
#define PAGE_TURN_PX 60
#define PAGE_FLICK_SPEED 0.4       // px/ms
#define PAGE_SLIDE_MS 180
// Scrolling (Settings, and any screen taller than the display) works the same way: the content
// follows the finger, a third as far past either end, carries on through the release hold,
// glides on after a flick (as far as SCROLL_GLIDE_MS at the flick's speed, easing out), and
// springs back from past an end in PAGE_SLIDE_MS.
#define SCROLL_GLIDE_MS 300
#define SCROLL_GLIDE_MIN_SPEED 0.15  // px/ms
static enum { DRAG_NONE, DRAG_MAYBE, DRAG_PAGING, DRAG_SCROLL, DRAG_TILE } drag;
static int drag_x0, drag_y0;
static bool drag_can_page;           // the touch began where a sideways drag turns pages
static lv_obj_t *drag_scroll;        // ... or where an up/down drag scrolls this
static int scroll_y0, scroll_max;
static void drag_input(int x, int y, bool pressed, bool was_pressed);
static void tile_drop(bool place);
static void tile_follow(int x, int y);
static lv_obj_t *bl_slider, *bl_auto;
// About's details: a row each (an icon, its name, what it is), in a card
enum { AB_MODEL, AB_FIRMWARE, AB_CPU, AB_MEMORY, AB_STORAGE, AB_ETH, AB_WIFI, AB_UP, AB_SPRITE, AB_SYSTEM,
       AB_KERNEL, AB_CRYSTALHD, AB_TEST, N_ABOUT };
static lv_obj_t *about_row[N_ABOUT], *about_val[N_ABOUT];
static void about_show(void);
static lv_obj_t *bl_hint, *bl_off_row; // automatic brightness: what the range means; off in the dark
static lv_obj_t *bl_now;                // the level now, by the heading
static void bl_mode_show(bool autobl);
static void bl_limits_ask(void);
static const char *reason_text(const char *st);
static void dialog_cancel(lv_event_t *e);
static bool visible = true, rebuild_pending;
static struct sprite_app *opening;
static lv_timer_t *toast_timer;
static lv_timer_t *hold_timer;        // polls input while a finger is down (long press)
static lv_obj_t *sheet;               // the long-press sheet for one app
static struct sprite_app *sheet_app;
static void sheet_dismiss(void);
static void dialog_close(void);

// Touch calibration: crosses at these points, tapped in turn.
#define CAL_N 9
static const int cal_pts[CAL_N][2] = {
	{ 60, 60 }, { 400, 60 }, { 740, 60 },
	{ 60, 240 }, { 400, 240 }, { 740, 240 },
	{ 60, 420 }, { 400, 420 }, { 740, 420 },
};
static lv_obj_t *cal;                  // the calibration screen, while it's up
static int cal_i;                      // which cross is showing
static double cal_raw[CAL_N][2];       // where each tap was reported, uncorrected
static double cal_sx, cal_sy;          // sums of positions during the current touch
static int cal_n;
static bool cal_collecting;
static double cal_prev[6];             // the calibration in use before, to put back on Cancel
static double cal_new[6];
static void cal_tap(double x, double y);

/* ---- the radius rule ---- */

static int radius_for(int w, int h) {
	int r = (int)((w < h ? w : h) * RADIUS_RATIO + 0.5);
	return r < UI_RADIUS ? r : UI_RADIUS;
}

static void radius_on_size(lv_event_t *e) {
	lv_obj_t *o = lv_event_get_target(e);
	lv_obj_set_style_radius(o, radius_for(lv_obj_get_width(o), lv_obj_get_height(o)), 0);
}

// Give a box the rule's radius, now and whenever its size changes.
static void rounded(lv_obj_t *o) {
	lv_obj_add_event_cb(o, radius_on_size, LV_EVENT_SIZE_CHANGED, NULL);
	lv_obj_update_layout(o);
	lv_obj_set_style_radius(o, radius_for(lv_obj_get_width(o), lv_obj_get_height(o)), 0);
}

/* ---- display and input glue ---- */

static uint32_t tick_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static double render_start;

static double now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// SPRITE_TRACE_PERF=3 also logs the areas each frame redraws, to find what's redrawn needlessly
static int trace_areas = -1;

static void refr_event(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
		render_start = now_ms();
		if (trace_areas < 0) trace_areas = getenv("SPRITE_TRACE_PERF") && atoi(getenv("SPRITE_TRACE_PERF")) >= 3;
		if (trace_areas) {
			lv_display_t *d = lv_event_get_target(e);
			char t[512];
			int n = 0;
			for (uint32_t i = 0; i < d->inv_p && n < (int)sizeof t - 40; i++)
				if (!d->inv_area_joined[i])
					n += snprintf(t + n, sizeof t - n, " %dx%d@%d,%d", (int)lv_area_get_width(&d->inv_areas[i]),
						      (int)lv_area_get_height(&d->inv_areas[i]), (int)d->inv_areas[i].x1, (int)d->inv_areas[i].y1);
			fprintf(stderr, "sprite: redraw%s\n", t);
		}
	} else {
		perf_render(now_ms() - render_start);
	}
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
	// DIRECT mode: LVGL has already drawn into the shared buffer; just pass on the damage.
	sprite_shell_damage(a->x1, a->y1, lv_area_get_width(a), lv_area_get_height(a), lv_display_flush_is_last(d));
	lv_display_flush_ready(d);
}

static void indev_read(lv_indev_t *i, lv_indev_data_t *data) {
	data->point.x = ptr_x;
	data->point.y = ptr_y;
	data->state = ptr_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void hold_read(lv_timer_t *t) { lv_indev_read(indev); }

// The touchscreen reports about 36 times a second, so a drag drawn as reported moves in
// uneven steps at 60 frames a second. While a finger is down, each frame instead gets the
// position one report period ago, interpolated between the last two reports: smooth, a
// report behind. Presses and releases go to LVGL as they come.
#define INTERP_DELAY_MS 28
static struct { double x, y, t; } smp[2];
static int nsmp;

void shell_pointer(int x, int y, bool pressed) {
	if (cal_collecting) { // calibration takes the raw positions, not LVGL
		if (pressed) { cal_sx += x; cal_sy += y; cal_n++; }
		else if (cal_n) { cal_tap(cal_sx / cal_n, cal_sy / cal_n); cal_sx = cal_sy = 0; cal_n = 0; }
		return;
	}
	x = x < 0 ? 0 : x >= SPRITE_W ? SPRITE_W - 1 : x;
	y = y < 0 ? 0 : y >= SPRITE_H ? SPRITE_H - 1 : y;
	double now = now_ms();
	// While dragging, the frames move things, from interpolated positions (shell_frame)
	bool dragging = (drag == DRAG_PAGING || drag == DRAG_SCROLL || drag == DRAG_TILE) && pressed && ptr_pressed;
	if (!dragging) drag_input(x, y, pressed, ptr_pressed);
	if (pressed && ptr_pressed) { // the finger moving: used by the next frames (shell_frame)
		smp[0] = smp[1];
		smp[1].x = x; smp[1].y = y; smp[1].t = now;
		if (nsmp < 2) nsmp++;
		sprite_request_frame();
		return;
	}
	nsmp = 0;
	if (pressed) { smp[1].x = x; smp[1].y = y; smp[1].t = now; nsmp = 1; }
	ptr_x = x;
	ptr_y = y;
	ptr_pressed = pressed;
	lv_indev_read(indev);
	// LVGL reads input only when there is some (event mode), but a long press needs reading
	// while the finger is still, so poll until it lifts.
	if (pressed && !hold_timer) hold_timer = lv_timer_create(hold_read, 30, NULL);
	else if (!pressed && hold_timer) { lv_timer_delete(hold_timer); hold_timer = NULL; }
	sprite_lvgl_kick();
	sprite_request_frame();
}

void shell_frame(void) {
	if (!visible) return;
	if (ptr_pressed && nsmp == 2 && smp[1].t > smp[0].t) {
		double k = (now_ms() - INTERP_DELAY_MS - smp[0].t) / (smp[1].t - smp[0].t);
		if (k < 0) k = 0;
		// Our drags carry on at the finger's last speed past its last report, for at most
		// SPRITE_DEBOUNCE_MS: a lifting finger's release is held that long (it may be a
		// contact bounce), and the strip shouldn't stop dead while it waits.
		bool ours = drag == DRAG_PAGING || drag == DRAG_SCROLL || drag == DRAG_TILE;
		double kmax = ours ? 1 + SPRITE_DEBOUNCE_MS / (smp[1].t - smp[0].t) : 1;
		if (k > kmax) k = kmax;
		ptr_x = (int)(smp[0].x + (smp[1].x - smp[0].x) * k + 0.5);
		ptr_y = (int)(smp[0].y + (smp[1].y - smp[0].y) * k + 0.5);
		if (ours) drag_input(ptr_x, ptr_y, true, true); // the strip, the screen or a tile, smoothly
		else lv_indev_read(indev);
		if (k < kmax) sprite_request_frame(); // still catching up, or carrying on
	}
	lv_refr_now(disp);
}

// Anything LVGL marks for redrawing asks for the next frame.
static void invalidated(lv_event_t *e) {
	if (visible) sprite_request_frame();
}

static void update_dots(void);

// Whatever was open over the shell goes: it's going out of sight (or under an app).
static void shell_tidy(void) {
	ptr_pressed = false;
	lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
	if (toast_timer) { lv_timer_delete(toast_timer); toast_timer = NULL; }
	if (hold_timer) { lv_timer_delete(hold_timer); hold_timer = NULL; }
	opening = NULL;
	tile_drop(false);
	sheet_dismiss();
	dialog_close();
}

// An app in front that keeps the bar (bar=yes): only the bar shows, with the app's name in
// it, and the bar's buttons bring the shell back to the front (see bar_leave_app).
static struct sprite_app *bar_app;
static char page_title[64]; // the title the page in front has, for when the app goes

void shell_set_bar_app(struct sprite_app *app) {
	if (app == bar_app) return;
	bar_app = app;
	if (app) {
		shell_tidy();
		lv_label_set_text(bar_title, app->name);
	} else {
		lv_label_set_text(bar_title, page_title);
	}
	update_dots();
	sprite_lvgl_kick();
}

// A button in the bar, pressed with an app in front below it: the shell first.
static void bar_leave_app(void) { if (bar_app) sprite_go_home(); }

void shell_set_visible(bool v) {
	visible = v;
	if (v) sprite_request_frame();
	if (!v) shell_tidy();
}

/* ---- icons ---- */

// A tile's icon: a Lucide glyph named by the manifest's icon= (the names are in src/icons.txt,
// made into icons_40.c by scripts/make-icons.sh), or else up to two letters from icon=, or
// the name's first letter. buf gets the text; the font to draw it in is returned.
extern const lv_font_t sprite_icons_40, sprite_ui_24;

static const lv_font_t *icon_text(const struct sprite_app *a, char *buf, size_t len) {
	for (size_t i = 0; i < sizeof icon_names / sizeof icon_names[0]; i++)
		if (!strcasecmp(a->icon, icon_names[i].name)) {
			uint32_t c = icon_names[i].cp; // all in the private use area: three bytes of UTF-8
			snprintf(buf, len, "%c%c%c", 0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
			return &sprite_icons_40;
		}
	const char *src = a->icon[0] ? a->icon : a->name;
	snprintf(buf, len, "%.2s", src);
	if (!a->icon[0]) buf[1] = 0; // one initial from the name
	return &lv_font_montserrat_40;
}

/* ---- status bar ---- */

// The clock: 24-hour or 12-hour, with or without seconds (clock= and seconds= in the settings).
// Its label has a fixed width, that of the widest time the format can show, so nothing after it
// in the bar moves as the time changes. It's updated just after each second begins, and only
// redrawn when what it shows changes.
static bool clock_24 = true, clock_secs = false;
static int upd_hour = 3;          // when the OpenFrame updates (of-update status time=); 03:00 until it says
static void upd_hour_set(int h);
static lv_timer_t *clock_timer;

static int text_w(const char *txt, const lv_font_t *f) {
	lv_point_t p;
	lv_text_get_size(&p, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return p.x;
}

static void clock_layout(void) {
	const lv_font_t *f = &lv_font_montserrat_20;
	char widest = '0', sample[32];
	for (char c = '0'; c <= '9'; c++) {
		char a[2] = { c, 0 }, b[2] = { widest, 0 };
		if (text_w(a, f) > text_w(b, f)) widest = c;
	}
	snprintf(sample, sizeof sample, clock_secs ? "%c%c:%c%c:%c%c" : "%c%c:%c%c", widest, widest, widest, widest,
		 widest, widest);
	int w = text_w(sample, f);
	if (!clock_24) w += text_w(" am", f) > text_w(" pm", f) ? text_w(" am", f) : text_w(" pm", f);
	// The date where the widest time would end, so it never moves as the time does; the clock's
	// touch target fits the time as it is, its pressed light centred on it
	lv_obj_align(bar_date, LV_ALIGN_LEFT_MID, CLOCK_PAD + w + 10, 0);
}

static void updating_check(void);

static void update_clock(lv_timer_t *t) {
	updating_check(); // Sprite's own update, about to restart it (once a second, as this is)
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	char s[32];
	strftime(s, sizeof s, clock_24 ? (clock_secs ? "%H:%M:%S" : "%H:%M") : (clock_secs ? "%-I:%M:%S %P" : "%-I:%M %P"), &tm);
	if (strcmp(s, lv_label_get_text(bar_clock))) lv_label_set_text(bar_clock, s);
	strftime(s, sizeof s, "%a %e %b", &tm);
	if (strcmp(s, lv_label_get_text(bar_date))) lv_label_set_text(bar_date, s);
	// next: just after the next second begins
	if (clock_timer) lv_timer_set_period(clock_timer, 1000 - ts.tv_nsec / 1000000 + 5);
}

static void clock_settings_changed(void) {
	clock_layout();
	update_clock(NULL);
}

/* ---- network icons: drawn once, anti-aliased, as alpha masks the text colour fills ---- */

static uint8_t eth_px[NET_ICON * NET_ICON], wifi_px[5][NET_ICON * NET_ICON], lock_px[NET_ICON * NET_ICON];
static uint8_t warn_px[NET_ICON * NET_ICON], restart_px[2][NET_ICON * NET_ICON], speaker_px[3][NET_ICON * NET_ICON];
static uint8_t download_px[NET_ICON * NET_ICON];
static lv_image_dsc_t eth_img, wifi_img[5], lock_img, warn_img, restart_img[2], speaker_img[3], download_img;

static bool in_rect(double x, double y, double x0, double y0, double x1, double y1) {
	return x >= x0 && x < x1 && y >= y0 && y < y1;
}

// Ethernet: a box above two, wired together ("network-wired")
static bool eth_shape(double x, double y, int unused) {
	return in_rect(x, y, 6.5, 2, 13.5, 7.5) || in_rect(x, y, 9.2, 7.5, 10.8, 10) ||
	       in_rect(x, y, 3.5, 9.5, 16.5, 11) || in_rect(x, y, 3.5, 11, 5, 13.5) ||
	       in_rect(x, y, 15, 11, 16.5, 13.5) || in_rect(x, y, 1, 13.5, 8, 18.5) || in_rect(x, y, 12, 13.5, 19, 18.5);
}

// Wi-Fi: a dot and three arcs; returns which part a point is in (1 dot, 2-4 arcs), 0 if none.
// Centred a pixel higher than Ethernet's boxes end, as a round dot looks lower than a flat edge.
static int wifi_part(double x, double y) {
	double dx = x - 10, dy = 15.5 - y, r = sqrt(dx * dx + dy * dy);
	if (r <= 2.1) return 1;
	if (dy <= 0 || fabs(dx) > dy) return 0; // within 45 degrees of straight up
	if (r >= 5.2 && r <= 7.3) return 2;
	if (r >= 9.0 && r <= 11.1) return 3;
	if (r >= 12.8 && r <= 14.9) return 4;
	return 0;
}

// Updates waiting: an arrow down into a tray ("download")
static bool download_shape(double x, double y) {
	if (in_rect(x, y, 9, 2, 11, 11)) return true;                       // the shaft
	if (y >= 7.5 && y <= 13 && fabs(x - 10) <= (13 - y) * 0.9) return true; // its head
	return (in_rect(x, y, 2.5, 13, 4.5, 18) || in_rect(x, y, 15.5, 13, 17.5, 18) || // the tray
		in_rect(x, y, 2.5, 16, 17.5, 18));
}

// The sound (the bar's speaker): a box and its cone (1), then two waves beside it (2, 3)
static int speaker_part(double x, double y) {
	if (in_rect(x, y, 2, 7.5, 6, 12.5)) return 1;
	if (x >= 6 && x <= 10.5 && fabs(y - 10) <= 2.5 + (x - 6) * (4.5 / 4.5)) return 1; // the cone
	double dx = x - 10.5, dy = y - 10, r = sqrt(dx * dx + dy * dy);
	if (dx <= 0 || fabs(dy) > dx * 1.2) return 0; // within about 50 degrees of straight out
	if (r >= 3.4 && r <= 5.2) return 2;
	if (r >= 6.9 && r <= 8.7) return 3;
	return 0;
}

// A padlock (a network that needs a password): a body, and a shackle over it
static bool lock_shape(double x, double y) {
	if (in_rect(x, y, 4, 9.5, 16, 18)) return true;
	double dx = x - 10, dy = y - 9.5, r = sqrt(dx * dx + dy * dy);
	return (dy <= 0 && r >= 3.4 && r <= 5.2) || (dy > 0 && dy < 1 && fabs(dx) >= 3.4 && fabs(dx) <= 5.2);
}

// A warning: a solid triangle with an exclamation mark cut out of it
static bool warn_shape(double x, double y) {
	if (y < 1.5 || y > 18.5 || fabs(x - 10) > (y - 1.5) * (9.5 / 17)) return false;
	return !(in_rect(x, y, 9, 6.5, 11, 12.5) || in_rect(x, y, 9, 14, 11, 16));
}

static bool in_triangle(double x, double y, const double t[6]) {
	double d1 = (x - t[2]) * (t[1] - t[3]) - (t[0] - t[2]) * (y - t[3]);
	double d2 = (x - t[4]) * (t[3] - t[5]) - (t[2] - t[4]) * (y - t[5]);
	double d3 = (x - t[0]) * (t[5] - t[1]) - (t[4] - t[0]) * (y - t[1]);
	return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
}

// Restart: a clockwise ring arrow, its gap at the top right and its head at the top pointing
// right, its bottom in line with the other icons'; bang puts an exclamation mark inside (the
// restart flashes firmware).
static bool restart_shape(double x, double y, bool bang) {
	const double cx = 10, cy = 10.8, r0 = 5.7, r1 = 7.7, gap0 = 25, gap1 = 95;
	static double head[6];
	if (!head[0]) {
		double t = gap1 * M_PI / 180, mid = (r0 + r1) / 2;
		double ex = cx + mid * cos(t), ey = cy - mid * sin(t);
		double dx = sin(t), dy = cos(t), nx = cos(t), ny = -sin(t); // clockwise, and outward
		double l = 3.8, w = 3.3;
		double h[6] = { ex + dx * l, ey + dy * l, ex + nx * w, ey + ny * w, ex - nx * w, ey - ny * w };
		memcpy(head, h, sizeof h);
	}
	double dx = x - cx, dy = cy - y, r = sqrt(dx * dx + dy * dy);
	double a = atan2(dy, dx) * 180 / M_PI;
	if (a < 0) a += 360;
	if (r >= r0 && r <= r1 && !(a > gap0 && a < gap1)) return true;
	if (in_triangle(x, y, head)) return true;
	return bang && x >= 9 && x <= 11 && ((y >= 7.2 && y <= 11.9) || (y >= 13.3 && y <= 15.2));
}

// lit: how many parts are bright (the rest are faint); 0 = all faint (not connected)
static void draw_icon(uint8_t *px, int lit, bool wifi) {
	for (int y = 0; y < NET_ICON; y++)
		for (int x = 0; x < NET_ICON; x++) {
			double a = 0;
			for (int sy = 0; sy < 4; sy++) // 4x4 samples a pixel
				for (int sx = 0; sx < 4; sx++) {
					double fx = x + (sx + 0.5) / 4, fy = y + (sy + 0.5) / 4;
					if (lit == -3 || lit == -4) {
						if (restart_shape(fx, fy, lit == -4)) a += 1.0;
					} else if (lit == -2) {
						if (warn_shape(fx, fy)) a += 1.0;
					} else if (lit < 0) {
						if (lock_shape(fx, fy)) a += 1.0;
					} else if (wifi) {
						int part = wifi_part(fx, fy);
						if (part) a += part <= lit ? 1.0 : 0.3;
					} else if (eth_shape(fx, fy, 0)) {
						a += 1.0;
					}
				}
			px[y * NET_ICON + x] = (uint8_t)(a / 16 * 255 + 0.5);
		}
}

// The speaker, its first waves (0-2) bright and the rest faint (by the volume)
static void draw_speaker(uint8_t *px, int waves) {
	for (int y = 0; y < NET_ICON; y++)
		for (int x = 0; x < NET_ICON; x++) {
			double a = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					int part = speaker_part(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4);
					if (part) a += part == 1 || part - 1 <= waves ? 1.0 : 0.3;
				}
			px[y * NET_ICON + x] = (uint8_t)(a / 16 * 255 + 0.5);
		}
}

static void make_img(lv_image_dsc_t *d, uint8_t *px) {
	memset(d, 0, sizeof *d);
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_A8;
	d->header.w = NET_ICON;
	d->header.h = NET_ICON;
	d->header.stride = NET_ICON;
	d->data_size = NET_ICON * NET_ICON;
	d->data = px;
}

static void make_net_icons(void) {
	draw_icon(eth_px, 0, false);
	make_img(&eth_img, eth_px);
	for (int i = 0; i < 5; i++) { // 0: not connected; 1-4: the dot and up to three arcs lit
		draw_icon(wifi_px[i], i, true);
		make_img(&wifi_img[i], wifi_px[i]);
	}
	draw_icon(lock_px, -1, false);
	make_img(&lock_img, lock_px);
	draw_icon(warn_px, -2, false);
	make_img(&warn_img, warn_px);
	for (int i = 0; i < 2; i++) { // restart; restart flashing firmware
		draw_icon(restart_px[i], -3 - i, false);
		make_img(&restart_img[i], restart_px[i]);
	}
	for (int y = 0; y < NET_ICON; y++) // updates waiting
		for (int x = 0; x < NET_ICON; x++) {
			int a = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) a += download_shape(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4);
			download_px[y * NET_ICON + x] = (uint8_t)(a * 255 / 16);
		}
	make_img(&download_img, download_px);
	for (int i = 0; i < 3; i++) { // the speaker: no waves lit (silent), one, both
		draw_speaker(speaker_px[i], i);
		make_img(&speaker_img[i], speaker_px[i]);
	}
}

// Ethernet shows while a cable is connected, faint until it has an address. Wi-Fi shows unless
// it's switched off or missing: bars by signal, all faint while not connected. An amber
// warning shows left of them while of-netplan has something to say about the settings files,
// and the Network tab in Settings has an amber dot. The icons are one touch target, sized to
// those showing, that opens Settings at the Network tab.
// Each icon right of centre has a space the gear's width (GEAR_W), the icon in its middle, so
// they're all as wide to touch and evenly spaced, centre to centre: the gear, the network icons
// (each of them, the warning too), the speaker, and the update sign.
#define NET_GAP (GEAR_W - NET_ICON)       // between two icons in one touch target
#define NET_PAD ((GEAR_W - NET_ICON) / 2) // and either side of them
static bool wifi_scanning;
static struct wifi_status wifi_now;                       // what the helper last said

/* ---- automatic updates (openframe-scripts' of-update): what it's doing now ---- */

static void set_text(lv_obj_t *l, const char *t);
static void show_if(lv_obj_t *o, bool show);

enum upd_state { UPD_IDLE, UPD_CHECKING, UPD_DOWNLOADING, UPD_INSTALLING };

// What of-update is doing: its state file, if one holds its lock (a state left by a run that
// was killed doesn't count). Read straight from /run: it's the bar's to show, every 5 s.
static enum upd_state update_state(void) {
	int fd = open("/run/of-update.lock", O_RDONLY | O_CLOEXEC);
	if (fd < 0) return UPD_IDLE;
	bool busy = flock(fd, LOCK_SH | LOCK_NB) != 0;
	close(fd);
	if (!busy) return UPD_IDLE;
	char line[64] = "";
	FILE *f = fopen("/run/of-update/state", "r");
	if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
	if (!strncmp(line, "state=installing", 16)) return UPD_INSTALLING;
	if (!strncmp(line, "state=downloading", 17)) return UPD_DOWNLOADING;
	return UPD_CHECKING;
}

// True if the next restart flashes firmware (openframe-firmware said so, in the list of what
// needs the restart).
static bool firmware_at_restart(void) {
	bool flash = false;
	FILE *f = fopen("/run/reboot-required.pkgs", "r");
	if (f) {
		char line[64];
		while (fgets(line, sizeof line, f)) flash |= !strncmp(line, "openframe-firmware", 18);
		fclose(f);
	}
	return flash;
}

// of-update's status between runs (/var/lib/of-update/status, anyone may read it): how many
// updates are waiting, and whether the last run went wrong. Read straight from the file,
// with the bar's other signs, every 5 s.
static void update_waiting(int *pending, bool *failed) {
	*pending = 0;
	*failed = false;
	FILE *f = fopen("/var/lib/of-update/status", "r");
	if (!f) return;
	char line[160];
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "pending=", 8)) *pending = atoi(line + 8);
		else if (!strncmp(line, "result=", 7)) *failed = strncmp(line + 7, "ok", 2) && line[7] != '\n' && line[7];
	}
	fclose(f);
}

static void update_net(lv_timer_t *t) {
	struct sprite_net n;
	sysinfo_net(&n);
	if (n.eth_up) {
		lv_obj_remove_flag(net_eth, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_image_opa(net_eth, n.eth_ip[0] ? LV_OPA_COVER : LV_OPA_40, 0);
	} else {
		lv_obj_add_flag(net_eth, LV_OBJ_FLAG_HIDDEN);
	}
	// (a scan brings the card up for a few seconds: that isn't Wi-Fi being on)
	if (n.wifi_present && n.wifi_enabled && !(wifi_scanning && !wifi_now.saved[0])) {
		int level = 0;
		if (n.wifi_ip[0]) level = n.wifi_dbm < 0 ? wifi_level(n.wifi_dbm) : 1; // as the list does
		lv_image_set_src(net_wifi, &wifi_img[level]);
		lv_obj_remove_flag(net_wifi, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(net_wifi, LV_OBJ_FLAG_HIDDEN);
	}
	if (n.notes) lv_obj_remove_flag(net_warn, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag(net_warn, LV_OBJ_FLAG_HIDDEN);
	if (net_tab_dot) {
		if (n.notes) lv_obj_remove_flag(net_tab_dot, LV_OBJ_FLAG_HIDDEN);
		else lv_obj_add_flag(net_tab_dot, LV_OBJ_FLAG_HIDDEN);
	}
	int shown = 0;
	for (lv_obj_t *o = net_warn; o; o = o == net_warn ? net_wifi : o == net_wifi ? net_eth : NULL)
		shown += !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
	if (shown) {
		lv_obj_set_width(net_box, shown * NET_ICON + (shown - 1) * NET_GAP + 2 * NET_PAD);
		lv_obj_remove_flag(net_box, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(net_box, LV_OBJ_FLAG_HIDDEN);
	}
	// The speaker (bar_aud_update shows it), left of the network icons, or against the gear
	lv_obj_update_layout(net_box);
	lv_obj_t *edge = shown ? net_box : NULL; // what the next thing leftwards goes against
	if (aud_box && !lv_obj_has_flag(aud_box, LV_OBJ_FLAG_HIDDEN)) {
		if (edge) lv_obj_align_to(aud_box, edge, LV_ALIGN_OUT_LEFT_MID, 0, 0);
		else lv_obj_align(aud_box, LV_ALIGN_RIGHT_MID, -GEAR_W - BAR_EDGE, 0);
		lv_obj_update_layout(aud_box);
		edge = aud_box;
	}
	// While updates are checked for, downloaded or installed: a spinner left of the network
	// icons (opening Settings at Updates), so nobody's surprised it's busy, or switches it off
	// without thinking.
	enum upd_state us = update_state();
	bool updating = us != UPD_IDLE;
	// ... and once an update needs a restart, a restart sign there instead, until it's had one
	// (at about 4am, or from Settings' Updates tab, where this goes)
	bool restart = !updating && access("/run/reboot-required", F_OK) == 0;
	// ... and otherwise, while updates are waiting (for the night, or Update now), a download
	// sign, and a dot on Settings' Updates tab: blue, or amber if the last run went wrong
	int pending;
	bool failed;
	update_waiting(&pending, &failed);
	bool waiting = !updating && !restart && pending > 0;
	if (upd_tab_dot) {
		show_if(upd_tab_dot, pending > 0 || failed);
		lv_obj_set_style_bg_color(upd_tab_dot, lv_color_hex(failed ? C_WARN : C_ACCENT), 0);
	}
	if (updating || restart || waiting) {
		lv_obj_t *sign = lv_obj_get_child(upd_box, 0);
		show_if(sign, restart || waiting); // the restart sign ("!" when the restart flashes firmware), or download
		if (restart) lv_image_set_src(sign, &restart_img[firmware_at_restart()]);
		else if (waiting) lv_image_set_src(sign, &download_img);
		show_if(lv_obj_get_child(upd_box, 1), updating); // the spinner
		if (edge) lv_obj_align_to(upd_box, edge, LV_ALIGN_OUT_LEFT_MID, 0, 0);
		else lv_obj_align(upd_box, LV_ALIGN_RIGHT_MID, -GEAR_W - BAR_EDGE, 0);
		lv_obj_remove_flag(upd_box, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(upd_box, LV_OBJ_FLAG_HIDDEN);
	}
}

static void gear_clicked(lv_event_t *e);
static void clock_clicked(lv_event_t *e);
static void dots_clicked(lv_event_t *e);
static void net_clicked(lv_event_t *e);
static void upd_box_clicked(lv_event_t *e);
static void aud_box_clicked(lv_event_t *e);
static void bar_vol_slid(lv_event_t *e);
static void bar_vol_pressed(lv_event_t *e);
static void bar_vol_released(lv_event_t *e);

static void build_bar(lv_obj_t *scr) {
	lv_obj_t *bar = lv_obj_create(scr);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, SPRITE_W, BAR_H);
	lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

	// Everything in the bar is centred on its midline. Left: the time and date, as far in from
	// the left edge as the gear's glyph is from the right.
	int inset = (GEAR_W - text_w(LV_SYMBOL_SETTINGS, &lv_font_montserrat_20)) / 2 + BAR_EDGE;
	// The clock is a touch target the bar's height (Settings at Display), as wide as the time
	// is and CLOCK_PAD more each side for its pressed light; the group is that much further
	// left, so the time sits where it would without it. The date, placed by clock_layout, does
	// nothing.
	lv_obj_t *left = bar_left = lv_obj_create(bar);
	lv_obj_remove_style_all(left);
	lv_obj_set_size(left, LV_SIZE_CONTENT, BAR_H);
	lv_obj_align(left, LV_ALIGN_LEFT_MID, inset - CLOCK_PAD, 0);
	lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *cbox = lv_obj_create(left);
	lv_obj_remove_style_all(cbox);
	lv_obj_set_size(cbox, LV_SIZE_CONTENT, BAR_H);
	lv_obj_set_style_pad_hor(cbox, CLOCK_PAD, 0);
	lv_obj_add_flag(cbox, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(cbox, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(cbox, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(cbox, LV_OPA_20, LV_STATE_PRESSED);
	rounded(cbox);
	lv_obj_add_event_cb(cbox, clock_clicked, LV_EVENT_CLICKED, NULL);
	lv_obj_align(cbox, LV_ALIGN_LEFT_MID, 0, 0);
	bar_clock = lv_label_create(cbox);
	lv_obj_align(bar_clock, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_text_font(bar_clock, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(bar_clock, lv_color_hex(C_TEXT), 0);
	lv_label_set_long_mode(bar_clock, LV_LABEL_LONG_CLIP);
	lv_label_set_text(bar_clock, "");
	bar_date = lv_label_create(left);
	lv_obj_set_style_text_font(bar_date, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(bar_date, lv_color_hex(C_DIM), 0);
	lv_label_set_text(bar_date, "");
	const char *v = settings_get("clock");
	clock_24 = !v || strcmp(v, "12h");
	v = settings_get("seconds");
	clock_secs = v && !strcmp(v, "yes");
	clock_layout();

	bar_title = lv_label_create(bar);
	lv_obj_set_style_text_font(bar_title, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(bar_title, lv_color_hex(C_DIM), 0);
	lv_label_set_text(bar_title, "");
	lv_obj_align(bar_title, LV_ALIGN_CENTER, 0, 0);

	// Centre: the page dots, as one touch target (the bar's height) that turns to the next page
	dots = lv_obj_create(bar);
	lv_obj_remove_style_all(dots);
	lv_obj_set_size(dots, LV_SIZE_CONTENT, BAR_H);
	lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(dots, 14, 0);
	lv_obj_set_style_pad_hor(dots, 20, 0);
	rounded(dots);
	lv_obj_set_style_bg_color(dots, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(dots, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_align(dots, LV_ALIGN_CENTER, 0, 0);
	lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(dots, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(dots, dots_clicked, LV_EVENT_CLICKED, NULL);

	// Right: Settings, a gear against the top, BAR_EDGE in from the right, the bar's height to touch
	gear = lv_obj_create(bar);
	lv_obj_remove_style_all(gear);
	lv_obj_set_size(gear, GEAR_W, BAR_H);
	lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -BAR_EDGE, 0);
	lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(gear, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(gear, LV_OPA_20, LV_STATE_PRESSED);
	rounded(gear);
	lv_obj_t *gl = lv_label_create(gear);
	lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
	lv_obj_set_style_text_font(gl, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(gl, lv_color_hex(C_TEXT), 0);
	lv_obj_center(gl);
	lv_obj_add_event_cb(gear, gear_clicked, LV_EVENT_CLICKED, NULL);

	// ... and to its left, the network icons
	make_net_icons();
	lv_obj_t *net = net_box = lv_obj_create(bar);
	lv_obj_remove_style_all(net);
	// Its width is set by update_net, for the icons showing (sized to its content, it didn't
	// grow when Wi-Fi appeared beside Ethernet, which was pushed out of it and lost). Pressed,
	// it lights as the gear does, which it sits against.
	lv_obj_set_size(net, 2 * NET_ICON + NET_GAP + 2 * NET_PAD, BAR_H);
	lv_obj_set_flex_flow(net, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(net, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(net, NET_GAP, 0);
	lv_obj_set_style_pad_hor(net, NET_PAD, 0);
	lv_obj_align(net, LV_ALIGN_RIGHT_MID, -GEAR_W - BAR_EDGE, 0);
	lv_obj_remove_flag(net, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(net, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(net, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(net, LV_OPA_20, LV_STATE_PRESSED);
	rounded(net);
	lv_obj_add_event_cb(net, net_clicked, LV_EVENT_CLICKED, NULL);
	// The warning, then Wi-Fi, then Ethernet, always: the flex row keeps creation order.
	net_warn = lv_image_create(net);
	lv_image_set_src(net_warn, &warn_img);
	lv_obj_set_style_image_recolor(net_warn, lv_color_hex(C_WARN), 0);
	lv_obj_set_style_image_recolor_opa(net_warn, LV_OPA_COVER, 0);
	lv_obj_add_flag(net_warn, LV_OBJ_FLAG_HIDDEN);
	net_wifi = lv_image_create(net);
	lv_image_set_src(net_wifi, &wifi_img[0]);
	net_eth = lv_image_create(net);
	lv_image_set_src(net_eth, &eth_img);
	for (lv_obj_t *o = net_eth; o; o = o == net_eth ? net_wifi : NULL) {
		lv_obj_set_style_image_recolor(o, lv_color_hex(C_TEXT), 0);
		lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
		lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
	}

	// The update sign, as tall a target as the bar, put in place by update_net: a spinner while
	// updating, a restart arrow when an update needs a restart.
	upd_box = lv_obj_create(bar);
	lv_obj_remove_style_all(upd_box);
	lv_obj_set_size(upd_box, NET_ICON + 2 * NET_PAD, BAR_H);
	lv_obj_add_flag(upd_box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(upd_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(upd_box, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(upd_box, LV_OPA_20, LV_STATE_PRESSED);
	rounded(upd_box);
	lv_obj_add_event_cb(upd_box, upd_box_clicked, LV_EVENT_CLICKED, NULL);
	lv_obj_t *ul = lv_image_create(upd_box);
	lv_image_set_src(ul, &restart_img[0]);
	lv_obj_set_style_image_recolor(ul, lv_color_hex(C_TEXT), 0);
	lv_obj_set_style_image_recolor_opa(ul, LV_OPA_COVER, 0);
	lv_obj_center(ul);
	lv_obj_t *usp = lv_spinner_create(upd_box);
	lv_spinner_set_anim_params(usp, 1000, 270);
	lv_obj_set_size(usp, NET_ICON, NET_ICON);
	lv_obj_center(usp);

	// The speaker, as tall a target as the bar, put in place by update_net and shown by
	// bar_aud_update (once there's a sound card). Its waves light by the volume.
	aud_box = lv_obj_create(bar);
	lv_obj_remove_style_all(aud_box);
	lv_obj_set_size(aud_box, NET_ICON + 2 * NET_PAD, BAR_H);
	lv_obj_add_flag(aud_box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(aud_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(aud_box, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(aud_box, LV_OPA_20, LV_STATE_PRESSED);
	rounded(aud_box);
	lv_obj_add_event_cb(aud_box, aud_box_clicked, LV_EVENT_CLICKED, NULL);
	lv_obj_t *al = lv_image_create(aud_box);
	lv_image_set_src(al, &speaker_img[2]);
	lv_obj_set_style_image_recolor(al, lv_color_hex(C_TEXT), 0);
	lv_obj_set_style_image_recolor_opa(al, LV_OPA_COVER, 0);
	lv_obj_center(al);
	lv_obj_add_flag(aud_box, LV_OBJ_FLAG_HIDDEN);

	// The volume slider the speaker opens (while the OpenFrame sets the volume): in the bar
	// itself, over its title and page dots, so it shows over an app that keeps the bar too
	// (only the bar's strip of the shell shows then). Put in place as it opens (bar_vol_show).
	bar_vol = lv_obj_create(bar);
	lv_obj_remove_style_all(bar_vol);
	lv_obj_set_height(bar_vol, BAR_H);
	lv_obj_set_style_bg_color(bar_vol, lv_color_hex(C_BAR), 0);
	lv_obj_set_style_bg_opa(bar_vol, LV_OPA_COVER, 0);
	lv_obj_remove_flag(bar_vol, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(bar_vol, LV_OBJ_FLAG_CLICKABLE); // (a tap beside the slider goes no further)
	bar_vol_slider = lv_slider_create(bar_vol);
	lv_obj_set_height(bar_vol_slider, BAR_VOL_H); // (its width, as it opens: bar_vol_show)
	lv_slider_set_range(bar_vol_slider, 0, 100);
	lv_obj_set_ext_click_area(bar_vol_slider, 10);
	lv_obj_add_event_cb(bar_vol_slider, bar_vol_slid, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(bar_vol_slider, bar_vol_pressed, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(bar_vol_slider, bar_vol_released, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(bar_vol_slider, bar_vol_released, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(bar_vol_slider, bar_vol_released, LV_EVENT_INDEV_RESET, NULL);
	lv_obj_add_flag(bar_vol, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(usp, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_arc_width(usp, 3, 0);
	lv_obj_set_style_arc_width(usp, 3, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(usp, lv_color_hex(C_TEXT), 0);
	lv_obj_set_style_arc_opa(usp, LV_OPA_30, 0);
	lv_obj_set_style_arc_color(usp, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
	lv_obj_add_flag(upd_box, LV_OBJ_FLAG_HIDDEN);

	update_net(NULL);
	clock_timer = lv_timer_create(update_clock, 1000, NULL);
	update_clock(NULL);
	lv_timer_create(update_net, 5000, NULL);
}

/* ---- pages ---- */

static void update_dots(void);
static void entry_clear(void);

static bool showing(lv_obj_t *page) { return page && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN); }

static void show_page(lv_obj_t *page) {
	if (page != page_entry && showing(page_entry)) entry_clear(); // never leave a password lying about
	lv_obj_t *pages[] = { page_home, page_settings, page_wifi, page_entry, page_netinfo, page_news };
	for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++)
		if (pages[i]) lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
	snprintf(page_title, sizeof page_title, "%s", page == page_settings ? STR_TITLE_SETTINGS : page == page_netinfo ? (netinfo_kind == NETINFO_WIFI ? STR_TITLE_WIFI : STR_TITLE_ETHERNET) :
			  page == page_news ? STR_TITLE_WHATS_NEW :
			  page == page_wifi || page == page_entry ? STR_TITLE_WIFI : "");
	lv_label_set_text(bar_title, page_title);
	update_dots();
}

static void toast_expire(lv_timer_t *t) {
	lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
	toast_timer = NULL;
	opening = NULL;
}

// Banners go by themselves: "Opening..." after 20 s if no window appears, others after 3 s.
static void show_toast(const char *text, uint32_t ms) {
	lv_label_set_text(lv_obj_get_child(toast, 0), text);
	lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(toast); // over a message or the calibration's screen there too
	if (toast_timer) lv_timer_delete(toast_timer);
	toast_timer = lv_timer_create(toast_expire, ms, NULL);
	lv_timer_set_repeat_count(toast_timer, 1);
}

void shell_notice(const char *text) {
	show_toast(text, 6000);
	sprite_lvgl_kick();
}

static void wifi_refresh(void);
static void ssh_refresh(void);
static void name_refresh(void);
static void upd_refresh(void);
static void eth_show(void);
static void netinfo_show(void);
static void net_notes_show(void);
static lv_obj_t *wifi_status_label;

static void saver_show(void); // (Display: which screensaver)

static void refresh_settings(lv_timer_t *t) {
	if (!visible) return;
	if (showing(page_wifi)) { wifi_refresh(); return; }
	if (showing(page_netinfo)) { netinfo_show(); return; }
	if (!showing(page_settings)) return;
	// Wi-Fi's state from the helper (a root process chain each time): every 3 s only while
	// Network shows it, and once as Settings opens
	if (wifi_status_label && (!t || settings_tab == TAB_NETWORK)) wifi_refresh();
	if (!t) ssh_refresh(); // as Settings opens: it changes only when it's set
	if (!t) name_refresh();
	if (settings_tab == TAB_UPDATES) upd_refresh();
	eth_show();
	net_notes_show();
	if (settings_tab == TAB_ABOUT) about_show();
	if (!t && settings_tab == TAB_DISPLAY) saver_show(); // the screensavers installed
	bool autobl = sysinfo_backlight_auto();
	if (autobl) lv_obj_add_state(bl_auto, LV_STATE_CHECKED);
	else lv_obj_remove_state(bl_auto, LV_STATE_CHECKED);
	bl_mode_show(autobl);
	if (!autobl && !lv_obj_has_state(bl_slider, LV_STATE_PRESSED)) {
		// moved only if it's really elsewhere: the hardware has 33 steps, so what's read back
		// can be a point or two from what the slider set, and the knob would hop
		int b = sysinfo_backlight();
		if (b >= 0 && abs(b - (int)lv_slider_get_value(bl_slider)) > 2) lv_slider_set_value(bl_slider, b, LV_ANIM_OFF);
	}
	if (!t && settings_tab == TAB_DISPLAY) bl_limits_ask(); // as Settings opens at Display
}

static void open_app(struct sprite_app *a) {
	if (a->builtin) {
		show_page(page_settings);
		refresh_settings(NULL);
		return;
	}
	if (!a->pid && !a->pending) {
		char s[96];
		snprintf(s, sizeof s, STR_APP_OPENING, a->name);
		show_toast(s, 20000);
		opening = a;
	}
	sprite_open_app(a);
}

static void tile_clicked(lv_event_t *e) { open_app(lv_event_get_user_data(e)); }

// The gear opens Settings at its first tab, and from any of its screens goes home again.
static void gear_clicked(lv_event_t *e) {
	if (bar_app) { // over an app: Settings, as from home
		bar_leave_app();
		settings_show_tab(settings_tab_first);
		show_page(page_settings);
		refresh_settings(NULL);
		return;
	}
	if (!showing(page_home)) {
		show_page(page_home);
		return;
	}
	settings_show_tab(settings_tab_first);
	show_page(page_settings);
	refresh_settings(NULL);
}

// A bar icon's tap, as the gear's: Settings at its tab t, from wherever we are, or home if
// Settings is showing that tab already (a second tap closes it, as the gear's does).
static void bar_icon_tab(int t) {
	if (!bar_app && showing(page_settings) && settings_tab == t) {
		show_page(page_home);
		return;
	}
	bar_leave_app();
	settings_show_tab(t);
	show_page(page_settings);
	refresh_settings(NULL);
}

// The clock: Settings at Display (where its format is set).
static void clock_clicked(lv_event_t *e) { bar_icon_tab(TAB_DISPLAY); }

// The network icons (and the warning): Settings at Network.
static void net_clicked(lv_event_t *e) { bar_icon_tab(TAB_NETWORK); }

// The updating sign: Settings at Updates.
static void upd_box_clicked(lv_event_t *e) { bar_icon_tab(TAB_UPDATES); }

/* ---- long-press sheet: open, close, open at startup ---- */

/* ---- buttons: one shape for every button, in Settings and in dialogs ---- */

// BUTTON_H tall (the smallest touch target), as wide as the label plus BUTTON_PAD each side,
// and no narrower than BUTTON_MIN_W. Main actions are the accent colour, others grey,
// destructive ones red. Pressed, a button darkens (the theme's recolour); it doesn't grow,
// which would push its corners outside its row and cut them off.
#define BUTTON_H 48
#define BUTTON_MIN_W 160
#define BUTTON_PAD 24
#define C_BUTTON_OTHER 0x3a4a5a
#define C_BUTTON_DANGER 0xb3403a
enum button_role { BUTTON_MAIN, BUTTON_OTHER, BUTTON_DANGER };

static lv_obj_t *button(lv_obj_t *parent, const char *text, enum button_role role, lv_event_cb_t cb, void *data) {
	int w = text_w(text, &lv_font_montserrat_20) + 2 * BUTTON_PAD;
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_size(b, w > BUTTON_MIN_W ? w : BUTTON_MIN_W, BUTTON_H);
	lv_color_t c = lv_color_hex(role == BUTTON_MAIN ? C_ACCENT : role == BUTTON_DANGER ? C_BUTTON_DANGER : C_BUTTON_OTHER);
	lv_obj_set_style_bg_color(b, c, 0);
	lv_obj_set_style_bg_color(b, c, LV_STATE_DISABLED);
	lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_set_style_text_color(b, lv_color_hex(C_TEXT), LV_STATE_DISABLED);
	lv_obj_set_style_text_opa(b, LV_OPA_50, LV_STATE_DISABLED);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
	return b;
}

// A row of buttons, BUTTON_GAP apart
#define BUTTON_GAP 20
static lv_obj_t *button_row(lv_obj_t *parent) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, BUTTON_GAP, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	return row;
}

/* ---- dialogs: one kind for everything that asks something (the long-press sheet, the
   power questions, the calibration result) ---- */

// A card in the middle of a dimmed screen: a title, wrapping text, then a row of buttons. The
// card is DIALOG_MIN_W wide, or as wide as its buttons need.
#define DIALOG_MIN_W 460
#define DIALOG_PAD 24
#define C_CARD 0x1c242c
#define TAB_DOT_DROP 1   // a tab's dot, below the middle: it sits by lower-case letters
#define HINT_GAP 6         // between a setting's name and the hint under it
static lv_obj_t *dialog_backdrop;    // the open dialog's own dimmed backdrop, if it made one

static void dialog_close(void) {
	if (!dialog_backdrop) return;
	lv_obj_delete_async(dialog_backdrop);
	dialog_backdrop = NULL;
}

static void dialog_backdrop_clicked(lv_event_t *e) {
	if (lv_event_get_target(e) == lv_event_get_current_target(e)) dialog_close(); // outside the card
}

// on: a screen to put the card on (the calibration screen), or NULL for a dimmed backdrop of
// its own, which a tap outside the card closes if tap_out_closes.
static lv_obj_t *dialog_open(lv_obj_t *on, const char *title, const char *body, bool tap_out_closes) {
	if (!on) {
		dialog_close();
		on = dialog_backdrop = lv_obj_create(lv_layer_top());
		lv_obj_remove_style_all(on);
		lv_obj_set_size(on, SPRITE_W, SPRITE_H);
		lv_obj_set_style_bg_color(on, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(on, LV_OPA_60, 0);
		lv_obj_add_flag(on, LV_OBJ_FLAG_CLICKABLE); // nothing underneath takes a touch meanwhile
		if (tap_out_closes) lv_obj_add_event_cb(on, dialog_backdrop_clicked, LV_EVENT_CLICKED, NULL);
	}
	lv_obj_t *card = lv_obj_create(on);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, DIALOG_MIN_W, LV_SIZE_CONTENT);
	lv_obj_center(card);
	lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(card, DIALOG_PAD, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); // a tap on the card isn't a tap outside it
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	rounded(card);
	lv_obj_t *t = lv_label_create(card);
	lv_label_set_text(t, title);
	lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(t, LV_PCT(100));
	lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
	if (body) {
		lv_obj_t *b = lv_label_create(card);
		lv_label_set_text(b, body);
		lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(b, LV_PCT(100));
		lv_obj_set_style_text_font(b, &lv_font_montserrat_20, 0);
		lv_obj_set_style_text_color(b, lv_color_hex(C_DIM), 0);
		lv_obj_set_style_text_line_space(b, 4, 0);
	}
	return card;
}

// A button on a dialog's card, in its row (made with the first), widening the card to fit.
static lv_obj_t *dialog_button(lv_obj_t *card, const char *text, enum button_role role, lv_event_cb_t cb, void *data) {
	lv_obj_t *row = lv_obj_get_user_data(card);
	if (!row) lv_obj_set_user_data(card, row = button_row(card));
	lv_obj_t *b = button(row, text, role, cb, data);
	int w = 2 * DIALOG_PAD - BUTTON_GAP;
	for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++)
		w += lv_obj_get_style_width(lv_obj_get_child(row, i), 0) + BUTTON_GAP;
	if (w > lv_obj_get_style_width(card, 0)) lv_obj_set_width(card, w);
	return b;
}

static void sheet_dismiss(void) {
	if (!sheet) return;
	dialog_close();
	sheet = NULL;
	sheet_app = NULL;
}

// The sheet's backdrop is going (closed by a tap outside, or by sheet_dismiss)
static void sheet_gone(lv_event_t *e) {
	sheet = NULL;
	sheet_app = NULL;
}

static void sheet_open(lv_event_t *e) {
	struct sprite_app *a = sheet_app;
	sheet_dismiss();
	open_app(a);
}

static void sheet_close_app(lv_event_t *e) {
	struct sprite_app *a = sheet_app;
	sheet_dismiss();
	char s[96];
	snprintf(s, sizeof s, STR_APP_CLOSING, a->name);
	show_toast(s, 4000);
	sprite_close_app(a);
}

static void sheet_startup(lv_event_t *e) {
	apps_set_autostart(sheet_app, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	shell_apps_changed();
}

static void open_sheet(struct sprite_app *a) {
	if (a->builtin || sheet) return;
	sheet_app = a;
	lv_obj_t *card = dialog_open(NULL, a->name, NULL, true);
	sheet = dialog_backdrop;
	lv_obj_add_event_cb(sheet, sheet_gone, LV_EVENT_DELETE, NULL);
	dialog_button(card, LV_SYMBOL_PLAY "  " STR_APP_OPEN, BUTTON_MAIN, sheet_open, NULL);
	if (a->pid || a->pending) dialog_button(card, LV_SYMBOL_CLOSE "  " STR_APP_CLOSE, BUTTON_DANGER, sheet_close_app, NULL);

	lv_obj_t *srow = lv_obj_create(card);
	lv_obj_remove_style_all(srow);
	lv_obj_set_size(srow, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *text = lv_obj_create(srow);
	lv_obj_remove_style_all(text);
	lv_obj_set_size(text, 300, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, HINT_GAP, 0);
	lv_obj_t *l1 = lv_label_create(text);
	lv_label_set_text(l1, STR_APP_OPEN_AT_STARTUP);
	lv_obj_set_style_text_color(l1, lv_color_hex(C_TEXT), 0);
	lv_obj_t *l2 = lv_label_create(text);
	lv_label_set_text(l2, STR_APP_OPEN_AT_STARTUP_NOTE);
	lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l2, 300);
	lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(l2, lv_color_hex(C_DIM), 0);
	lv_obj_t *sw = lv_switch_create(srow);
	lv_obj_set_size(sw, 72, 38);
	if (apps_autostart() == a) lv_obj_add_state(sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, sheet_startup, LV_EVENT_VALUE_CHANGED, NULL);
	sprite_lvgl_kick();
}

static void tile_lift(lv_obj_t *tile, struct sprite_app *a);
static void tile_long_pressed(lv_event_t *e) {
	tile_lift(lv_event_get_current_target(e), lv_event_get_user_data(e));
}

/* ---- the home grid, in pages ---- */

// An app's icon: a rounded square of its colour, with a white glyph.
static lv_obj_t *make_badge(lv_obj_t *cell, const struct sprite_app *a) {
	lv_obj_t *badge = lv_obj_create(cell);
	lv_obj_remove_style_all(badge);
	lv_obj_set_size(badge, ICON_SIZE, ICON_SIZE);
	lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, ICON_TOP);
	lv_obj_set_style_radius(badge, radius_for(ICON_SIZE, ICON_SIZE), 0);
	lv_obj_set_style_bg_color(badge, lv_color_hex(a->color), 0);
	lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
	lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	char buf[8];
	lv_obj_t *icon = lv_label_create(badge);
	const lv_font_t *font = icon_text(a, buf, sizeof buf);
	lv_label_set_text(icon, buf);
	lv_obj_set_style_text_font(icon, font, 0);
	lv_obj_set_style_text_color(icon, lv_color_white(), 0);
	lv_obj_center(icon);
	return badge;
}

static lv_obj_t *make_cell(lv_obj_t *page, struct sprite_app *a) {
	lv_obj_t *tile = lv_obj_create(page);
	lv_obj_remove_style_all(tile);
	lv_obj_set_size(tile, TILE_W, TILE_H);
	lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
	// a short click opens; a long press (which LVGL doesn't follow with SHORT_CLICKED) shows the sheet
	lv_obj_add_event_cb(tile, tile_clicked, LV_EVENT_SHORT_CLICKED, a);
	lv_obj_add_event_cb(tile, tile_long_pressed, LV_EVENT_LONG_PRESSED, a);

	lv_obj_t *badge = make_badge(tile, a);

	if (a->pid || a->pending) { // running dot: green with a window, amber while starting
		lv_obj_t *dot = lv_obj_create(badge);
		lv_obj_remove_style_all(dot);
		lv_obj_set_size(dot, 12, 12);
		lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(dot, lv_color_hex(a->view ? 0x45d483 : C_WARN), 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(dot, lv_color_white(), 0);
		lv_obj_set_style_border_width(dot, 2, 0);
		lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -6, 6);
	}

	// The app that opens at startup (only ever one): an accent ring round its icon, 2 px out
	// from it, inside the cell, so the press highlight takes it in with room to spare.
	if (a == apps_autostart()) {
		lv_obj_set_style_outline_color(badge, lv_color_hex(C_ACCENT), 0);
		lv_obj_set_style_outline_width(badge, STARTUP_RING_W, 0);
		lv_obj_set_style_outline_pad(badge, STARTUP_RING_PAD, 0);
	}

	lv_obj_t *name = lv_label_create(tile);
	lv_label_set_text(name, a->name);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT); // wraps, and ends in ... past two lines
	lv_obj_set_size(name, TILE_W - 4, LABEL_H);
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_line_space(name, 0, 0);
	lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
	lv_obj_align(name, LV_ALIGN_TOP_MID, 0, ICON_TOP + ICON_SIZE + LABEL_GAP);

	// pressed look on the whole cell
	lv_obj_set_style_bg_color(tile, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(tile, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tile, radius_for(TILE_W, TILE_H), 0);
	return tile;
}

static void slide_exec(void *obj, int32_t x) { lv_obj_set_x(obj, x); }

// Slide the strip to a page.
static void go_to_page(int p, bool animate) {
	if (p < 0) p = 0;
	if (p > n_pages - 1) p = n_pages - 1;
	cur_page = p;
	lv_anim_delete(strip, slide_exec);
	int to = -p * SPRITE_W;
	if (animate && lv_obj_get_x(strip) != to) {
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, strip);
		lv_anim_set_exec_cb(&a, slide_exec);
		lv_anim_set_values(&a, lv_obj_get_x(strip), to);
		lv_anim_set_duration(&a, PAGE_SLIDE_MS);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_start(&a);
	} else {
		lv_obj_set_x(strip, to);
	}
	update_dots();
}

// The dots are one touch target: each tap turns to the next page, round to the first again.
static void dots_clicked(lv_event_t *e) {
	if (n_pages > 1) go_to_page((cur_page + 1) % n_pages, true);
}

// Called with each position the shell is given, before LVGL sees it.
static void scroll_exec(void *obj, int32_t y) { lv_obj_scroll_to_y(obj, y, LV_ANIM_OFF); }

static void scroll_glide(lv_obj_t *obj, int from, int to, uint32_t ms) {
	lv_anim_delete(obj, scroll_exec);
	if (from == to) return;
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, scroll_exec);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

// The scrolling screen showing, if any: sprite does its scrolling itself (see drag_input).
static lv_obj_t *scrolling_screen(void) {
	if (showing(page_settings)) return tab_panel[settings_tab];
	if (showing(page_wifi)) return page_wifi;
	if (showing(page_netinfo)) return page_netinfo;
	if (showing(page_news)) return page_news;
	return NULL;
}

// Nothing over the pages on the top layer (a message, a lifted tile...): the toast, which
// lives there too, doesn't count, as touches go through it.
static bool top_layer_free(void) {
	lv_obj_t *top = lv_layer_top();
	for (uint32_t i = 0; i < lv_obj_get_child_count(top); i++) {
		lv_obj_t *o = lv_obj_get_child(top, i);
		if (o != toast && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
	}
	return true;
}

// Called with each position the shell is given, before LVGL sees it: page turning and
// scrolling, the same way everywhere.
static void drag_input(int x, int y, bool pressed, bool was_pressed) {
	if (drag == DRAG_TILE) { // a lifted tile (see tile_lift) follows, then drops
		if (pressed) tile_follow(x, y);
		else tile_drop(true);
		return;
	}
	bool free = !sheet && !cal && top_layer_free();
	if (pressed && !was_pressed) { // a touch begins
		// one page can be flicked too, against the ends: it shows there's paging to be had
		drag_can_page = free && pager && !lv_obj_has_flag(page_home, LV_OBJ_FLAG_HIDDEN);
		drag_scroll = free ? scrolling_screen() : NULL;
		drag = (drag_can_page || drag_scroll) && y >= BAR_H ? DRAG_MAYBE : DRAG_NONE;
		drag_x0 = x;
		drag_y0 = y;
		if (drag_scroll) lv_anim_delete(drag_scroll, scroll_exec); // a touch stops a glide
		return;
	}
	if (drag == DRAG_NONE) return;
	int dx = x - drag_x0, dy = y - drag_y0;
	if (pressed) {
		if (drag == DRAG_MAYBE) {
			if (drag_can_page && abs(dx) >= PAGE_DRAG_START && abs(dx) >= abs(dy)) {
				drag = DRAG_PAGING;
				lv_anim_delete(strip, slide_exec);
			} else if (drag_scroll && abs(dy) >= PAGE_DRAG_START && abs(dy) > abs(dx)) {
				drag = DRAG_SCROLL;
				scroll_y0 = lv_obj_get_scroll_y(drag_scroll);
				scroll_max = scroll_y0 + lv_obj_get_scroll_bottom(drag_scroll);
				if (scroll_max < 0) scroll_max = 0;
			} else if (abs(dx) >= PAGE_DRAG_START) {
				// sideways, and not a page turn: LVGL's (a slider's, say) until the finger lifts.
				// Without this a slider worked back and forth, the finger drifting up or down
				// as it does, became a scroll as it passed where it began, and stopped dead.
				drag = DRAG_NONE;
				return;
			} else {
				return;
			}
			// from here on the drag is ours: no tap, long press or LVGL scrolling
			lv_indev_reset(indev, NULL);
			lv_indev_wait_release(indev);
		}
		if (drag == DRAG_PAGING) {
			// past the first or last page, the strip moves a third as far as the finger
			bool edge = (cur_page == 0 && dx > 0) || (cur_page == n_pages - 1 && dx < 0);
			lv_obj_set_x(strip, -cur_page * SPRITE_W + (edge ? dx / 3 : dx));
		} else {
			int t = scroll_y0 - dy;
			if (t < 0) t /= 3;
			else if (t > scroll_max) t = scroll_max + (t - scroll_max) / 3;
			lv_obj_scroll_to_y(drag_scroll, t, LV_ANIM_OFF);
		}
		return;
	}
	double vx = 0, vy = 0; // px/ms over the last two reports
	if (nsmp == 2 && smp[1].t > smp[0].t) {
		vx = (smp[1].x - smp[0].x) / (smp[1].t - smp[0].t);
		vy = (smp[1].y - smp[0].y) / (smp[1].t - smp[0].t);
	}
	if (drag == DRAG_PAGING) { // released: turn the page, or settle back (from where the strip is)
		int p = cur_page;
		if (dx <= -PAGE_TURN_PX || vx <= -PAGE_FLICK_SPEED) p++;
		else if (dx >= PAGE_TURN_PX || vx >= PAGE_FLICK_SPEED) p--;
		go_to_page(p, true);
	} else if (drag == DRAG_SCROLL) { // released: spring back from past an end, or glide on
		int cur = lv_obj_get_scroll_y(drag_scroll);
		if (cur < 0) scroll_glide(drag_scroll, cur, 0, PAGE_SLIDE_MS);
		else if (cur > scroll_max) scroll_glide(drag_scroll, cur, scroll_max, PAGE_SLIDE_MS);
		else if (fabs(vy) >= SCROLL_GLIDE_MIN_SPEED) {
			int to = cur - (int)(vy * SCROLL_GLIDE_MS);
			if (to < 0) to = 0;
			if (to > scroll_max) to = scroll_max;
			scroll_glide(drag_scroll, cur, to, 2 * SCROLL_GLIDE_MS);
		}
	}
	drag = DRAG_NONE;
}

// Dots in the status bar for the pages, the current one bright; none with only one page.
static void update_dots(void) {
	if (!dots) return;
	lv_obj_clean(dots);
	bool show = n_pages > 1 && !lv_obj_has_flag(page_home, LV_OBJ_FLAG_HIDDEN) && !bar_app;
	if (!show) { lv_obj_add_flag(dots, LV_OBJ_FLAG_HIDDEN); return; }
	lv_obj_remove_flag(dots, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < n_pages; i++) {
		lv_obj_t *d = lv_obj_create(dots);
		lv_obj_remove_style_all(d);
		lv_obj_set_size(d, 8, 8);
		lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(d, lv_color_hex(i == cur_page ? C_TEXT : 0x4a5560), 0);
		lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
		lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
	}
}

/* ---- arranging the home screen ---- */

// Each app has a place: a slot on one of up to MAX_PAGES pages, kept in
// /var/lib/sprite/layout (layout_path) as "<slot> <app id>" lines (slot = page * PER_PAGE + cell).
// Apps the file doesn't place (new ones) take the first free slots. A long press lifts a
// tile: keep holding it still (SHEET_HOLD_MS), or let go where it is, and the sheet opens;
// drag it and it follows the finger, the slot
// under it lit, and dropping it there swaps it with whatever was there. Held at a side
// edge, the pages turn; while a tile is up there's an empty page after the last (up to
// MAX_PAGES), to start a new one.
#define MAX_PAGES 3
#define MAX_SLOTS (MAX_PAGES * PER_PAGE)
#define EDGE_W 40                  // px at either side where a held tile turns the page
#define EDGE_HOLD_MS 600
#define SHEET_HOLD_MS 500          // a lifted tile held still this long opens its sheet
static struct sprite_app *slots[MAX_SLOTS];
static struct { char id[64]; int slot; } placed[MAX_SLOTS];
static int n_placed = -1;          // -1: the file hasn't been read yet

static lv_obj_t *lift_tile, *floater, *marker;
static struct sprite_app *lift_app;
static int lift_from, lift_to, lift_dx, lift_dy, edge_dir;
static bool lift_moved;
static lv_timer_t *sheet_timer;    // SHEET_HOLD_MS after a lift, unless it moves first
static lv_timer_t *edge_timer;

// In sprite's state directory (/var/lib/sprite, StateDirectory= in sprite.service), or, run
// by hand, ~/.local/state/sprite.
static const char *state_path(const char *name) {
	static char path[256];
	const char *state = getenv("STATE_DIRECTORY"), *home = getenv("HOME");
	if (state && state[0]) snprintf(path, sizeof path, "%s/%s", state, name);
	else snprintf(path, sizeof path, "%s/.local/state/sprite/%s", home && home[0] ? home : "/tmp", name);
	return path;
}
static const char *layout_path(void) { return state_path("layout"); }

static void layout_read(void) {
	n_placed = 0;
	FILE *f = fopen(layout_path(), "r");
	if (!f) return;
	char line[128];
	int slot;
	char id[64];
	while (n_placed < MAX_SLOTS && fgets(line, sizeof line, f))
		if (sscanf(line, "%d %63s", &slot, id) == 2 && slot >= 0 && slot < MAX_SLOTS) {
			placed[n_placed].slot = slot;
			snprintf(placed[n_placed].id, sizeof placed[0].id, "%s", id);
			n_placed++;
		}
	fclose(f);
}

// Written to a new file, synced, and renamed over the old, so a power cut leaves one or the other.
static void layout_write(void) {
	char dir[256], tmp[300];
	const char *path = layout_path();
	snprintf(dir, sizeof dir, "%s", path);
	for (char *p = dir + 1; (p = strchr(p, '/')); *p++ = '/') { *p = 0; mkdir(dir, 0755); }
	snprintf(tmp, sizeof tmp, "%s.new", path);
	n_placed = 0;
	FILE *f = fopen(tmp, "w");
	if (!f) { fprintf(stderr, "sprite: can't write %s: %s\n", tmp, strerror(errno)); return; }
	for (int s = 0; s < MAX_SLOTS; s++)
		if (slots[s]) {
			fprintf(f, "%d %s\n", s, slots[s]->id);
			placed[n_placed].slot = s;
			snprintf(placed[n_placed].id, sizeof placed[0].id, "%s", slots[s]->id);
			n_placed++;
		}
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	if (rename(tmp, path) != 0) fprintf(stderr, "sprite: can't replace %s: %s\n", path, strerror(errno));
}

static bool on_grid(const struct sprite_app *a) { return !a->hidden && !a->builtin && !a->saver; } // Settings is the gear

// Fills slots[] from the layout, then the rest in name order; returns the number of pages used.
static int layout_assign(void) {
	if (n_placed < 0) layout_read();
	memset(slots, 0, sizeof slots);
	for (int i = 0; i < n_placed; i++) {
		if (slots[placed[i].slot]) continue;
		for (int k = 0; k < apps_count(); k++) {
			struct sprite_app *a = apps_get(k);
			if (!on_grid(a) || strcmp(a->id, placed[i].id)) continue;
			bool already = false;
			for (int s = 0; s < MAX_SLOTS; s++) already |= slots[s] == a;
			if (!already) slots[placed[i].slot] = a;
			break;
		}
	}
	for (int k = 0; k < apps_count(); k++) {
		struct sprite_app *a = apps_get(k);
		bool already = false;
		for (int s = 0; s < MAX_SLOTS; s++) already |= slots[s] == a;
		if (!on_grid(a) || already) continue;
		int s = 0;
		while (s < MAX_SLOTS && slots[s]) s++;
		if (s == MAX_SLOTS) { fprintf(stderr, "sprite: no room on the home screen for %s\n", a->name); continue; }
		slots[s] = a;
	}
	int last = 0;
	for (int s = 0; s < MAX_SLOTS; s++)
		if (slots[s]) last = s / PER_PAGE;
	return last + 1;
}

static void cell_pos(int slot, int *x, int *y) {
	int c = slot % PER_PAGE;
	*x = GRID_PAD_X + (c % GRID_COLS) * TILE_W;
	*y = GRID_PAD_TOP + (c / GRID_COLS) * TILE_H;
}

static lv_obj_t *new_page(int p) {
	lv_obj_t *page = lv_obj_create(strip);
	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_set_pos(page, p * SPRITE_W, 0);
	lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
	return page;
}

// The slot under a point on the screen, on the page showing.
static int slot_at(int x, int y) {
	int col = (x - GRID_PAD_X) / TILE_W, row = (y - BAR_H - GRID_PAD_TOP) / TILE_H;
	col = col < 0 ? 0 : col >= GRID_COLS ? GRID_COLS - 1 : col;
	row = row < 0 ? 0 : row >= GRID_ROWS ? GRID_ROWS - 1 : row;
	return cur_page * PER_PAGE + row * GRID_COLS + col;
}

static void mark_slot(int slot) {
	lift_to = slot;
	lv_obj_t *page = lv_obj_get_child(strip, slot / PER_PAGE);
	if (!page) return;
	if (lv_obj_get_parent(marker) != page) lv_obj_set_parent(marker, page);
	int x, y;
	cell_pos(slot, &x, &y);
	lv_obj_set_pos(marker, x, y);
}

static void edge_turn(lv_timer_t *t) {
	int p = cur_page + edge_dir;
	if (drag != DRAG_TILE || !edge_dir || p < 0 || p >= n_pages) return;
	go_to_page(p, true);
	mark_slot(slot_at(ptr_x, ptr_y)); // the cell at the edge the finger's on, on the new page
}

static void tile_drop(bool place);

// Held still since the lift: the sheet, with the finger still down (lifting it then does
// nothing: the touch is already LVGL's no more, and the drag is over).
static void tile_still(lv_timer_t *t) {
	sheet_timer = NULL;
	if (drag == DRAG_TILE && !lift_moved) tile_drop(true);
}

static void tile_lift(lv_obj_t *tile, struct sprite_app *a) {
	if (drag == DRAG_TILE || sheet) return;
	lift_from = -1;
	for (int s = 0; s < MAX_SLOTS; s++)
		if (slots[s] == a) lift_from = s;
	if (lift_from < 0) { open_sheet(a); return; }
	lift_app = a;
	lift_tile = tile;
	lift_moved = false;
	drag = DRAG_TILE;
	drag_x0 = ptr_x;
	drag_y0 = ptr_y;
	// a copy of the icon, over everything, to carry about; the tile itself fades
	lv_area_t c;
	lv_obj_get_coords(tile, &c);
	floater = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(floater);
	lv_obj_set_size(floater, TILE_W, TILE_H);
	lv_obj_set_pos(floater, c.x1, c.y1);
	lv_obj_remove_flag(floater, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	make_badge(floater, a);
	lift_dx = ptr_x - c.x1;
	lift_dy = ptr_y - c.y1;
	lv_obj_set_style_opa(tile, LV_OPA_30, 0);
	// where it would land: lit like a pressed cell
	marker = lv_obj_create(lv_obj_get_parent(tile));
	lv_obj_remove_style_all(marker);
	lv_obj_set_size(marker, TILE_W, TILE_H);
	lv_obj_set_style_bg_color(marker, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(marker, LV_OPA_20, 0);
	lv_obj_set_style_radius(marker, radius_for(TILE_W, TILE_H), 0);
	lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	mark_slot(lift_from);
	// one empty page after the last, to start a new one
	if (n_pages < MAX_PAGES) {
		new_page(n_pages++);
		lv_obj_set_width(strip, n_pages * SPRITE_W);
	}
	sheet_timer = lv_timer_create(tile_still, SHEET_HOLD_MS, NULL);
	lv_timer_set_repeat_count(sheet_timer, 1);
	lv_obj_remove_state(tile, LV_STATE_PRESSED);
	lv_indev_reset(indev, NULL); // the drag is ours now: nothing more for LVGL from this touch
	lv_indev_wait_release(indev);
	sprite_lvgl_kick();
}

// Ends a lift: with place, the tile goes where it's marked (or, if it never moved, the sheet
// opens); without, it goes back.
static void tile_drop(bool place) {
	if (drag != DRAG_TILE) return;
	drag = DRAG_NONE;
	if (edge_timer) { lv_timer_delete(edge_timer); edge_timer = NULL; }
	if (sheet_timer) { lv_timer_delete(sheet_timer); sheet_timer = NULL; }
	lv_obj_delete(floater);
	floater = marker = NULL; // the marker goes with the grid, rebuilt below
	struct sprite_app *a = lift_app;
	if (place && !lift_moved) {
		lv_obj_set_style_opa(lift_tile, LV_OPA_COVER, 0);
		shell_apps_changed(); // takes the empty page away again
		open_sheet(a);
		return;
	}
	if (place && lift_to != lift_from) {
		slots[lift_from] = slots[lift_to];
		slots[lift_to] = a;
		layout_write();
	}
	shell_apps_changed();
}

static void tile_follow(int x, int y) {
	if (!lift_moved && abs(x - drag_x0) < PAGE_DRAG_START && abs(y - drag_y0) < PAGE_DRAG_START) return;
	lift_moved = true;
	if (sheet_timer) { lv_timer_delete(sheet_timer); sheet_timer = NULL; } // a drag, not the sheet
	lv_obj_set_pos(floater, x - lift_dx, y - lift_dy);
	int dir = x < EDGE_W ? -1 : x >= SPRITE_W - EDGE_W ? 1 : 0;
	if (dir != edge_dir || !dir) {
		edge_dir = dir;
		if (edge_timer) { lv_timer_delete(edge_timer); edge_timer = NULL; }
		if (dir) edge_timer = lv_timer_create(edge_turn, EDGE_HOLD_MS, NULL);
	}
	if (!dir) {
		int s = slot_at(x, y);
		if (s != lift_to) mark_slot(s);
	}
}

static void rebuild_grid(void *unused) {
	rebuild_pending = false;
	if (drag == DRAG_TILE) return; // the lifted tile is still in it: the drop rebuilds
	lv_obj_clean(strip);
	n_pages = layout_assign();
	lv_obj_set_width(strip, n_pages * SPRITE_W);
	for (int p = 0; p < n_pages; p++) {
		lv_obj_t *page = new_page(p);
		for (int c = 0; c < PER_PAGE; c++) {
			struct sprite_app *a = slots[p * PER_PAGE + c];
			if (!a) continue;
			lv_obj_t *tile = make_cell(page, a);
			int x, y;
			cell_pos(p * PER_PAGE + c, &x, &y);
			lv_obj_set_pos(tile, x, y);
		}
	}
	go_to_page(cur_page, false);
	if (opening && opening->pid == 0 && !opening->pending) { // it exited before showing a window
		char s[96];
		snprintf(s, sizeof s, STR_APP_CLOSED, opening->name);
		show_toast(s, 3000);
		opening = NULL;
	}
}

void shell_apps_changed(void) {
	// Deferred: this can be called from inside a tile's own click handler.
	if (rebuild_pending || !strip) return;
	rebuild_pending = true;
	lv_async_call(rebuild_grid, NULL);
	sprite_lvgl_kick();
}

static void build_home(lv_obj_t *scr) {
	page_home = lv_obj_create(scr);
	lv_obj_remove_style_all(page_home);
	lv_obj_set_size(page_home, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_set_pos(page_home, 0, BAR_H);

	// The pages of the grid, side by side on a strip that the pager moves (see drag_input)
	pager = lv_obj_create(page_home);
	lv_obj_remove_style_all(pager);
	lv_obj_set_size(pager, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_remove_flag(pager, LV_OBJ_FLAG_SCROLLABLE);
	strip = lv_obj_create(pager);
	lv_obj_remove_style_all(strip);
	lv_obj_set_size(strip, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

	// On the top layer, over every page: made here on the screen, the pages made after it
	// (Settings, Wi-Fi, What's new...) were drawn over it, their text through it. Touches go
	// through it to what's underneath.
	toast = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(toast);
	lv_obj_remove_flag(toast, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(toast, lv_color_hex(0x26313c), 0);
	lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
	rounded(toast);
	lv_obj_set_style_pad_hor(toast, 22, 0);
	lv_obj_set_style_pad_ver(toast, 10, 0);
	lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -16);
	lv_obj_t *tl = lv_label_create(toast);
	lv_obj_set_style_text_font(tl, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT), 0);
	lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

/* ---- settings ---- */

static void back_clicked(lv_event_t *e) { show_page(page_home); }

/* Brightness: one slider. Fixed, it's the level; automatic, it's a range with a knob at each
   end, the lowest and highest automatic brightness goes to (of-backlight's min and max, in
   backlight.txt), with a switch for whether the screen goes off in the dark (off_below) or
   stays at its lowest. */

static int bl_min = 0, bl_max = 100, bl_off_below = 1; // of-backlight settings; its defaults till it says

static void bl_mode_show(bool autobl) {
	bool range = lv_slider_get_mode(bl_slider) == LV_SLIDER_MODE_RANGE;
	if (autobl && !range) {
		lv_slider_set_mode(bl_slider, LV_SLIDER_MODE_RANGE);
		lv_slider_set_range(bl_slider, 0, 100);
		lv_slider_set_value(bl_slider, bl_max, LV_ANIM_OFF); // the right first: the left can't pass it
		lv_slider_set_start_value(bl_slider, bl_min, LV_ANIM_OFF);
	} else if (!autobl && range) {
		lv_slider_set_range(bl_slider, 1, 100);
		lv_slider_set_mode(bl_slider, LV_SLIDER_MODE_NORMAL);
		int b = sysinfo_backlight();
		lv_slider_set_value(bl_slider, b >= 1 ? b : 1, LV_ANIM_OFF);
	}
	show_if(bl_hint, autobl);
	show_if(bl_off_row, autobl);
}

static void bl_limits_done(char *reply, void *data) {
	if (strcmp(helper_reply_status(reply), "ok")) return; // (an older system: its defaults)
	char *save = NULL;
	for (char *l = strtok_r(reply, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(l, "min=", 4)) bl_min = atoi(l + 4);
		else if (!strncmp(l, "max=", 4)) bl_max = atoi(l + 4);
		else if (!strncmp(l, "off_below=", 10)) bl_off_below = atoi(l + 10);
	}
	if (lv_slider_get_mode(bl_slider) == LV_SLIDER_MODE_RANGE && !lv_obj_has_state(bl_slider, LV_STATE_PRESSED)) {
		lv_slider_set_value(bl_slider, bl_max, LV_ANIM_OFF);
		lv_slider_set_start_value(bl_slider, bl_min, LV_ANIM_OFF);
	}
	lv_obj_t *sw = lv_obj_get_child(bl_off_row, 1);
	if (bl_off_below > 0) lv_obj_add_state(sw, LV_STATE_CHECKED);
	else lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

static void bl_limits_ask(void) { helper_request("backlight-settings\n", bl_limits_done, NULL); }

static void bl_limits_saved(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (strcmp(st, "ok")) {
		lv_obj_t *card = dialog_open(NULL, STR_DISPLAY_NOT_CHANGED, reason_text(st), true);
		dialog_button(card, STR_COMMON_OK, BUTTON_MAIN, dialog_cancel, NULL);
		sysinfo_backlight_request("reload"); // the limits in the file, not the ones it was sent
	}
	bl_limits_ask(); // what's in the file now
}

static void bl_limits_save(void) {
	char req[48];
	snprintf(req, sizeof req, "backlight-set %d %d %d\n", bl_min, bl_max, bl_off_below);
	helper_request(req, bl_limits_saved, NULL);
}

// The level the backlight has now, by the heading, read four times a second while the
// Display tab's in view: it follows the slider as it's dragged, and automatic brightness.
static void bl_now_tick(lv_timer_t *t) {
	if (!bl_now || !visible || !showing(page_settings) || settings_tab != TAB_DISPLAY) return;
	int b = sysinfo_backlight();
	char s[16] = "";
	if (b == 0) snprintf(s, sizeof s, STR_COMMON_OFF);
	else if (b > 0) snprintf(s, sizeof s, STR_COMMON_NOW_PERCENT, b);
	set_text(bl_now, s);
}

// While the slider's held, the backlight shows the level under the finger: at once as a knob
// is grabbed, following it as it's dragged (of-backlight show, no fade), and fading back to
// what it should be as it's let go (the limits saved, or the level set).
//
// Sent only as the hardware's level (one of its few steps) changes, and at most every
// BL_SEND_MS, always ending with the latest: the slider moves at every frame while it's
// dragged, and of-backlight's service, in bash, takes a few milliseconds over each, on a CPU
// Sprite is busy drawing with. Sent at every change, they queued faster than it could take
// them, the light lagging further behind for as long as the slider was held. (The service
// also skips any it's fallen behind on, showing only the latest.) Ten a second is plenty to
// see the brightness by.
#define BL_SEND_MS 100
static int bl_shown = -1, bl_want = -1;  // the hardware level last sent; the level last asked for
static uint32_t bl_sent_at;
static lv_timer_t *bl_send_timer;        // the latest, when it came too soon after the last

static void bl_send(void) {
	int v = bl_want < 1 ? 1 : bl_want;   // (of-backlight shows nothing lower)
	int max = sysinfo_backlight_max(), step = max > 0 ? (v * max + 50) / 100 : v;
	if (step == bl_shown) return;
	bl_shown = step;
	bl_sent_at = lv_tick_get();
	char req[16];
	snprintf(req, sizeof req, "show %d", v);
	sysinfo_backlight_request(req);
}

static void bl_send_later(lv_timer_t *t) {
	bl_send_timer = NULL; // (a one-shot: LVGL deletes it)
	bl_send();
}

static void bl_send_cancel(void) {
	if (bl_send_timer) lv_timer_delete(bl_send_timer);
	bl_send_timer = NULL;
}

static lv_timer_t *bl_settle_timer, *bl_save_timer; // (see BL_SETTLE_MS, below)
static void bl_settle_cancel(void) {
	if (bl_settle_timer) lv_timer_delete(bl_settle_timer);
	if (bl_save_timer) lv_timer_delete(bl_save_timer);
	bl_settle_timer = bl_save_timer = NULL;
}

static void bl_show(int v) {
	bl_want = v;
	uint32_t since = lv_tick_elaps(bl_sent_at);
	if (bl_shown < 0 || since >= BL_SEND_MS) {
		bl_send_cancel();
		bl_send();
	} else if (!bl_send_timer) {
		bl_send_timer = lv_timer_create(bl_send_later, BL_SEND_MS - since, NULL);
		lv_timer_set_repeat_count(bl_send_timer, 1);
	}
}

// The knob a press has: LVGL only picks one once the finger moves, so its rule is used here
// (beyond either knob, that one; between them, the nearer).
static void bl_pressed(lv_event_t *e) {
	lv_slider_t *s = (lv_slider_t *)bl_slider;
	bl_settle_cancel(); // touched again before it settled: still being worked
	bl_send_cancel();
	bl_shown = -1;
	if (lv_slider_get_mode(bl_slider) != LV_SLIDER_MODE_RANGE) { bl_show(lv_slider_get_value(bl_slider)); return; }
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_transform_point(bl_slider, &p, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE_RECURSIVE);
	int32_t lc = (s->left_knob_area.x1 + s->left_knob_area.x2) / 2, rc = (s->right_knob_area.x1 + s->right_knob_area.x2) / 2;
	bool right = p.x > s->right_knob_area.x2 || (p.x >= s->left_knob_area.x1 && LV_ABS(rc - p.x) < LV_ABS(lc - p.x));
	bl_show(right ? lv_slider_get_value(bl_slider) : lv_slider_get_left_value(bl_slider));
}

static void bl_dragged(lv_event_t *e) {
	if (!lv_obj_has_state(bl_slider, LV_STATE_PRESSED)) return; // set from here, not by a finger
	lv_slider_t *s = (lv_slider_t *)bl_slider;
	bool left = lv_slider_get_mode(bl_slider) == LV_SLIDER_MODE_RANGE && s->value_to_set == &s->bar.start_value;
	bl_show(left ? lv_slider_get_left_value(bl_slider) : lv_slider_get_value(bl_slider));
}

// Letting go settles BL_SETTLE_MS later (the fade back, or the level set), and automatic
// brightness's limits are written to OF-BOOT once the slider's been left BL_SAVE_MS; a touch
// again before then cancels both. The touchscreen loses contact now and then as a finger
// slides, and a finger working a slider lifts between strokes: each of those is a release
// here. Settled and saved at every one, the screen dipped between strokes, and the saves (a
// root process and a FAT write each) piled up faster than they finished, slowing everything
// for as long as the slider was worked.
#define BL_SETTLE_MS 600
#define BL_SAVE_MS 2000

static void timer_cancel(lv_timer_t **t) {
	if (*t) lv_timer_delete(*t);
	*t = NULL;
}

static lv_timer_t *timer_once(lv_timer_cb_t cb, uint32_t ms) {
	lv_timer_t *t = lv_timer_create(cb, ms, NULL);
	lv_timer_set_repeat_count(t, 1); // (LVGL deletes it once it's run)
	return t;
}

static void bl_save_now(lv_timer_t *t) {
	bl_save_timer = NULL;
	bl_limits_save();
}

static void bl_settle(lv_timer_t *t) {
	bl_settle_timer = NULL;
	bl_send_cancel(); // nothing shown after what follows
	bl_shown = -1;
	if (lv_slider_get_mode(bl_slider) == LV_SLIDER_MODE_RANGE) {
		bl_min = (int)lv_slider_get_left_value(bl_slider);
		bl_max = (int)lv_slider_get_value(bl_slider);
		// the fade back, by the new limits, now; OF-BOOT once it's been left a while longer
		char req[24];
		snprintf(req, sizeof req, "resume %d %d", bl_min, bl_max);
		sysinfo_backlight_request(req);
		timer_cancel(&bl_save_timer);
		bl_save_timer = timer_once(bl_save_now, BL_SAVE_MS - BL_SETTLE_MS);
		return;
	}
	char v[8];
	snprintf(v, sizeof v, "%d", (int)lv_slider_get_value(bl_slider));
	sysinfo_set_backlight(v);
	lv_obj_remove_state(bl_auto, LV_STATE_CHECKED);
}

static void bl_released(lv_event_t *e) {
	if (!e) { bl_settle(NULL); return; } // (from the Auto switch: at once)
	timer_cancel(&bl_settle_timer);
	bl_settle_timer = timer_once(bl_settle, BL_SETTLE_MS);
}

static void bl_auto_changed(lv_event_t *e) {
	bool on = lv_obj_has_state(bl_auto, LV_STATE_CHECKED);
	if (on) {
		sysinfo_set_backlight("auto");
		bl_limits_ask();
	}
	bl_mode_show(on);
	if (!on) bl_released(NULL);
}

static void bl_off_changed(lv_event_t *e) {
	// on: off below 1 (or whatever backlight.txt had above 0); off: never off, at its lowest
	static int had = 1;
	if (bl_off_below > 0) had = bl_off_below;
	bl_off_below = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? had : 0;
	bl_limits_save();
}

static void confirm_cb(lv_event_t *e) {
	dialog_close();
	sysinfo_helper(lv_event_get_user_data(e)); // "reboot" or "poweroff"
}

static void dialog_cancel(lv_event_t *e) { dialog_close(); }

// A helper's "error: reason" as a sentence to show on its own: a capital, and a full stop.
static const char *reason_text(const char *st) {
	static char buf[256];
	const char *r = helper_reason(st);
	snprintf(buf, sizeof buf, "%s", r);
	size_t n = strlen(buf);
	if (n) buf[0] = (char)toupper((unsigned char)buf[0]);
	if (n && n < sizeof buf - 1 && !strchr(".!?", buf[n - 1])) { buf[n] = '.'; buf[n + 1] = 0; }
	return buf;
}

/* ---- erasing it for its next owner (sprite-helper factory-reset, of-reset) ---- */

static void reset_started(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (!strcmp(st, "ok")) return; // it restarts when it's done; the dialog stays till then
	dialog_close();
	lv_obj_t *card = dialog_open(NULL, STR_ERASE_FAILED, reason_text(st), true);
	dialog_button(card, STR_COMMON_OK, BUTTON_MAIN, dialog_cancel, NULL);
}

static void reset_go(lv_event_t *e) {
	dialog_close();
	// nothing to press, and no tapping it away: the OpenFrame restarts when it's done
	dialog_open(NULL, STR_ERASE_ERASING, STR_ERASE_ERASING_NOTE, false);
	helper_request("factory-reset\n", reset_started, NULL);
}

static void reset_sure(lv_event_t *e) {
	dialog_close();
	lv_obj_t *card = dialog_open(NULL, STR_ERASE_SURE, STR_ERASE_SURE_NOTE, true);
	dialog_button(card, LV_SYMBOL_TRASH "  " STR_ERASE_BUTTON, BUTTON_DANGER, reset_go, NULL);
	dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
}

static void reset_ask(lv_event_t *e) {
	lv_obj_t *card = dialog_open(NULL, STR_ERASE_ASK,
				     STR_ERASE_ASK_NOTE, true);
	dialog_button(card, STR_ERASE_ASK_BUTTON, BUTTON_DANGER, reset_sure, NULL);
	dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
}

static void after_update_cb(lv_event_t *e) {
	const char *what = lv_event_get_user_data(e);
	dialog_close();
	char req[48];
	snprintf(req, sizeof req, "after-update %s", what);
	sysinfo_helper(req);
	show_toast(!strcmp(what, "reboot") ? STR_POWER_WILL_RESTART
					   : STR_POWER_WILL_OFF, 4000);
}

static void power_clicked(lv_event_t *e) {
	bool reboot = !strcmp(lv_event_get_user_data(e), "reboot");
	enum upd_state us = update_state();
	if (us == UPD_DOWNLOADING || us == UPD_INSTALLING) {
		// Not halfway through installing something: once it's done.
		lv_obj_t *card = dialog_open(NULL, STR_POWER_UPDATING,
					     reboot ? STR_POWER_UPDATING_RESTART
						    : STR_POWER_UPDATING_OFF, true);
		dialog_button(card, reboot ? LV_SYMBOL_REFRESH "  " STR_POWER_RESTART_WHEN_DONE : LV_SYMBOL_POWER "  " STR_POWER_OFF_WHEN_DONE,
			      reboot ? BUTTON_MAIN : BUTTON_DANGER, after_update_cb, reboot ? "reboot" : "poweroff");
		dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
		return;
	}
	// firmware to flash at this restart: say so, so nobody's alarmed
	bool flash = firmware_at_restart();
	lv_obj_t *card = dialog_open(NULL, reboot ? STR_POWER_RESTART_ASK : STR_POWER_OFF_ASK,
				     reboot ? (flash ? STR_POWER_RESTART_ASK_FIRMWARE
						     : STR_POWER_RESTART_ASK_NOTE)
					    : flash ? STR_POWER_OFF_ASK_FIRMWARE
						    : STR_POWER_OFF_ASK_NOTE, true);
	dialog_button(card, reboot ? LV_SYMBOL_REFRESH "  " STR_POWER_RESTART : LV_SYMBOL_POWER "  " STR_POWER_OFF,
		      reboot ? BUTTON_MAIN : BUTTON_DANGER, confirm_cb, reboot ? "reboot" : "poweroff");
	dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
}

// A list screen (Settings) is a column of headings and rows, ROW_GAP apart. Every row of
// controls is ROW_H tall (a button fills it; a switch or slider sits in its middle), so the
// space above a heading is the same whatever came before it: ROW_GAP plus HEADING_SPACE.
#define ROW_GAP 12
#define ROW_H 48
#define HEADING_SPACE 16
#define LIST_ROW_GAP 4      // a list of choices (networks): rows closer together, each pressable
#define LIST_ROW_PAD 12     // ... with their content inset, inside the press highlight

static lv_obj_t *section(lv_obj_t *parent, const char *title) {
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_set_style_margin_top(l, HEADING_SPACE, 0);
	lv_label_set_text(l, title);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
	return l;
}

/* ---- touch calibration ---- */

static void apply(const double m[6], double x, double y, double *ox, double *oy) {
	*ox = m[0] * x + m[1] * y + m[2];
	*oy = m[3] * x + m[4] * y + m[5];
}

// Least squares for x' = a x + b y + c (and the same for y'), by the normal equations.
static bool cal_fit(double m[6]) {
	double S[3][3] = { { 0 } }, bx[3] = { 0 }, by[3] = { 0 };
	for (int i = 0; i < CAL_N; i++) {
		double v[3] = { cal_raw[i][0], cal_raw[i][1], 1 };
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) S[r][c] += v[r] * v[c];
			bx[r] += v[r] * cal_pts[i][0];
			by[r] += v[r] * cal_pts[i][1];
		}
	}
	double det = S[0][0] * (S[1][1] * S[2][2] - S[1][2] * S[2][1]) - S[0][1] * (S[1][0] * S[2][2] - S[1][2] * S[2][0]) +
		     S[0][2] * (S[1][0] * S[2][1] - S[1][1] * S[2][0]);
	if (fabs(det) < 1e-9) return false;
	for (int k = 0; k < 2; k++) { // Cramer's rule, once for x' and once for y'
		double *b = k ? by : bx;
		for (int col = 0; col < 3; col++) {
			double T[3][3];
			memcpy(T, S, sizeof T);
			for (int r = 0; r < 3; r++) T[r][col] = b[r];
			double d = T[0][0] * (T[1][1] * T[2][2] - T[1][2] * T[2][1]) - T[0][1] * (T[1][0] * T[2][2] - T[1][2] * T[2][0]) +
				   T[0][2] * (T[1][0] * T[2][1] - T[1][1] * T[2][0]);
			m[k * 3 + col] = d / det;
		}
	}
	return true;
}

static void cal_errors(const double m[6], double *mean, double *worst) {
	*mean = *worst = 0;
	for (int i = 0; i < CAL_N; i++) {
		double x, y;
		apply(m, cal_raw[i][0], cal_raw[i][1], &x, &y);
		double e = hypot(x - cal_pts[i][0], y - cal_pts[i][1]);
		*mean += e / CAL_N;
		if (e > *worst) *worst = e;
	}
}

static void cal_close(void) {
	cal_collecting = false;
	sprite_calibrating(false);
	if (cal) { lv_obj_delete_async(cal); cal = NULL; }
}

static void cal_show_cross(void);
static void cal_start(lv_event_t *e);

static void cal_cancel(lv_event_t *e) {
	sprite_set_calibration(cal_prev);
	cal_close();
}

static void cal_save(lv_event_t *e) {
	char v[160];
	snprintf(v, sizeof v, "%.6f,%.6f,%.4f,%.6f,%.6f,%.4f", cal_new[0], cal_new[1], cal_new[2], cal_new[3], cal_new[4], cal_new[5]);
	settings_set("calibration", v);
	cal_close();
	show_toast(STR_DISPLAY_CALIBRATE_SAVED, 3000);
}

static void cal_result(void) {
	cal_collecting = false;
	sprite_calibrating(false);
	lv_obj_clean(cal);
	double before, before_worst, after, after_worst;
	cal_errors(cal_prev, &before, &before_worst);
	if (!cal_fit(cal_new)) after_worst = 1e9;
	else cal_errors(cal_new, &after, &after_worst);
	if (after_worst > 25) { // one tap well away from its cross, most likely
		lv_obj_t *card = dialog_open(cal, STR_DISPLAY_CALIBRATE_BAD,
					     STR_DISPLAY_CALIBRATE_BAD_NOTE, false);
		dialog_button(card, LV_SYMBOL_REFRESH "  " STR_DISPLAY_CALIBRATE_AGAIN, BUTTON_MAIN, cal_start, NULL);
		dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, cal_cancel, NULL);
		return;
	}
	// Try the new calibration straight away, so the buttons below are tapped with it.
	sprite_set_calibration(cal_new);
	char body[200];
	snprintf(body, sizeof body, STR_DISPLAY_CALIBRATE_RESULT, before, before_worst, after, after_worst);
	lv_obj_t *card = dialog_open(cal, STR_DISPLAY_CALIBRATE_TITLE, body, false);
	dialog_button(card, LV_SYMBOL_OK "  " STR_DISPLAY_CALIBRATE_SAVE, BUTTON_MAIN, cal_save, NULL);
	dialog_button(card, LV_SYMBOL_REFRESH "  " STR_DISPLAY_CALIBRATE_AGAIN, BUTTON_OTHER, cal_start, NULL);
	dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, cal_cancel, NULL);
}

static void cal_tap(double x, double y) {
	cal_raw[cal_i][0] = x;
	cal_raw[cal_i][1] = y;
	if (++cal_i == CAL_N) cal_result();
	else cal_show_cross();
	sprite_lvgl_kick();
}

static void cal_show_cross(void) {
	lv_obj_clean(cal);
	int x = cal_pts[cal_i][0], y = cal_pts[cal_i][1];
	for (int k = 0; k < 2; k++) { // a cross: two thin bars
		lv_obj_t *bar = lv_obj_create(cal);
		lv_obj_remove_style_all(bar);
		lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
		if (k == 0) { lv_obj_set_size(bar, 40, 2); lv_obj_set_pos(bar, x - 20, y - 1); }
		else { lv_obj_set_size(bar, 2, 40); lv_obj_set_pos(bar, x - 1, y - 20); }
	}
	lv_obj_t *l = lv_label_create(cal);
	lv_label_set_text_fmt(l, STR_DISPLAY_CALIBRATE_TAP, cal_i + 1, CAL_N);
	lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
	lv_obj_align(l, LV_ALIGN_CENTER, 0, cal_pts[cal_i][1] == 240 ? 60 : 0);
}

static void cal_start(lv_event_t *e) {
	if (!cal) {
		sprite_get_calibration(cal_prev);
		cal = lv_obj_create(lv_layer_top());
		lv_obj_remove_style_all(cal);
		lv_obj_set_size(cal, SPRITE_W, SPRITE_H);
		lv_obj_set_style_bg_color(cal, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(cal, LV_OPA_COVER, 0);
		lv_obj_add_flag(cal, LV_OBJ_FLAG_CLICKABLE); // nothing underneath reacts meanwhile
	} else {
		sprite_set_calibration(cal_prev);
	}
	cal_i = 0;
	cal_n = 0;
	cal_sx = cal_sy = 0;
	cal_show_cross();
	// The button fires on release, so the finger is already up: collect from the next touch.
	cal_collecting = true;
	sprite_calibrating(true);
}

static void cal_reset(lv_event_t *e) {
	const double identity[6] = { 1, 0, 0, 0, 1, 0 };
	sprite_set_calibration(identity);
	settings_set("calibration", "");
	show_toast(STR_DISPLAY_CALIBRATE_WAS_RESET, 3000);
}

static void clock24_changed(lv_event_t *e) {
	clock_24 = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	settings_set("clock", clock_24 ? "24h" : "12h");
	upd_hour_set(upd_hour); // the update time, written the same way
	clock_settings_changed();
}

static void seconds_changed(lv_event_t *e) {
	clock_secs = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	settings_set("seconds", clock_secs ? "yes" : "no");
	clock_settings_changed();
}

// A row of Settings: text on the left, a switch on the right.
static lv_obj_t *switch_row(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(row);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	lv_obj_t *sw = lv_switch_create(row);
	lv_obj_set_size(sw, 72, 38);
	if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
	return row;
}

// A list screen (Settings, Wi-Fi): below the bar, the content inset like every screen, a
// column of headings and rows that sprite scrolls (see drag_input).
static lv_obj_t *list_page(lv_obj_t *scr) {
	lv_obj_t *p = lv_obj_create(scr);
	lv_obj_remove_style_all(p);
	lv_obj_set_size(p, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_set_pos(p, 0, BAR_H);
	lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(p, CONTENT_X, 0);
	lv_obj_set_style_pad_ver(p, CONTENT_TOP, 0);
	lv_obj_set_style_pad_row(p, ROW_GAP, 0);
	lv_obj_set_scroll_dir(p, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
	return p;
}

/* ---- Wi-Fi: its section in Settings, the networks in reach, joining and forgetting. The
   helper does the work (wifi.c); the network to join is kept in settings/wifi.txt. ---- */

#define WIFI_JOIN_WAIT_MS 45000    // how long joining may take before we say it hasn't worked
#define WIFI_TROUBLE_AFTER_MS 6000 // ignore "trouble" this soon: it may be the old network's
#define WIFI_RESCAN_MS 30000       // a list older than this is looked for again on opening

enum entry_mode { ENTRY_PASSWORD, ENTRY_OPEN_PASSWORD, ENTRY_NAME, ENTRY_SSH, ENTRY_DEVNAME };
static void open_entry(enum entry_mode mode, const char *ssid);

static lv_obj_t *wifi_detail_label, *wifi_forget_btn;     // in Settings (and wifi_status_label)
static lv_obj_t *wifi_list, *wifi_note, *wifi_rescan;     // the Wi-Fi screen
static struct wifi_net wifi_nets[WIFI_NETS_MAX];          // what the last scan found
static int wifi_n;
static uint32_t wifi_scanned_at;
static bool wifi_asking, wifi_ever_scanned;
static char wifi_listed[3][WIFI_SSID_MAX * 4 + 2];        // the state the list was drawn for
static char wifi_target[WIFI_SSID_MAX + 1];               // the network being joined, or asked about
static lv_timer_t *wifi_watch;                            // while joining: the status, every 1.5 s
static uint32_t wifi_watch_start;

static void wifi_build_list(void);

static void set_text(lv_obj_t *l, const char *t) {
	if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void wifi_show_status(void) {
	if (!wifi_status_label) return;
	const struct wifi_status *s = &wifi_now;
	char t[200];
	const char *st = s->state;
	bool trying = st[0] && strcmp(st, "INACTIVE") && strcmp(st, "DISCONNECTED") && strcmp(st, "INTERFACE_DISABLED");
	if (s->connected) snprintf(t, sizeof t, STR_WIFI_CONNECTED_TO, s->ssid);
	else if (s->saved[0] && s->trouble) snprintf(t, sizeof t, STR_WIFI_CANT_CONNECT_TO, s->saved);
	else if (s->saved[0] && (trying || wifi_watch)) snprintf(t, sizeof t, STR_WIFI_CONNECTING_TO, s->saved);
	else if (s->saved[0]) snprintf(t, sizeof t, STR_WIFI_NOT_CONNECTED_TO, s->saved);
	else snprintf(t, sizeof t, STR_WIFI_NOT_CONNECTED);
	set_text(wifi_status_label, t);
	set_text(wifi_detail_label, s->connected ? s->address : "");
	if (s->saved[0]) lv_obj_remove_flag(wifi_forget_btn, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag(wifi_forget_btn, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_try_again(lv_event_t *e) {
	dialog_close();
	open_entry(ENTRY_PASSWORD, wifi_target);
}

static void wifi_forget_now(lv_event_t *e);

static void wifi_watch_stop(void) {
	if (wifi_watch) lv_timer_delete(wifi_watch);
	wifi_watch = NULL;
}

// While joining: done when we're connected to it, or wpa_supplicant gives up, or it's too long.
static void wifi_watch_check(void) {
	if (!wifi_watch) return;
	uint32_t ms = lv_tick_elaps(wifi_watch_start);
	if (wifi_now.connected && !strcmp(wifi_now.ssid, wifi_target)) {
		wifi_watch_stop();
		char t[80];
		snprintf(t, sizeof t, STR_WIFI_CONNECTED_TO, wifi_target);
		show_toast(t, 3000);
	} else if ((wifi_now.trouble && ms > WIFI_TROUBLE_AFTER_MS) || ms > WIFI_JOIN_WAIT_MS) {
		wifi_watch_stop();
		char t[80];
		snprintf(t, sizeof t, STR_WIFI_COULDNT_CONNECT, wifi_target);
		lv_obj_t *card = dialog_open(NULL, t, STR_WIFI_COULDNT_CONNECT_NOTE, false);
		dialog_button(card, STR_COMMON_TRY_AGAIN, BUTTON_MAIN, wifi_try_again, NULL);
		dialog_button(card, STR_WIFI_FORGET, BUTTON_DANGER, wifi_forget_now, NULL);
		dialog_button(card, STR_COMMON_CLOSE, BUTTON_OTHER, dialog_cancel, NULL);
	}
	wifi_show_status();
}

static void wifi_status_got(const struct wifi_status *s, const char *error, void *data) {
	wifi_asking = false;
	if (error) return; // no card, or no helper: leave what's shown
	wifi_now = *s;
	wifi_show_status();
	if (showing(page_wifi)) wifi_build_list();
	wifi_watch_check();
}

static void wifi_refresh(void) {
	if (wifi_asking) return; // one question at a time
	wifi_asking = true;
	wifi_status(wifi_status_got, NULL);
}

static void wifi_watch_tick(lv_timer_t *t) { wifi_refresh(); }

static void wifi_joined(const char *error, void *data) {
	if (!error) return; // written: the watch follows it
	wifi_watch_stop();
	char t[80];
	snprintf(t, sizeof t, STR_WIFI_COULDNT_JOIN, wifi_target);
	lv_obj_t *card = dialog_open(NULL, t, reason_text(error), true);
	dialog_button(card, STR_COMMON_CLOSE, BUTTON_OTHER, dialog_cancel, NULL);
	wifi_refresh();
}

// Join a network: save it (the helper writes wifi.txt and has it applied), then back to
// Settings, whose Wi-Fi section follows it, and say how it went.
static void wifi_join_now(const char *ssid, const char *password) {
	snprintf(wifi_target, sizeof wifi_target, "%s", ssid);
	wifi_join(ssid, password, wifi_joined, NULL);
	snprintf(wifi_now.saved, sizeof wifi_now.saved, "%s", ssid); // until the helper says
	wifi_now.connected = wifi_now.trouble = false;
	wifi_watch_stop();
	wifi_watch = lv_timer_create(wifi_watch_tick, 1500, NULL);
	wifi_watch_start = lv_tick_get();
	show_page(page_settings);
	settings_show_tab(TAB_NETWORK); // where Wi-Fi is
	refresh_settings(NULL);
	wifi_show_status();
}

static void wifi_forgot(const char *error, void *data) {
	if (error) {
		lv_obj_t *card = dialog_open(NULL, STR_WIFI_COULDNT_FORGET, reason_text(error), true);
		dialog_button(card, STR_COMMON_CLOSE, BUTTON_OTHER, dialog_cancel, NULL);
	}
	wifi_refresh();
}

static void wifi_forget_now(lv_event_t *e) {
	dialog_close();
	wifi_watch_stop();
	wifi_forget(wifi_forgot, NULL);
	char t[sizeof wifi_now.saved + 16];
	snprintf(t, sizeof t, STR_WIFI_FORGOT, wifi_now.saved[0] ? wifi_now.saved : wifi_target);
	show_toast(t, 3000);
	wifi_now.saved[0] = 0;
	wifi_now.connected = wifi_now.trouble = false;
	wifi_show_status();
	if (showing(page_wifi)) wifi_build_list();
}

static void wifi_forget_ask(lv_event_t *e) {
	char t[sizeof wifi_now.saved + 16];
	snprintf(t, sizeof t, STR_WIFI_FORGET_ASK, wifi_now.saved);
	lv_obj_t *card = dialog_open(NULL, t, STR_WIFI_FORGET_ASK_NOTE, true);
	dialog_button(card, STR_WIFI_FORGET, BUTTON_DANGER, wifi_forget_now, NULL);
	dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
}

static void wifi_scan_got(const struct wifi_net *nets, int n, const char *error, void *data) {
	wifi_scanning = false;
	wifi_ever_scanned = true;
	wifi_scanned_at = lv_tick_get();
	lv_obj_remove_state(wifi_rescan, LV_STATE_DISABLED);
	wifi_n = error ? 0 : n;
	memcpy(wifi_nets, nets, wifi_n * sizeof nets[0]);
	if (error) {
		char t[160];
		snprintf(t, sizeof t, STR_WIFI_COULDNT_LOOK, helper_reason(error));
		lv_label_set_text(wifi_note, t);
	}
	wifi_listed[0][0] = 1; // whatever was drawn, draw it again
	wifi_build_list();
}

static void wifi_start_scan(void) {
	if (wifi_scanning) return;
	wifi_scanning = true;
	lv_obj_add_state(wifi_rescan, LV_STATE_DISABLED);
	lv_label_set_text(wifi_note, STR_WIFI_LOOKING);
	lv_obj_remove_flag(wifi_note, LV_OBJ_FLAG_HIDDEN);
	wifi_scan(wifi_scan_got, NULL);
}

static void wifi_rescan_clicked(lv_event_t *e) { wifi_start_scan(); }

static void wifi_open_join(lv_event_t *e) {
	dialog_close();
	wifi_join_now(wifi_target, "");
}

static void wifi_change_password(lv_event_t *e) {
	dialog_close();
	open_entry(ENTRY_PASSWORD, wifi_target);
}

static void wifi_row_clicked(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0) { open_entry(ENTRY_NAME, ""); return; } // a network that doesn't say its name
	const struct wifi_net *w = &wifi_nets[i];
	snprintf(wifi_target, sizeof wifi_target, "%s", w->ssid);
	bool joined = wifi_now.connected && !strcmp(w->ssid, wifi_now.ssid);
	if (joined || !strcmp(w->ssid, wifi_now.saved)) { // the one we have: change or forget it
		char body[120];
		if (joined) snprintf(body, sizeof body, STR_WIFI_CONNECTED_ADDRESS, wifi_now.address);
		else snprintf(body, sizeof body, STR_WIFI_SAVED_NOTE);
		lv_obj_t *card = dialog_open(NULL, w->ssid, body, true);
		if (w->secured) dialog_button(card, STR_WIFI_CHANGE_PASSWORD, BUTTON_MAIN, wifi_change_password, NULL);
		dialog_button(card, STR_WIFI_FORGET, BUTTON_DANGER, wifi_forget_now, NULL);
		dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
	} else if (!w->supported) {
		char t[80];
		snprintf(t, sizeof t, STR_WIFI_CANT_JOIN, w->ssid);
		lv_obj_t *card = dialog_open(NULL, t, STR_WIFI_CANT_JOIN_NOTE, true);
		dialog_button(card, STR_COMMON_CLOSE, BUTTON_OTHER, dialog_cancel, NULL);
	} else if (w->secured) {
		open_entry(ENTRY_PASSWORD, w->ssid);
	} else {
		char t[80];
		snprintf(t, sizeof t, STR_WIFI_JOIN_OPEN, w->ssid);
		lv_obj_t *card = dialog_open(NULL, t, STR_WIFI_JOIN_OPEN_NOTE, true);
		dialog_button(card, STR_WIFI_JOIN, BUTTON_MAIN, wifi_open_join, NULL);
		dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
	}
}

// One row of the list: the whole row takes the touch, and lightens while pressed.
static void wifi_row(int index, const lv_image_dsc_t *icon, const char *symbol, const char *name,
		     bool lock, const char *note, uint32_t note_color, bool dim) {
	lv_obj_t *r = lv_obj_create(wifi_list);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(r, LIST_ROW_PAD, 0);
	lv_obj_set_style_pad_column(r, 14, 0);
	lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(r, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(r, wifi_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	rounded(r);
	lv_color_t text = lv_color_hex(dim ? C_DIM : C_TEXT);
	if (icon) {
		lv_obj_t *im = lv_image_create(r);
		lv_image_set_src(im, icon);
		lv_obj_set_style_image_recolor(im, text, 0);
		lv_obj_set_style_image_recolor_opa(im, LV_OPA_COVER, 0);
	} else {
		lv_obj_t *l = lv_label_create(r);
		lv_label_set_text(l, symbol);
		lv_obj_set_width(l, NET_ICON);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(l, text, 0);
	}
	lv_obj_t *l = lv_label_create(r);
	lv_label_set_text(l, name);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(l, 1);
	lv_obj_set_style_text_color(l, text, 0);
	if (note) {
		lv_obj_t *nl = lv_label_create(r);
		lv_label_set_text(nl, note);
		lv_obj_set_style_text_font(nl, &lv_font_montserrat_16, 0);
		lv_obj_set_style_text_color(nl, lv_color_hex(note_color), 0);
	}
	if (lock) {
		lv_obj_t *im = lv_image_create(r);
		lv_image_set_src(im, &lock_img);
		lv_obj_set_style_image_recolor(im, lv_color_hex(C_DIM), 0);
		lv_obj_set_style_image_recolor_opa(im, LV_OPA_COVER, 0);
	} else {
		lv_obj_t *sp = lv_obj_create(r); // keeps the notes in line with or without a lock
		lv_obj_remove_style_all(sp);
		lv_obj_set_size(sp, NET_ICON, 1);
	}
}

// The list again, if what it shows has changed: the networks, and which is joined or saved.
static void wifi_build_list(void) {
	char now[3][WIFI_SSID_MAX * 4 + 2] = { { 0 } }; // (compared whole with memcmp: no stray bytes)
	snprintf(now[0], sizeof now[0], "%u", (unsigned)wifi_scanned_at);
	snprintf(now[1], sizeof now[1], "%d%s", wifi_now.connected, wifi_now.ssid);
	snprintf(now[2], sizeof now[2], "%s", wifi_now.saved);
	if (!memcmp(now, wifi_listed, sizeof now)) return;
	memcpy(wifi_listed, now, sizeof now);
	lv_obj_clean(wifi_list);
	if (!wifi_ever_scanned) return;
	if (!wifi_scanning) {
		if (wifi_n) lv_obj_add_flag(wifi_note, LV_OBJ_FLAG_HIDDEN);
		else if (!strncmp(lv_label_get_text(wifi_note), "Looking", 7)) lv_label_set_text(wifi_note, STR_WIFI_NONE_FOUND);
	}
	for (int i = 0; i < wifi_n; i++) {
		const struct wifi_net *w = &wifi_nets[i];
		bool joined = wifi_now.connected && !strcmp(w->ssid, wifi_now.ssid);
		bool saved = !joined && !strcmp(w->ssid, wifi_now.saved);
		const char *note = joined ? STR_WIFI_ROW_CONNECTED : saved ? STR_WIFI_ROW_SAVED : !w->supported ? STR_WIFI_ROW_UNSUPPORTED : NULL;
		wifi_row(i, &wifi_img[wifi_level(w->dbm)], NULL, w->ssid, w->secured, note,
			 joined ? C_ACCENT : C_DIM, !w->supported);
	}
	wifi_row(-1, NULL, LV_SYMBOL_PLUS, STR_WIFI_OTHER, false, NULL, 0, false);
}

static void wifi_choose_clicked(lv_event_t *e) {
	show_page(page_wifi);
	lv_obj_scroll_to_y(page_wifi, 0, LV_ANIM_OFF);
	wifi_refresh();
	if (!wifi_ever_scanned || lv_tick_elaps(wifi_scanned_at) > WIFI_RESCAN_MS) wifi_start_scan();
}

static void wifi_back_clicked(lv_event_t *e) {
	show_page(page_settings);
	refresh_settings(NULL);
}

static void build_wifi(lv_obj_t *scr) {
	page_wifi = list_page(scr);
	lv_obj_t *br = button_row(page_wifi);
	lv_obj_set_style_translate_x(br, BACK_X - CONTENT_X, 0); // the back button's place (BACK_X)
	lv_obj_set_width(button(br, LV_SYMBOL_LEFT "  " STR_BACK_SETTINGS, BUTTON_MAIN, wifi_back_clicked, NULL), BACK_W);
	wifi_rescan = button(br, LV_SYMBOL_REFRESH "  " STR_WIFI_LOOK_AGAIN, BUTTON_OTHER, wifi_rescan_clicked, NULL);
	section(page_wifi, STR_WIFI_HEAD_NETWORKS);
	wifi_note = lv_label_create(page_wifi);
	lv_obj_set_style_text_color(wifi_note, lv_color_hex(C_DIM), 0);
	lv_label_set_text(wifi_note, "");
	wifi_list = lv_obj_create(page_wifi);
	lv_obj_remove_style_all(wifi_list);
	lv_obj_set_size(wifi_list, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(wifi_list, LIST_ROW_GAP, 0);
	lv_obj_remove_flag(wifi_list, LV_OBJ_FLAG_SCROLLABLE);
}

/* ---- typing: a screen with one field and sprite's own keyboard under it ----

   The keyboard fills the bottom of the screen: five rows of 48 px keys (the smallest touch
   target), 6 px apart. Digits are always on the top row; one layer has the letters, one
   every other printable ASCII character. Shift is for one letter, or tapped again, for all
   of them until it's tapped once more. A key acts as it's pressed, and only backspace
   repeats when held. */

#define KB_KEY_H 48
#define KB_GAP 6
#define KB_ROWS 5
#define KB_H (KB_ROWS * KB_KEY_H + (KB_ROWS + 1) * KB_GAP)
#define KB_SHIFT_KEY 31            // the index of shift in the letter layers (see kb_lower)

static const char *kb_lower[] = {
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
	"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
	" ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
	LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
	"#+=", ",", " ", ".", "-", "" };
static const char *kb_upper[] = {
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
	" ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
	LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
	"#+=", ",", " ", ".", "-", "" };
static const char *kb_symbols[] = {
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
	"!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
	"-", "_", "=", "+", "[", "]", "{", "}", "\\", "|", "\n",
	"`", "~", ";", ":", "'", "\"", "<", ">", LV_SYMBOL_BACKSPACE, "\n",
	"abc", "?", " ", "/", ".", ",", "" };

// Widths in half keys. NR: no repeat (everything but backspace); HID: a half-key indent.
#define NR LV_BUTTONMATRIX_CTRL_NO_REPEAT
#define HID (LV_BUTTONMATRIX_CTRL_HIDDEN | NR | 1)
static const lv_buttonmatrix_ctrl_t kb_letters_ctrl[] = {
	NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2,
	NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2,
	HID, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, HID,
	NR | 3, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, 3,
	NR | 4, NR | 2, NR | 10, NR | 2, NR | 2 };
static const lv_buttonmatrix_ctrl_t kb_symbols_ctrl[] = {
	NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2,
	NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2,
	NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2,
	NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, NR | 2, 4,
	NR | 4, NR | 2, NR | 8, NR | 2, NR | 2, NR | 2 };
#undef NR
#undef HID

static lv_obj_t *kb, *entry_ta, *entry_title, *entry_hint, *entry_go, *entry_eye, *entry_back_btn;
static enum entry_mode entry_mode;
static char entry_ssid[WIFI_SSID_MAX + 1];
static enum { SHIFT_OFF, SHIFT_ONCE, SHIFT_LOCK } kb_shift;
static bool kb_syms;

static void kb_layout(void) {
	if (kb_syms) {
		lv_buttonmatrix_set_map(kb, kb_symbols);
		lv_buttonmatrix_set_ctrl_map(kb, kb_symbols_ctrl);
	} else {
		lv_buttonmatrix_set_map(kb, kb_shift ? kb_upper : kb_lower);
		lv_buttonmatrix_set_ctrl_map(kb, kb_letters_ctrl);
		if (kb_shift) lv_buttonmatrix_set_button_ctrl(kb, KB_SHIFT_KEY, LV_BUTTONMATRIX_CTRL_CHECKED);
	}
}

static void entry_validate(void) {
	size_t n = strlen(lv_textarea_get_text(entry_ta));
	bool ok = entry_mode == ENTRY_NAME ? n >= 1 && n <= WIFI_SSID_MAX
					  : (n >= 8 && n <= 64) || (entry_mode == ENTRY_OPEN_PASSWORD && n == 0);
	if (entry_mode == ENTRY_SSH) ok = n >= 8 && n <= 64;
	if (entry_mode == ENTRY_DEVNAME) ok = n <= 64; // none: openframe
	if (ok) lv_obj_remove_state(entry_go, LV_STATE_DISABLED);
	else lv_obj_add_state(entry_go, LV_STATE_DISABLED);
}

static void entry_changed(lv_event_t *e) { entry_validate(); }

static void kb_draw_task(lv_event_t *e) {
	lv_draw_task_t *t = lv_event_get_draw_task(e);
	lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
	if (base->part != LV_PART_ITEMS || lv_draw_task_get_type(t) != LV_DRAW_TASK_TYPE_FILL) return;
	if (!lv_obj_has_state(kb, LV_STATE_PRESSED) || base->id1 != lv_buttonmatrix_get_selected_button(kb)) return;
	lv_draw_fill_dsc_t *f = lv_draw_task_get_fill_dsc(t);
	f->color = lv_color_darken(f->color, LV_OPA_30);
}

static void kb_pressed(lv_event_t *e) {
	uint32_t id = lv_buttonmatrix_get_selected_button(kb);
	if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
	const char *t = lv_buttonmatrix_get_button_text(kb, id);
	if (!strcmp(t, LV_SYMBOL_BACKSPACE)) {
		lv_textarea_delete_char(entry_ta);
	} else if (!strcmp(t, LV_SYMBOL_UP)) {
		kb_shift = kb_shift == SHIFT_OFF ? SHIFT_ONCE : kb_shift == SHIFT_ONCE ? SHIFT_LOCK : SHIFT_OFF;
		kb_layout();
	} else if (!strcmp(t, "#+=") || !strcmp(t, "abc")) {
		kb_syms = !kb_syms;
		kb_shift = SHIFT_OFF;
		kb_layout();
	} else {
		lv_textarea_add_text(entry_ta, t);
		if (kb_shift == SHIFT_ONCE) {
			kb_shift = SHIFT_OFF;
			kb_layout();
		}
	}
	entry_validate();
}

static void entry_clear(void) {
	if (entry_ta) lv_textarea_set_text(entry_ta, "");
}

static void entry_back(lv_event_t *e) { show_page(entry_mode == ENTRY_SSH || entry_mode == ENTRY_DEVNAME ? page_settings : page_wifi); }
static void ssh_set_now(const char *password);
static void name_set_now(const char *name);

static void entry_go_clicked(lv_event_t *e) {
	if (lv_obj_has_state(entry_go, LV_STATE_DISABLED)) return;
	if (entry_mode == ENTRY_NAME) {
		char name[WIFI_SSID_MAX + 1];
		snprintf(name, sizeof name, "%s", lv_textarea_get_text(entry_ta));
		open_entry(ENTRY_OPEN_PASSWORD, name);
		return;
	}
	if (entry_mode == ENTRY_SSH) ssh_set_now(lv_textarea_get_text(entry_ta));
	else if (entry_mode == ENTRY_DEVNAME) name_set_now(lv_textarea_get_text(entry_ta));
	else wifi_join_now(entry_ssid, lv_textarea_get_text(entry_ta)); // leaving the screen clears it
}

static void entry_eye_clicked(lv_event_t *e) {
	bool hidden = lv_textarea_get_password_mode(entry_ta);
	lv_textarea_set_password_mode(entry_ta, !hidden);
	lv_label_set_text(lv_obj_get_child(entry_eye, 0), hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void open_entry(enum entry_mode mode, const char *ssid) {
	entry_mode = mode;
	snprintf(entry_ssid, sizeof entry_ssid, "%s", ssid);
	char t[80];
	if (mode == ENTRY_NAME) snprintf(t, sizeof t, STR_ENTRY_NETWORK_NAME);
	else if (mode == ENTRY_SSH) snprintf(t, sizeof t, STR_ENTRY_SSH_PASSWORD);
	else if (mode == ENTRY_DEVNAME) snprintf(t, sizeof t, STR_ENTRY_DEVICE_NAME);
	else snprintf(t, sizeof t, STR_ENTRY_PASSWORD_FOR, ssid);
	lv_label_set_text(entry_title, t);
	lv_label_set_text(entry_hint, mode == ENTRY_NAME ? STR_ENTRY_NETWORK_NAME_HINT
				      : mode == ENTRY_OPEN_PASSWORD ? STR_ENTRY_WIFI_PASSWORD_HINT
				      : mode == ENTRY_SSH ? STR_ENTRY_SSH_PASSWORD_HINT
				      : mode == ENTRY_DEVNAME ? STR_ENTRY_DEVICE_NAME_HINT
								    : STR_ENTRY_PASSWORD_HINT);
	bool plain = mode == ENTRY_NAME || mode == ENTRY_DEVNAME; // what's typed is shown
	lv_label_set_text(lv_obj_get_child(entry_go, 0), mode == ENTRY_NAME ? STR_ENTRY_NEXT : mode == ENTRY_SSH ? STR_ENTRY_SET
							 : mode == ENTRY_DEVNAME ? STR_COMMON_SAVE : STR_WIFI_CONNECT);
	lv_textarea_set_max_length(entry_ta, mode == ENTRY_NAME ? WIFI_SSID_MAX : 64);
	lv_textarea_set_password_mode(entry_ta, !plain);
	if (plain) lv_obj_add_flag(entry_eye, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag(entry_eye, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(lv_obj_get_child(entry_eye, 0), LV_SYMBOL_EYE_OPEN);
	kb_shift = SHIFT_OFF;
	kb_syms = false;
	kb_layout();
	// back to where it came from: Wi-Fi, or Settings for the login password
	bool from_settings = mode == ENTRY_SSH || mode == ENTRY_DEVNAME;
	const char *from = from_settings ? LV_SYMBOL_LEFT "  " STR_BACK_SETTINGS : LV_SYMBOL_LEFT "  " STR_BACK_WIFI;
	lv_label_set_text(lv_obj_get_child(entry_back_btn, 0), from);
	lv_obj_set_width(entry_back_btn, BACK_W);
	lv_obj_update_layout(entry_back_btn);
	lv_obj_set_width(entry_title, SPRITE_W - CONTENT_X - BACK_X - lv_obj_get_width(entry_back_btn) - BUTTON_GAP);
	lv_obj_align_to(entry_title, entry_back_btn, LV_ALIGN_OUT_RIGHT_MID, BUTTON_GAP, 0);
	show_page(page_entry);
	if (from_settings) lv_label_set_text(bar_title, STR_TITLE_SETTINGS);
	entry_clear();
	if (mode == ENTRY_DEVNAME) lv_textarea_set_text(entry_ta, ssid); // the name it has, to change
	entry_validate();
}

static void build_entry(lv_obj_t *scr) {
	page_entry = lv_obj_create(scr);
	lv_obj_remove_style_all(page_entry);
	lv_obj_set_size(page_entry, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_set_pos(page_entry, 0, BAR_H);
	lv_obj_remove_flag(page_entry, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(page_entry, LV_OBJ_FLAG_HIDDEN);

	// the top row: back, and what's wanted
	lv_obj_t *back = entry_back_btn = button(page_entry, LV_SYMBOL_LEFT "  " STR_BACK_WIFI, BUTTON_MAIN, entry_back, NULL);
	lv_obj_set_width(back, BACK_W);
	lv_obj_set_pos(back, BACK_X, CONTENT_TOP);
	lv_obj_update_layout(back);
	int tx = BACK_X + lv_obj_get_width(back) + BUTTON_GAP;
	entry_title = lv_label_create(page_entry);
	lv_label_set_long_mode(entry_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(entry_title, SPRITE_W - CONTENT_X - tx);
	lv_obj_set_style_text_color(entry_title, lv_color_hex(C_TEXT), 0);
	lv_obj_align_to(entry_title, back, LV_ALIGN_OUT_RIGHT_MID, BUTTON_GAP, 0);

	// the field, the eye that shows what's typed, and the button that goes on
	int fy = CONTENT_TOP + BUTTON_H + ROW_GAP + 4;
	entry_go = button(page_entry, STR_WIFI_CONNECT, BUTTON_MAIN, entry_go_clicked, NULL);
	lv_obj_update_layout(entry_go);
	int gw = lv_obj_get_width(entry_go);
	lv_obj_set_pos(entry_go, SPRITE_W - CONTENT_X - gw, fy);
	entry_eye = lv_button_create(page_entry);
	lv_obj_set_size(entry_eye, BUTTON_H, BUTTON_H);
	lv_obj_set_pos(entry_eye, SPRITE_W - CONTENT_X - gw - BUTTON_GAP / 2 - BUTTON_H, fy);
	lv_obj_set_style_bg_color(entry_eye, lv_color_hex(C_BUTTON_OTHER), 0);
	lv_obj_t *el = lv_label_create(entry_eye);
	lv_label_set_text(el, LV_SYMBOL_EYE_OPEN);
	lv_obj_center(el);
	lv_obj_add_event_cb(entry_eye, entry_eye_clicked, LV_EVENT_CLICKED, NULL);
	entry_ta = lv_textarea_create(page_entry);
	lv_textarea_set_one_line(entry_ta, true);
	lv_textarea_set_password_show_time(entry_ta, 1200); // each character shows briefly as it's typed
	lv_obj_set_size(entry_ta, SPRITE_W - 2 * CONTENT_X - gw - BUTTON_GAP - BUTTON_H - BUTTON_GAP / 2, BUTTON_H);
	lv_obj_set_pos(entry_ta, CONTENT_X, fy);
	lv_obj_set_style_bg_color(entry_ta, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(entry_ta, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(entry_ta, 2, 0);
	lv_obj_set_style_border_color(entry_ta, lv_color_hex(C_ACCENT), 0);
	lv_obj_set_style_text_color(entry_ta, lv_color_hex(C_TEXT), 0);
	lv_obj_set_style_text_font(entry_ta, &lv_font_montserrat_20, 0);
	lv_obj_set_style_pad_hor(entry_ta, 14, 0);
	lv_obj_set_style_pad_ver(entry_ta, (BUTTON_H - 4 - lv_font_get_line_height(&lv_font_montserrat_20)) / 2, 0);
	lv_obj_set_style_border_color(entry_ta, lv_color_hex(C_ACCENT), LV_PART_CURSOR);
	lv_obj_set_style_border_width(entry_ta, 2, LV_PART_CURSOR);
	lv_obj_add_state(entry_ta, LV_STATE_FOCUSED); // shows the cursor
	lv_obj_add_event_cb(entry_ta, entry_changed, LV_EVENT_VALUE_CHANGED, NULL);
	rounded(entry_ta);
	entry_hint = lv_label_create(page_entry);
	lv_obj_set_style_text_font(entry_hint, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(entry_hint, lv_color_hex(C_DIM), 0);
	lv_obj_set_pos(entry_hint, CONTENT_X + 2, fy + BUTTON_H + HINT_GAP);

	// the keyboard. None of LVGL's theme: its pressed style for the keys makes LVGL redraw the
	// whole keyboard (19 ms) each time it's pressed and released, as the widget's own state
	// changes. The pressed key is darkened as it's drawn instead, so only that key redraws.
	kb = lv_buttonmatrix_create(page_entry);
	lv_obj_remove_style_all(kb);
	lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
	lv_obj_add_event_cb(kb, kb_draw_task, LV_EVENT_DRAW_TASK_ADDED, NULL);
	lv_obj_set_size(kb, SPRITE_W, KB_H);
	lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_color(kb, lv_color_hex(C_BAR), 0);
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(kb, 0, 0);
	lv_obj_set_style_radius(kb, 0, 0);
	lv_obj_set_style_pad_all(kb, KB_GAP, 0);
	lv_obj_set_style_pad_row(kb, KB_GAP, 0);
	lv_obj_set_style_pad_column(kb, KB_GAP, 0);
	lv_obj_set_style_bg_color(kb, lv_color_hex(C_BUTTON_OTHER), LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(kb, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), LV_PART_ITEMS);
	lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
	lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
	int key_w = (SPRITE_W - 2 * KB_GAP - 9 * KB_GAP) / 10;
	lv_obj_set_style_radius(kb, radius_for(key_w, KB_KEY_H), LV_PART_ITEMS);
	lv_obj_add_event_cb(kb, kb_pressed, LV_EVENT_VALUE_CHANGED, NULL);
	kb_layout();
}

/* ---- remote access: the password for logging in as "of" (sprite-helper ssh-status/-password) ---- */

static lv_obj_t *ssh_status_label, *ssh_detail_label, *ssh_off_button;
static lv_obj_t *eth_status_label, *eth_detail_label;

// The wired network: whether a cable's in, at what speed, and the address it has.
static void eth_show(void) {
	if (!eth_status_label) return;
	struct sprite_net n;
	sysinfo_net(&n);
	char t[64];
	if (!n.eth_up) snprintf(t, sizeof t, STR_NET_CABLE_NONE);
	else if (!n.eth_ip[0]) snprintf(t, sizeof t, STR_NET_CABLE_WAITING);
	else if (n.eth_mbps >= 1000) snprintf(t, sizeof t, STR_NET_CABLE_GBIT, n.eth_mbps / 1000);
	else if (n.eth_mbps > 0) snprintf(t, sizeof t, STR_NET_CABLE_MBIT, n.eth_mbps);
	else snprintf(t, sizeof t, STR_NET_CABLE_CONNECTED);
	set_text(eth_status_label, t);
	set_text(eth_detail_label, n.eth_up ? n.eth_ip : "");
}

/* ---- network details: a page of its own from the Wi-Fi and Ethernet rows (read-only) ---- */

#define PANEL_TEXT_W (SPRITE_W - RAIL_W - (CONTENT_X - 8) - CONTENT_X) // a Settings panel's content
static lv_obj_t *net_notes_label, *netinfo_body;
static char netinfo_shown[sizeof(struct sprite_netinfo)];

static void netinfo_open(lv_event_t *e);

// A status row that opens its interface's details: pressable across its width, its highlight
// reaching past the text as a list row's does, with a chevron at its end in the accent colour,
// so it's plain there's more behind it.
static void row_pressable(lv_obj_t *r, lv_event_cb_t cb, void *data);
static void net_row_pressable(lv_obj_t *r, int kind) { row_pressable(r, netinfo_open, (void *)(intptr_t)kind); }

// A row that opens more about itself: pressable across its width, its highlight reaching past
// the text as a list row's does, with a chevron at its end in the accent colour.
static void row_pressable(lv_obj_t *r, lv_event_cb_t cb, void *data) {
	// a whole touch target tall, whatever's in it, so its highlight has room and its corners
	// are rounded by its size (rounded() needs one)
	lv_obj_set_size(r, PANEL_TEXT_W + 2 * LIST_ROW_PAD, ROW_H);
	lv_obj_set_style_translate_x(r, -LIST_ROW_PAD, 0);
	lv_obj_set_style_pad_hor(r, LIST_ROW_PAD, 0);
	lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(r, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, data);
	rounded(r);
	lv_obj_t *box = lv_obj_create(r);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, 32, 32);
	lv_obj_set_style_bg_color(box, lv_color_hex(C_ACCENT), 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE); // the row takes the touch
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_margin_left(box, -8, 0); // nearer the address than the row's gaps
	rounded(box);
	lv_obj_t *c = lv_label_create(box);
	lv_label_set_text(c, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_font(c, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(c, lv_color_white(), 0);
	lv_obj_center(c);
}

// of-netplan's notes on the settings files (a bad address in ethernet.txt, say): they used
// to be on the login prompt, which sprite covers, so the Network tab says them at its top.
static void net_notes_show(void) {
	if (!net_notes_label) return;
	struct sprite_netinfo ni;
	sysinfo_netinfo(&ni);
	lv_obj_t *first = lv_obj_get_child(lv_obj_get_parent(net_notes_label), 1); // the first heading
	if (ni.notes[0]) {
		set_text(net_notes_label, ni.notes);
		lv_obj_remove_flag(net_notes_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_margin_top(first, HEADING_SPACE, 0);
	} else {
		lv_obj_add_flag(net_notes_label, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_style_margin_top(first, 0, 0);
	}
}

// One line of the details: a name, dimmed, then its value.
static void info_row(lv_obj_t *parent, const char *name, const char *value) {
	lv_obj_t *r = lv_obj_create(parent);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(r, 20, 0);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *k = lv_label_create(r);
	lv_label_set_text(k, name);
	lv_obj_set_width(k, 200);
	lv_obj_set_style_text_color(k, lv_color_hex(C_DIM), 0);
	lv_obj_t *v = lv_label_create(r);
	lv_label_set_text(v, value);
	lv_obj_set_flex_grow(v, 1);
	lv_obj_set_style_text_color(v, lv_color_hex(C_TEXT), 0);
}

static void netif_rows(lv_obj_t *p, const struct sprite_netif *nif, const char *file) {
	char t[96];
	if (nif->ip[0]) snprintf(t, sizeof t, "%s", nif->ip);
	else snprintf(t, sizeof t, STR_COMMON_NONE);
	info_row(p, STR_NET_ADDRESS, t);
	if (nif->ip[0]) {
		if (nif->fixed) snprintf(t, sizeof t, STR_NET_FIXED, file);
		else snprintf(t, sizeof t, STR_NET_DHCP);
		info_row(p, STR_NET_SET_BY, t);
		info_row(p, STR_NET_GATEWAY, nif->gateway[0] ? nif->gateway : STR_COMMON_NONE);
	}
	if (nif->mac[0]) info_row(p, STR_NET_MAC, nif->mac);
}

// The page again, if anything on it has changed.
static void netinfo_show(void) {
	struct sprite_netinfo ni;
	sysinfo_netinfo(&ni);
	if (!memcmp(&ni, netinfo_shown, sizeof ni)) return;
	memcpy(netinfo_shown, &ni, sizeof ni);
	lv_obj_t *p = netinfo_body;
	lv_obj_clean(p);
	bool wifi = netinfo_kind == NETINFO_WIFI;
	// of-netplan's notes about this interface ("Wi-Fi: ...", "Ethernet: ..."), and those
	// about neither (network.yaml's)
	char notes[sizeof ni.notes] = "", *save = NULL;
	for (char *l = strtok_r(ni.notes, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		bool w = !strncmp(l, "Wi-Fi:", 6), e = !strncmp(l, "Ethernet:", 9);
		if ((w || e) && w != wifi) continue;
		size_t at = strlen(notes);
		snprintf(notes + at, sizeof notes - at, "%s%s", at ? "\n" : "", l);
	}
	if (notes[0]) {
		lv_obj_t *l = lv_label_create(p);
		lv_label_set_text(l, notes);
		lv_obj_set_width(l, LV_PCT(100));
		lv_obj_set_style_text_color(l, lv_color_hex(C_WARN), 0);
	}
	section(p, wifi ? STR_NET_HEAD_WIFI : STR_NET_HEAD_ETHERNET);
	netif_rows(p, wifi ? &ni.wifi : &ni.eth, wifi ? "wifi.txt" : "ethernet.txt");
	section(p, STR_NET_HEAD_DNS);
	if (!ni.n_dns) info_row(p, STR_NET_SERVERS, STR_COMMON_NONE);
	for (int i = 0; i < ni.n_dns; i++) info_row(p, i ? "" : STR_NET_SERVERS, ni.dns[i]);
	section(p, STR_NET_HEAD_THIS);
	info_row(p, STR_NET_NAME, ni.host[0] ? ni.host : STR_COMMON_NONE);
}

static void netinfo_open(lv_event_t *e) {
	netinfo_kind = (int)(intptr_t)lv_event_get_user_data(e);
	memset(netinfo_shown, 0, sizeof netinfo_shown);
	netinfo_show();
	show_page(page_netinfo);
	lv_obj_scroll_to_y(page_netinfo, 0, LV_ANIM_OFF);
}

static void netinfo_back(lv_event_t *e) {
	show_page(page_settings);
	refresh_settings(NULL);
}

static void build_netinfo(lv_obj_t *scr) {
	page_netinfo = list_page(scr);
	lv_obj_t *br = button_row(page_netinfo);
	lv_obj_set_style_translate_x(br, BACK_X - CONTENT_X, 0); // the back button's place (BACK_X)
	lv_obj_set_width(button(br, LV_SYMBOL_LEFT "  " STR_BACK_SETTINGS, BUTTON_MAIN, netinfo_back, NULL), BACK_W);
	netinfo_body = lv_obj_create(page_netinfo);
	lv_obj_remove_style_all(netinfo_body);
	lv_obj_set_size(netinfo_body, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(netinfo_body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(netinfo_body, 8, 0);
	lv_obj_remove_flag(netinfo_body, LV_OBJ_FLAG_SCROLLABLE);
}

static void ssh_status_done(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (!ssh_status_label) return;
	if (strcmp(st, "ok")) { set_text(ssh_status_label, STR_COMMON_CANT_TELL); return; }
	char pw[16] = "", *save = NULL;
	int keys = 0;
	for (char *l = strtok_r(reply, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(l, "password=", 9)) snprintf(pw, sizeof pw, "%s", l + 9);
		else if (!strncmp(l, "keys=", 5)) keys = atoi(l + 5);
	}
	char t[96];
	if (!strcmp(pw, "shared")) snprintf(t, sizeof t, STR_SSH_SHARED);
	else if (!strcmp(pw, "set")) snprintf(t, sizeof t, STR_SSH_SET, keys ? STR_SSH_AND_KEYS : "");
	else if (keys) snprintf(t, sizeof t, STR_SSH_KEYS_ONLY);
	else snprintf(t, sizeof t, STR_SSH_OFF);
	set_text(ssh_status_label, t);
	lv_obj_set_style_text_color(ssh_status_label, lv_color_hex(!strcmp(pw, "shared") ? C_WARN : C_TEXT), 0);
	struct sprite_net n;
	sysinfo_net(&n);
	const char *ip = sprite_net_best_ip(&n); // Ethernet's, with both
	snprintf(t, sizeof t, "%s%s", ip[0] ? "ssh of@" : "", ip);
	set_text(ssh_detail_label, strcmp(pw, "none") || keys ? t : "");
	// a way to turn it off again, while there's a way in
	if (ssh_off_button) lv_obj_set_flag(ssh_off_button, LV_OBJ_FLAG_HIDDEN, !strcmp(pw, "none") && !keys);
}

static void ssh_refresh(void) {
	if (ssh_status_label) helper_request("ssh-status\n", ssh_status_done, NULL);
}

static void ssh_set_done(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (strcmp(st, "ok")) {
		lv_obj_t *card = dialog_open(NULL, STR_SSH_NOT_SET, reason_text(st), true);
		dialog_button(card, STR_COMMON_OK, BUTTON_MAIN, dialog_cancel, NULL);
		return;
	}
	show_toast(STR_SSH_WAS_SET, 2500);
	ssh_refresh();
}

static void ssh_set_now(const char *password) {
	char req[96];
	snprintf(req, sizeof req, "ssh-password\n%s\n", password);
	helper_request(req, ssh_set_done, NULL);
	explicit_bzero(req, sizeof req);
	show_page(page_settings); // leaving the screen clears the password
}

static void ssh_set_clicked(lv_event_t *e) { open_entry(ENTRY_SSH, ""); }

static void ssh_off_done(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (strcmp(st, "ok")) {
		lv_obj_t *card = dialog_open(NULL, STR_SSH_STILL_ON, reason_text(st), true);
		dialog_button(card, STR_COMMON_OK, BUTTON_MAIN, dialog_cancel, NULL);
	} else show_toast(STR_SSH_IS_OFF, 2500);
	ssh_refresh();
}

static void ssh_off_go(lv_event_t *e) {
	dialog_close();
	helper_request("ssh-off\n", ssh_off_done, NULL);
}

/* ---- audio: where the sound goes (openframe-scripts' of-audio, through sprite-helper's
   audio-* requests) and how loud (audio.c, in-process) ---- */

#define AUD_CARDS_MAX 6
static struct aud_card { char id[32], kind[8], usbid[12], name[80]; } aud_cards[AUD_CARDS_MAX];
static int aud_n;
static char aud_out[32], aud_kind[8], aud_mixer[64];
static bool aud_direct, aud_managed = true, aud_known;
static bool aud_rate48, aud_rate_known; // the mixer at 48 kHz (of-audio's mixer_rate), and whether it says
static lv_obj_t *aud_list, *aud_vol_head, *aud_now, *aud_slider_row, *aud_slider, *aud_direct_row, *aud_note;
static lv_obj_t *aud_rate_row, *aud_rate_note;
static lv_obj_t *aud_dac_head, *aud_dac_label; // a USB DAC's section: its name; how it's connected,
                                               // what it takes, what it's playing

// The OpenFrame's own card by what it is to an owner; a DAC by the full name it gives (its
// maker's too: "Audient iD4", where ALSA's short name is "iD4").
static const char *aud_card_name(const struct aud_card *c) {
	if (!strcmp(c->kind, "builtin")) return STR_AUDIO_BUILTIN;
	static struct audio_dac d;
	return audio_dac_info(c->id, &d) && d.name[0] ? d.name : c->name;
}

static void aud_now_show(int v) {
	char t[16] = "";
	if (v == 0) snprintf(t, sizeof t, STR_COMMON_OFF);
	else if (v > 0) snprintf(t, sizeof t, STR_COMMON_NOW_PERCENT, v);
	set_text(aud_now, t);
}

static void aud_choose(lv_event_t *e);

// A rate as people say it: 44.1 kHz, 96 kHz (a USB DAC's momentary rate, near enough).
static void rate_text(int r, char *o, size_t n) {
	r = (r + 50) / 100 * 100;
	if (r % 1000) snprintf(o, n, STR_AUDIO_RATE_KHZ_FRAC, r / 1000.0);
	else snprintf(o, n, STR_AUDIO_RATE_KHZ, r / 1000);
}

// The DAC in use: how it's connected, what it takes, and what it's playing now, as the kernel
// has it (audio.c), every second while the tab's in view.
static void aud_dac_show(void) {
	if (!aud_dac_label) return;
	struct audio_dac d;
	bool shown = !strcmp(aud_kind, "usb") && audio_dac_info(aud_out, &d);
	show_if(aud_dac_head, shown);
	show_if(aud_dac_label, shown);
	if (!shown) return;
	char head[96];
	snprintf(head, sizeof head, "%s", d.name);
	for (char *c = head; *c; c++) *c = (char)toupper((unsigned char)*c); // as the other headings
	set_text(aud_dac_head, head);
	const char *sp = !strcmp(d.speed, "high speed") ? STR_AUDIO_DAC_USB2
		       : !strcmp(d.speed, "full speed") ? STR_AUDIO_DAC_USB1
		       : !strcmp(d.speed, "super speed") ? STR_AUDIO_DAC_USB3 : d.speed;
	char lo[16], hi[16], bits[24], now[80], t[360];
	rate_text(d.rate_min, lo, sizeof lo);
	rate_text(d.rate_max, hi, sizeof hi);
	if (d.bits_min == d.bits_max) snprintf(bits, sizeof bits, "%d-bit", d.bits_max);
	else snprintf(bits, sizeof bits, STR_AUDIO_DAC_BITS_EITHER, d.bits_min, d.bits_max);
	// what it's playing: as it was opened (hw_params), or on an older kernel, as stream0 has it
	bool playing = d.hw_known ? d.hw_rate > 0 : d.running && d.run_rate > 0;
	int now_bits = d.hw_known ? d.hw_bits : d.run_bits, now_rate = d.hw_known ? d.hw_rate : d.run_rate;
	const char *way = aud_direct ? STR_AUDIO_DAC_DIRECT : "mixed";
	if (playing) {
		char r[16];
		rate_text(now_rate, r, sizeof r);
		if (now_bits) snprintf(now, sizeof now, STR_AUDIO_DAC_NOW, now_bits, r, way);
		else snprintf(now, sizeof now, "%s, %s", r, way);
	} else snprintf(now, sizeof now, STR_AUDIO_DAC_NOT_PLAYING);
	// "44.1 to 96 kHz": the unit once
	char *k = strstr(lo, " kHz");
	if (k && d.rate_max != d.rate_min) *k = 0;
	snprintf(t, sizeof t, STR_AUDIO_DAC_LINES, sp,
		 d.async ? STR_AUDIO_DAC_ASYNC : "", bits, lo,
		 d.rate_max != d.rate_min ? STR_AUDIO_DAC_TO : "", d.rate_max != d.rate_min ? hi : "", now);
	set_text(aud_dac_label, t);
}

static void aud_dac_tick(lv_timer_t *t) {
	if (visible && showing(page_settings) && settings_tab == TAB_AUDIO) aud_dac_show();
}

static void aud_show(void) {
	if (!aud_list) return;
	lv_obj_clean(aud_list);
	for (int i = 0; i < aud_n; i++) {
		struct aud_card *c = &aud_cards[i];
		lv_obj_t *r = lv_obj_create(aud_list); // a list row: pressable, the chosen one ticked
		lv_obj_remove_style_all(r);
		lv_obj_set_size(r, LV_PCT(100), ROW_H); // the list's width: see aud_list
		lv_obj_set_style_pad_hor(r, LIST_ROW_PAD, 0);
		lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(r, 20, 0);
		lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(r, LV_OPA_20, LV_STATE_PRESSED);
		rounded(r);
		if (aud_managed) {
			lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_add_event_cb(r, aud_choose, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		}
		lv_obj_t *n = lv_label_create(r);
		lv_label_set_text(n, aud_card_name(c));
		lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(n, 1);
		lv_obj_set_style_text_color(n, lv_color_hex(C_TEXT), 0);
		lv_obj_t *k = lv_label_create(r);
		bool on = !strcmp(c->id, aud_out);
		lv_label_set_text(k, on ? LV_SYMBOL_OK : (!strcmp(c->kind, "usb") ? STR_AUDIO_USB : ""));
		lv_obj_set_style_text_color(k, lv_color_hex(on ? C_ACCENT : C_DIM), 0);
		if (!on) lv_obj_set_style_text_font(k, &lv_font_montserrat_16, 0);
	}
	if (!aud_n) {
		lv_obj_t *n = lv_label_create(aud_list);
		lv_label_set_text(n, aud_known ? STR_AUDIO_NO_CARDS : "");
		lv_obj_set_style_text_color(n, lv_color_hex(C_DIM), 0);
	}
	bool usb = !strcmp(aud_kind, "usb"), vol = aud_managed && aud_n && !aud_direct && aud_mixer[0], any_dac = false;
	for (int i = 0; i < aud_n; i++) any_dac |= !strcmp(aud_cards[i].kind, "usb");
	show_if(aud_vol_head, vol);
	show_if(aud_slider_row, vol);
	if (vol && !lv_obj_has_state(aud_slider, LV_STATE_PRESSED)) {
		int v = audio_volume(aud_out, aud_mixer);
		if (v >= 0) lv_slider_set_value(aud_slider, v, LV_ANIM_OFF);
		aud_now_show(v);
	}
	show_if(aud_direct_row, aud_managed && usb);
	aud_dac_show();
	lv_obj_t *sw = lv_obj_get_child(aud_direct_row, 1);
	if (aud_direct) lv_obj_add_state(sw, LV_STATE_CHECKED);
	else lv_obj_remove_state(sw, LV_STATE_CHECKED);
	const char *note = !aud_managed ? STR_AUDIO_NOTE_BY_HAND
			   : usb && aud_direct ? STR_AUDIO_NOTE_DIRECT_ON
			   : usb ? STR_AUDIO_NOTE_DIRECT_OFF
			   : aud_n && !any_dac ? STR_AUDIO_NOTE_PLUG_IN : "";
	set_text(aud_note, note);
	show_if(aud_note, note[0]);
	// The mixer's rate: only while the audio's mixed (Direct to DAC sends it at its own), and
	// only if of-audio has the setting
	bool rate = aud_managed && aud_n && !aud_direct && aud_rate_known;
	show_if(aud_rate_row, rate);
	show_if(aud_rate_note, rate);
	lv_obj_t *rs = lv_obj_get_child(aud_rate_row, 1);
	if (aud_rate48) lv_obj_add_state(rs, LV_STATE_CHECKED);
	else lv_obj_remove_state(rs, LV_STATE_CHECKED);
}

static void aud_status_done(char *reply, void *data) {
	if (strcmp(helper_reply_status(reply), "ok")) return;
	char was[32];
	snprintf(was, sizeof was, "%s", aud_out);
	bool knew = aud_known, was_direct = aud_direct, was_rate48 = aud_rate48;
	char playing[8][80]; // what's playing (of-audio's holder= lines): its units
	int n_playing = 0;
	aud_n = 0;
	aud_out[0] = aud_kind[0] = aud_mixer[0] = 0;
	aud_direct = aud_rate48 = aud_rate_known = false;
	char *save = NULL;
	for (char *l = strtok_r(reply, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(l, "card=", 5) && aud_n < AUD_CARDS_MAX) {
			struct aud_card *c = &aud_cards[aud_n];
			int at = 0;
			if (sscanf(l + 5, "%31s %7s %11s %n", c->id, c->kind, c->usbid, &at) == 3 && at) {
				snprintf(c->name, sizeof c->name, "%s", l + 5 + at);
				aud_n++;
			}
		} else if (!strncmp(l, "output=", 7)) snprintf(aud_out, sizeof aud_out, "%s", l + 7);
		else if (!strncmp(l, "kind=", 5)) snprintf(aud_kind, sizeof aud_kind, "%s", l + 5);
		else if (!strncmp(l, "mixer=", 6)) snprintf(aud_mixer, sizeof aud_mixer, "%s", l + 6);
		else if (!strncmp(l, "direct=", 7)) aud_direct = !strcmp(l + 7, "yes");
		else if (!strncmp(l, "managed=", 8)) aud_managed = !strcmp(l + 8, "yes");
		else if (!strncmp(l, "mixer_rate=", 11)) { aud_rate_known = true; aud_rate48 = !strcmp(l + 11, "48000"); }
		else if (!strncmp(l, "holder=", 7) && n_playing < 8) snprintf(playing[n_playing++], sizeof playing[0], "%s", l + 7);
	}
	// The sound going elsewhere, or another way (direct, the mixer's rate): an app of ours already playing goes
	// on the old way until it opens its sound again, so it's restarted (of-audio restarts the
	// system's services). Where it was: behind Settings, say, it comes back behind.
	if (knew && (strcmp(was, aud_out) || was_direct != aud_direct || was_rate48 != aud_rate48))
		for (int i = 0; i < n_playing; i++) {
			char id[64];
			if (sscanf(playing[i], "sprite-app-%63[^.].service", id) != 1) continue;
			for (int k = 0; k < apps_count(); k++) {
				struct sprite_app *a = apps_get(k);
				if (strcmp(a->id, id)) continue;
				apps_restart(a, sprite_foreground() != a);
			}
		}
	aud_known = true;
	// a DAC just plugged in and made the output (of-audio): say so, wherever we are
	if (knew && strcmp(was, aud_out) && !strcmp(aud_kind, "usb"))
		for (int i = 0; i < aud_n; i++)
			if (!strcmp(aud_cards[i].id, aud_out)) {
				char t[120];
				snprintf(t, sizeof t, STR_AUDIO_PLAYING_THROUGH, aud_card_name(&aud_cards[i]));
				show_toast(t, 3500);
			}
	aud_show();
	bar_aud_update();
}

static void aud_refresh(void) { helper_request("audio-status\n", aud_status_done, NULL); }

static void aud_set_done(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (strcmp(st, "ok")) {
		lv_obj_t *card = dialog_open(NULL, STR_AUDIO_NOT_CHANGED, reason_text(st), true);
		dialog_button(card, STR_COMMON_OK, BUTTON_MAIN, dialog_cancel, NULL);
	}
	aud_refresh();
}

static void aud_choose(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= aud_n || !strcmp(aud_cards[i].id, aud_out)) return;
	char req[64];
	snprintf(req, sizeof req, "audio-set\noutput=%s\n", !strcmp(aud_cards[i].kind, "usb") ? aud_cards[i].usbid : "builtin");
	helper_request(req, aud_set_done, NULL);
}

static void aud_direct_changed(lv_event_t *e) {
	bool direct = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	helper_request(direct ? "audio-set\ndirect=yes\n" : "audio-set\ndirect=no\n", aud_set_done, NULL);
}

static void aud_rate_changed(lv_event_t *e) {
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	helper_request(on ? "audio-set\nmixer_rate=48000\n" : "audio-set\nmixer_rate=44100\n", aud_set_done, NULL);
}

// The volume, set as the slider moves (in-process: nothing's started for each step), and kept
// for the next start once it's let go.
static void aud_slid(lv_event_t *e) {
	int v = (int)lv_slider_get_value(aud_slider);
	if (audio_set_volume(aud_out, aud_mixer, v)) { aud_now_show(v); bar_aud_icon(v); }
}

// Kept for the next start once the slider's been left AUD_STORE_MS (a root process each time:
// not at every lift of a finger working it, see BL_SETTLE_MS)
#define AUD_STORE_MS 2000
static lv_timer_t *aud_store_timer;
static void aud_store_done(char *reply, void *data) {}
static void aud_store(lv_timer_t *t) {
	aud_store_timer = NULL;
	helper_request("audio-store\n", aud_store_done, NULL);
}
static void aud_released(lv_event_t *e) {
	timer_cancel(&aud_store_timer);
	aud_store_timer = timer_once(aud_store, AUD_STORE_MS);
}
static void aud_pressed(lv_event_t *e) { timer_cancel(&aud_store_timer); }

/* ---- the bar's speaker: the volume there, or Settings at Audio ---- */

static lv_timer_t *bar_vol_timer;

// Whether the OpenFrame sets the volume (mixed, with a volume control): then the speaker
// opens its slider. Otherwise (Direct to DAC, someone's own asound.conf) it opens Settings at
// Audio.
static bool aud_can_slide(void) { return aud_known && aud_managed && aud_n && !aud_direct && aud_mixer[0]; }

static void bar_vol_hide(void) {
	timer_cancel(&bar_vol_timer);
	if (bar_vol) lv_obj_add_flag(bar_vol, LV_OBJ_FLAG_HIDDEN);
}
static void bar_vol_timeout(lv_timer_t *t) { bar_vol_timer = NULL; bar_vol_hide(); }
static void bar_vol_later(void) {
	timer_cancel(&bar_vol_timer);
	bar_vol_timer = timer_once(bar_vol_timeout, BAR_VOL_MS);
}

// The speaker's waves: by the volume v while the OpenFrame sets it (none at 0, one under
// half, both from half), both otherwise (-1)
static void bar_aud_icon(int v) {
	if (!aud_box) return;
	int waves = v < 0 ? 2 : v == 0 ? 0 : v < 50 ? 1 : 2;
	lv_image_set_src(lv_obj_get_child(aud_box, 0), &speaker_img[waves]);
}

// After audio-status: the speaker shows while there's a sound card
static void bar_aud_update(void) {
	if (!aud_box) return;
	bool show = aud_known && aud_n > 0, was = !lv_obj_has_flag(aud_box, LV_OBJ_FLAG_HIDDEN);
	if (show) bar_aud_icon(aud_can_slide() ? audio_volume(aud_out, aud_mixer) : -1);
	show_if(aud_box, show);
	if (show != was) update_net(NULL); // (it lays out the bar's right-hand icons)
	if (!aud_can_slide()) bar_vol_hide();
}

// The slider, from beyond the clock and date up to the speaker, at the volume now
static void bar_vol_show(void) {
	int v = audio_volume(aud_out, aud_mixer);
	if (v >= 0) lv_slider_set_value(bar_vol_slider, v, LV_ANIM_OFF);
	lv_obj_update_layout(lv_obj_get_parent(bar_vol));
	int x0 = lv_obj_get_x(bar_left) + lv_obj_get_width(bar_left) + 16, x1 = lv_obj_get_x(aud_box) - 4;
	lv_obj_set_pos(bar_vol, x0, 0);
	lv_obj_set_width(bar_vol, x1 - x0);
	// the slider in the middle, with room either side for its knob held (BAR_VOL_ROOM): sized
	// here, not by a flex row, whose margins left the held knob clipped at the right end
	lv_obj_set_width(bar_vol_slider, x1 - x0 - 2 * BAR_VOL_ROOM);
	lv_obj_center(bar_vol_slider);
	lv_obj_remove_flag(bar_vol, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(bar_vol);
	bar_vol_later();
}

static void aud_box_clicked(lv_event_t *e) {
	if (aud_can_slide()) {
		if (lv_obj_has_flag(bar_vol, LV_OBJ_FLAG_HIDDEN)) bar_vol_show();
		else bar_vol_hide();
		return;
	}
	bar_vol_hide();
	bar_icon_tab(TAB_AUDIO);
}

// As Settings' slider does, which follows it (and kept for the next start the same way)
static void bar_vol_slid(lv_event_t *e) {
	int v = (int)lv_slider_get_value(bar_vol_slider);
	if (!audio_set_volume(aud_out, aud_mixer, v)) return;
	bar_aud_icon(v);
	if (aud_slider) lv_slider_set_value(aud_slider, v, LV_ANIM_OFF);
	aud_now_show(v);
}
static void bar_vol_pressed(lv_event_t *e) { timer_cancel(&bar_vol_timer); aud_pressed(e); }
static void bar_vol_released(lv_event_t *e) { aud_released(e); bar_vol_later(); }

// The test sound (audio.c): its button says what's playing while it plays.
static lv_obj_t *aud_test_btn;
static lv_timer_t *aud_test_timer;
static void aud_test_tick(lv_timer_t *t) {
	int s = audio_test_state();
	static const char *says[] = { LV_SYMBOL_AUDIO "  " STR_AUDIO_TEST, LV_SYMBOL_AUDIO "  " STR_AUDIO_TEST_LEFT, LV_SYMBOL_AUDIO "  " STR_AUDIO_TEST_RIGHT,
				      LV_SYMBOL_AUDIO "  " STR_AUDIO_TEST_BOTH };
	set_text(lv_obj_get_child(aud_test_btn, 0), says[s > 0 ? s : 0]);
	if (s > 0) return;
	lv_timer_delete(aud_test_timer);
	aud_test_timer = NULL;
	if (s < 0) {
		audio_test_seen();
		show_toast(aud_direct ? STR_AUDIO_TEST_BUSY : STR_AUDIO_TEST_FAILED, 3500);
	}
}
static void aud_test_clicked(lv_event_t *e) {
	if (!audio_test_sound() || aud_test_timer) return;
	aud_test_timer = lv_timer_create(aud_test_tick, 100, NULL);
	aud_test_tick(NULL);
}

// A card coming or going (a DAC plugged in or out): of-audio's hotplug has a moment's work to
// do, then the tab, and the notice if the sound has moved to a DAC.
static void aud_later(lv_timer_t *t) { aud_refresh(); }
static void aud_watch(lv_timer_t *t) {
	static char seen[1024];
	static bool first = true;
	char now[1024] = "";
	FILE *f = fopen("/proc/asound/cards", "r");
	if (f) {
		size_t n = fread(now, 1, sizeof now - 1, f);
		now[n] = 0;
		fclose(f);
	}
	if (first) {
		first = false;
		snprintf(seen, sizeof seen, "%s", now);
		aud_refresh(); // what there is to begin with (no notice for it)
		return;
	}
	if (strcmp(now, seen)) {
		snprintf(seen, sizeof seen, "%s", now);
		lv_timer_t *l = lv_timer_create(aud_later, 2500, NULL);
		lv_timer_set_repeat_count(l, 1);
	}
}

/* ---- the OpenFrame's name: name.txt on OF-BOOT, through sprite-helper's name-set ---- */

static lv_obj_t *name_label, *name_host_label;

static void name_refresh(void) {
	if (!name_label) return;
	char name[96], host[72];
	sysinfo_name(name, sizeof name, host, sizeof host);
	set_text(name_label, name);
	// its name on the network (what the router lists), when that isn't just the name
	set_text(name_host_label, strcasecmp(name, host) ? host : "");
}

static void name_set_done(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (strcmp(st, "ok")) {
		lv_obj_t *card = dialog_open(NULL, STR_NAME_NOT_CHANGED, reason_text(st), true);
		dialog_button(card, STR_COMMON_OK, BUTTON_MAIN, dialog_cancel, NULL);
	} else show_toast(STR_NAME_RENAMED, 2500);
	name_refresh();
}

static void name_set_now(const char *name) {
	char req[96 + 16], n[72];
	// spaces at either end are no part of a name
	while (*name == ' ') name++;
	snprintf(n, sizeof n, "%s", name);
	for (size_t l = strlen(n); l && n[l - 1] == ' '; l--) n[l - 1] = 0;
	snprintf(req, sizeof req, "name-set\n%s\n", n);
	helper_request(req, name_set_done, NULL);
	show_page(page_settings);
}

static void name_clicked(lv_event_t *e) {
	char name[96], host[72];
	sysinfo_name(name, sizeof name, host, sizeof host);
	open_entry(ENTRY_DEVNAME, name);
}

static void ssh_off_clicked(lv_event_t *e) {
	lv_obj_t *card = dialog_open(NULL, STR_SSH_OFF_ASK,
				     STR_SSH_OFF_ASK_NOTE, true);
	dialog_button(card, STR_SSH_TURN_OFF, BUTTON_DANGER, ssh_off_go, NULL);
	dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
}

/* ---- updates: the Updates tab (sprite-helper's update-* requests, of-update) ---- */

static lv_obj_t *upd_status_label, *upd_detail_label, *upd_note_label, *upd_now_btn, *upd_restart_btn, *upd_check_btn;
static lv_obj_t *upd_bar, *upd_buttons, *upd_list;
static bool upd_asking;
// When Check now or Update now was tapped: the helper answers before of-update has started, so
// until the status shows it running, or a look newer than the tap, it isn't taken as idle.
static time_t upd_requested;
static void show_if(lv_obj_t *o, bool show);
static char upd_listed[2048]; // what the list shows now, to redraw it only when that changes

// A part of the OpenFrame tier, by what it is, for its small print.
static const char *upd_part(const char *pkg) {
	if (!strcmp(pkg, "openframe-sprite")) return STR_UPD_PART_SPRITE;
	if (!strcmp(pkg, "openframe-scripts")) return STR_UPD_PART_SYSTEM;
	if (!strcmp(pkg, "openframe-crystalhd")) return STR_UPD_PART_CRYSTALHD;
	if (!strcmp(pkg, "openframe-resampler")) return STR_UPD_PART_RESAMPLER;
	if (!strncmp(pkg, "linux-image-", 12)) return STR_UPD_PART_KERNEL;
	return pkg;
}

static lv_obj_t *upd_small(const char *text) {
	lv_obj_t *l = lv_label_create(upd_list);
	lv_label_set_text(l, text);
	lv_obj_set_width(l, LV_PCT(100));
	lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
	return l;
}

static const char *no_epoch(const char *v) { const char *c = strchr(v, ':'); return c ? c + 1 : v; }

// A row of the list: a name, and on the right what it brings (or a tick once it's in).
static lv_obj_t *upd_row(const char *name, const char *what, bool done) {
	lv_obj_t *r = lv_obj_create(upd_list);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *n = lv_label_create(r);
	lv_label_set_text(n, name);
	lv_obj_set_style_text_color(n, lv_color_hex(done ? C_DIM : C_TEXT), 0);
	lv_obj_t *v = lv_label_create(r);
	lv_label_set_text(v, done ? LV_SYMBOL_OK : what);
	lv_obj_set_style_text_color(v, lv_color_hex(done ? C_ACCENT : C_DIM), 0);
	if (!done) lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
	lv_obj_set_flex_grow(n, 1); // the name takes the room, so the rest sits at the right
	lv_obj_set_style_pad_column(r, 20, 0);
	return r;
}

/* ---- What's new: an update's notes (of-update news), from the Updates tab's rows ---- */

static lv_obj_t *news_body;
static char news_which[64]; // "openframe", "firmware", or an app's package

static void news_open(lv_event_t *e);

// The notes, for the row that was tapped: each of its parts (the OpenFrame's are Sprite, the
// System and the Kernel), its version, and what's new since the one in, by version.
static void news_done(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	lv_obj_clean(news_body);
	if (strcmp(st, "ok")) {
		lv_obj_t *l = lv_label_create(news_body);
		lv_label_set_text(l, strncmp(st, "error: ", 7) ? STR_COMMON_CANT_TELL : st + 7);
		return;
	}
	static char lines[256][200];
	int n = 0;
	char *save = NULL;
	for (char *l = strtok_r(reply, "\n", &save); l && n < 256; l = strtok_r(NULL, "\n", &save))
		snprintf(lines[n++], sizeof lines[0], "%s", l);
	bool any = false;
	for (int i = 0; i < n; i++) {
		char pkg[64], ver[48], tier[12];
		if (strncmp(lines[i], "part=", 5) || sscanf(lines[i] + 5, "%63s %47s %11s", pkg, ver, tier) != 3) continue;
		if (strcmp(news_which, "openframe") && strcmp(news_which, "firmware") ? strcmp(pkg, news_which) : strcmp(tier, news_which)) continue;
		if (!strncmp(pkg, "linux-image-openframe-", 22)) continue; // the kernel's metapackage
		any = true;
		char head[128], up[64];
		const char *name = strcmp(tier, "openframe") ? (strcmp(tier, "firmware") ? (strncmp(pkg, "ofapp-", 6) ? pkg : pkg + 6) : STR_UPD_FIRMWARE) : upd_part(pkg);
		snprintf(up, sizeof up, "%s", name);
		for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
		snprintf(head, sizeof head, STR_UPD_NOTES_HEAD, up, no_epoch(ver));
		section(news_body, head);
		// its notes: news=<pkg>\t<version>\t<note>, newest first
		int notes = 0;
		for (int j = 0; j < n; j++) {
			if (strncmp(lines[j], "news=", 5)) continue;
			char *f1 = lines[j] + 5, *f2 = strchr(f1, '\t'), *f3 = f2 ? strchr(f2 + 1, '\t') : NULL;
			if (!f3 || (size_t)(f2 - f1) != strlen(pkg) || strncmp(f1, pkg, f2 - f1)) continue;
			// (not by version: only the last of a push is ever released, so the ones in between
			// would be versions nobody saw)
			char t[220];
			snprintf(t, sizeof t, "- %s", f3 + 1);
			lv_obj_t *l = lv_label_create(news_body);
			lv_label_set_text(l, t);
			lv_obj_set_width(l, LV_PCT(100));
			lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
			notes++;
		}
		if (!notes) {
			lv_obj_t *l = lv_label_create(news_body);
			lv_label_set_text(l, STR_UPD_NOTES_DEFAULT);
			lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
		}
	}
	if (!any) {
		lv_obj_t *l = lv_label_create(news_body);
		lv_label_set_text(l, STR_UPD_NOTES_NOTHING);
		lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
	}
}

static void news_open(lv_event_t *e) {
	snprintf(news_which, sizeof news_which, "%s", (const char *)lv_event_get_user_data(e));
	lv_obj_clean(news_body);
	lv_obj_t *l = lv_label_create(news_body);
	lv_label_set_text(l, STR_UPD_LOADING);
	lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
	show_page(page_news);
	lv_obj_scroll_to_y(page_news, 0, LV_ANIM_OFF);
	helper_request("update-news\n", news_done, NULL);
}

static void news_back(lv_event_t *e) {
	show_page(page_settings);
	refresh_settings(NULL);
}

static void build_news(lv_obj_t *scr) {
	page_news = list_page(scr);
	lv_obj_t *br = button_row(page_news);
	lv_obj_set_style_translate_x(br, BACK_X - CONTENT_X, 0); // the back button's place (BACK_X)
	lv_obj_set_width(button(br, LV_SYMBOL_LEFT "  " STR_BACK_SETTINGS, BUTTON_MAIN, news_back, NULL), BACK_W);
	news_body = lv_obj_create(page_news);
	lv_obj_remove_style_all(news_body);
	lv_obj_set_size(news_body, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(news_body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(news_body, 8, 0);
	lv_obj_remove_flag(news_body, LV_OBJ_FLAG_SCROLLABLE);
}


// The updates waiting as the list shows them: a row for each firmware, each part of the
// OpenFrame's own (not the kernel's metapackage, which goes in with the kernel) and each app,
// and one for all of Debian's. So "3 updates waiting" over three rows, where of-update's
// pending= counts packages (a new kernel is two). *done: the rows that are in.
static int upd_rows(char items[][96], int n, int *done) {
	int rows = 0, in_rows = 0, deb = 0, deb_in = 0;
	for (int i = 0; i < n; i++) {
		char p[64], v[48], tier[12];
		int in = 0;
		if (sscanf(items[i], "%63s %47s %11s %d", p, v, tier, &in) < 4) continue;
		if (!strcmp(tier, "debian")) { deb++; deb_in += in == 1; continue; }
		if (!strcmp(tier, "openframe") && !strncmp(p, "linux-image-openframe-", 22)) continue;
		if (strcmp(tier, "firmware") && strcmp(tier, "openframe") && strcmp(tier, "apps")) continue;
		rows++;
		in_rows += in == 1 && strcmp(tier, "firmware"); // (firmware: flashed at the restart)
	}
	if (deb) { rows++; in_rows += deb_in == deb; }
	if (done) *done = in_rows;
	return rows;
}

// The updates waiting (of-update's item= lines: package, version, tier, done, old version), by
// tier: the firmware first, with what its restart will look like; then the OpenFrame's own as
// one row, its parts (the kernel too) in small print; then each app; then Debian's, together.
// firmware_pending: firmware already installed, to be flashed at the next restart.
static void upd_show_list(char items[][96], int n, bool firmware_pending) {
	// what's listed, to redraw only when it changes (bounded: strncat's room worked out as
	// size - length - 2 wrapped round once the buffer was nearly full, with ~35 waiting)
	char sig[sizeof upd_listed] = "";
	size_t at = 0;
	for (int i = 0; i < n && at < sizeof sig - 1; i++) {
		int w = snprintf(sig + at, sizeof sig - at, "%s;", items[i]);
		at = w < 0 ? at : at + (size_t)w < sizeof sig ? at + (size_t)w : sizeof sig - 1;
	}
	if (firmware_pending && at < sizeof sig - 1) snprintf(sig + at, sizeof sig - at, "fw");
	if (!strcmp(sig, upd_listed)) return;
	snprintf(upd_listed, sizeof upd_listed, "%s", sig);
	lv_obj_clean(upd_list);
	show_if(upd_list, n > 0 || firmware_pending);
	if (!n && !firmware_pending) return;
	struct { char pkg[64], ver[48], tier[12], old[48]; int done; } it[64];
	int m = 0;
	for (int i = 0; i < n && m < 64; i++) {
		strcpy(it[m].old, "none");
		if (sscanf(items[i], "%63s %47s %11s %d %47s", it[m].pkg, it[m].ver, it[m].tier, &it[m].done, it[m].old) >= 4) m++;
	}
	char t[160];
	// the firmware
	bool fw_shown = false;
	for (int i = 0; i < m; i++) {
		if (strcmp(it[i].tier, "firmware")) continue;
		section(upd_list, STR_UPD_HEAD_FIRMWARE);
		snprintf(t, sizeof t, STR_UPD_VERSION, no_epoch(it[i].ver));
		row_pressable(upd_row(t, "", false), news_open, "firmware"); // flashed at the restart, so never ticked here
		fw_shown = true;
	}
	if (firmware_pending && !fw_shown) {
		section(upd_list, STR_UPD_HEAD_FIRMWARE);
		upd_row(STR_UPD_FIRMWARE_WHEN, "", false);
		fw_shown = true;
	}
	if (fw_shown)
		upd_small(STR_UPD_FIRMWARE_NOTE);
	// the OpenFrame's own: a row a part (Sprite, System, Kernel...), its version at the right
	// and its notes behind the chevron
	static char part_pkgs[8][64]; // what their rows open (the list is redrawn, these stay)
	int n_parts = 0;
	for (int i = 0; i < m && n_parts < 8; i++) {
		if (strcmp(it[i].tier, "openframe")) continue;
		if (!strncmp(it[i].pkg, "linux-image-openframe-", 22)) continue; // the kernel's metapackage
		if (!n_parts) section(upd_list, STR_UPD_HEAD_OPENFRAME);
		snprintf(part_pkgs[n_parts], sizeof part_pkgs[0], "%.63s", it[i].pkg);
		row_pressable(upd_row(upd_part(it[i].pkg), no_epoch(it[i].ver), it[i].done), news_open, part_pkgs[n_parts++]);
	}
	// apps
	bool apps = false;
	static char app_pkgs[32][64]; // what their rows open (the list is redrawn, these stay)
	int n_app_pkgs = 0;
	for (int i = 0; i < m; i++) {
		if (strcmp(it[i].tier, "apps")) continue;
		if (!apps) { section(upd_list, STR_UPD_HEAD_APPS); apps = true; }
		if (n_app_pkgs >= 32) continue;
		snprintf(app_pkgs[n_app_pkgs], sizeof app_pkgs[0], "%.63s", it[i].pkg);
		row_pressable(upd_row(strncmp(it[i].pkg, "ofapp-", 6) ? it[i].pkg : it[i].pkg + 6, no_epoch(it[i].ver), it[i].done),
			      news_open, app_pkgs[n_app_pkgs++]);
	}
	// Debian's
	char names[700] = "";
	int n_deb = 0, deb_done = 0;
	for (int i = 0; i < m; i++) {
		if (strcmp(it[i].tier, "debian")) continue;
		n_deb++;
		deb_done += it[i].done;
		size_t at = strlen(names);
		snprintf(names + at, sizeof names - at, "%s%.63s", at ? ", " : "", it[i].pkg);
	}
	if (n_deb) {
		section(upd_list, STR_UPD_HEAD_DEBIAN);
		snprintf(t, sizeof t, n_deb == 1 ? STR_UPD_PACKAGES_ONE : STR_UPD_PACKAGES, n_deb);
		upd_row(STR_UPD_DEBIAN, t, deb_done == n_deb);
		upd_small(names);
	}
	lv_obj_t *first = lv_obj_get_child(upd_list, 0); // (none if nothing listed has a row)
	if (first) lv_obj_set_style_margin_top(first, 0, 0);
}

// Busy (checking, downloading, installing): no buttons, as there's nothing to press; the
// status line says what's happening, the progress bar how far, and the bar's spinner that
// something is. Idle: the buttons again.
static void upd_busy(bool busy) {
	show_if(upd_buttons, !busy);
}

// A time for the Updates tab: the time today, "yesterday", or the date.
static void upd_when(long t, char *buf, size_t len) {
	time_t now = time(NULL), tt = t;
	struct tm a, b;
	localtime_r(&now, &a);
	localtime_r(&tt, &b);
	if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
		strftime(buf, len, clock_24 ? "%H:%M" : "%-I:%M %P", &b);
	else if (now - tt < 2 * 86400 && (a.tm_yday - b.tm_yday == 1 || (a.tm_yday == 0 && b.tm_mon == 11 && b.tm_mday == 31)))
		snprintf(buf, len, "yesterday");
	else strftime(buf, len, "%e %b", &b);
}

static void show_if(lv_obj_t *o, bool show) {
	if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void upd_started(char *reply, void *data);

/* ---- the update time: when the OpenFrame updates (and restarts, if one needs it), by the hour ---- */

static lv_obj_t *upd_time_label;  // the Updates tab's row
static int time_pick;             // the hour being picked (a stepper's)
static lv_obj_t *time_pick_label;

static void hour_text(char *buf, size_t len, int h) {
	if (clock_24) snprintf(buf, len, "%02d:00", h);
	else snprintf(buf, len, "%d%s", h % 12 ? h % 12 : 12, h < 12 ? "am" : "pm");
}

static void upd_hour_set(int h) {
	if (h < 0 || h > 23) return;
	upd_hour = h;
	if (!upd_time_label) return;
	char t[16];
	hour_text(t, sizeof t, h);
	set_text(upd_time_label, t);
}

static void upd_time_done(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (strcmp(st, "ok")) {
		lv_obj_t *card = dialog_open(NULL, STR_UPD_TIME_NOT_CHANGED, reason_text(st), true);
		dialog_button(card, STR_COMMON_OK, BUTTON_MAIN, dialog_cancel, NULL);
	}
}

static void upd_time_save_hour(int h) {
	if (h == upd_hour) return;
	char req[32];
	snprintf(req, sizeof req, "update-time %02d:00\n", h);
	upd_hour_set(h);
	helper_request(req, upd_time_done, NULL);
}

static void time_step(lv_event_t *e) {
	time_pick = (time_pick + (int)(intptr_t)lv_event_get_user_data(e) + 24) % 24;
	char t[16];
	hour_text(t, sizeof t, time_pick);
	lv_label_set_text(time_pick_label, t);
}

// [-] 3am [+], starting at the current time, in parent (a dialog's card, or a row).
static lv_obj_t *time_stepper(lv_obj_t *parent) {
	time_pick = upd_hour;
	lv_obj_t *r = button_row(parent);
	lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 8, 0);
	lv_obj_set_width(button(r, LV_SYMBOL_MINUS, BUTTON_OTHER, time_step, (void *)(intptr_t)-1), BUTTON_H + 8);
	time_pick_label = lv_label_create(r);
	lv_obj_set_width(time_pick_label, 96);
	lv_obj_set_style_text_align(time_pick_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(time_pick_label, lv_color_hex(C_TEXT), 0);
	char t[16];
	hour_text(t, sizeof t, time_pick);
	lv_label_set_text(time_pick_label, t);
	lv_obj_set_width(button(r, LV_SYMBOL_PLUS, BUTTON_OTHER, time_step, (void *)(intptr_t)1), BUTTON_H + 8);
	return r;
}

static void upd_time_save(lv_event_t *e) {
	dialog_close();
	upd_time_save_hour(time_pick);
}

static void upd_time_clicked(lv_event_t *e) {
	lv_obj_t *card = dialog_open(NULL, STR_UPD_TIME,
				     STR_UPD_TIME_ASK, true);
	lv_obj_t *st = time_stepper(card);
	lv_obj_set_width(st, LV_PCT(100));
	dialog_button(card, STR_COMMON_SAVE, BUTTON_MAIN, upd_time_save, NULL);
	dialog_button(card, STR_COMMON_CANCEL, BUTTON_OTHER, dialog_cancel, NULL);
}

// The progress bar: a percentage, or a segment sliding along it while there's nothing to measure.
#define UPD_NO_BAR (-2)
#define UPD_SLIDING (-1)
#define UPD_SLIDE_W 30   // the sliding segment, in percent of the bar

static void upd_slide(void *bar, int32_t v) {
	int s = v - UPD_SLIDE_W, e = v;
	lv_bar_set_start_value(bar, s < 0 ? 0 : s, LV_ANIM_OFF);
	lv_bar_set_value(bar, e > 100 ? 100 : e, LV_ANIM_OFF);
}

static void upd_bar_show(int progress) {
	show_if(upd_bar, progress != UPD_NO_BAR);
	bool sliding = lv_anim_get(upd_bar, upd_slide) != NULL;
	if (progress == UPD_SLIDING) {
		if (sliding) return;
		lv_bar_set_mode(upd_bar, LV_BAR_MODE_RANGE);
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, upd_bar);
		lv_anim_set_exec_cb(&a, upd_slide);
		lv_anim_set_values(&a, 0, 100 + UPD_SLIDE_W);
		lv_anim_set_duration(&a, 1400);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
		lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
		lv_anim_start(&a);
		return;
	}
	if (sliding) {
		lv_anim_delete(upd_bar, upd_slide);
		lv_bar_set_mode(upd_bar, LV_BAR_MODE_NORMAL);
		lv_bar_set_start_value(upd_bar, 0, LV_ANIM_OFF);
		lv_bar_set_value(upd_bar, 0, LV_ANIM_OFF);
	}
	if (progress >= 0) lv_bar_set_value(upd_bar, progress, LV_ANIM_ON);
}

static void upd_status_done(char *reply, void *data) {
	upd_asking = false;
	const char *st = helper_reply_status(reply);
	if (!upd_status_label) return;
	if (strcmp(st, "ok")) {
		set_text(upd_status_label, strncmp(st, "error: ", 7) ? STR_COMMON_CANT_TELL : st + 7);
		set_text(upd_detail_label, "");
		show_if(upd_now_btn, false);
		show_if(upd_restart_btn, false);
		show_if(upd_check_btn, false);
		return;
	}
	char state[16] = "idle", result[16] = "", message[160] = "", restart[8] = "", firmware[12] = "", notice[200] = "";
	long last = 0, checked = 0, got = 0, size = 0;
	int pending = 0, n_items = 0, n_done = 0, n_partial = 0;
	char now_pkg[64] = "";
	static char items[64][96];
	char *save = NULL;
	for (char *l = strtok_r(reply, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(l, "item=", 5) && n_items < 64) {
			snprintf(items[n_items], sizeof items[0], "%s", l + 5);
			// "package version tier in old": in is 1 once it's in
			char p[64], v[48], tier[12];
			int in = 0;
			if (sscanf(l + 5, "%63s %47s %11s %d", p, v, tier, &in) == 4) n_done += in == 1;
			n_items++;
		}
		else if (!strncmp(l, "got=", 4)) got = atol(l + 4);
		else if (!strncmp(l, "size=", 5)) size = atol(l + 5);
		else if (!strncmp(l, "partial=", 8)) n_partial = atoi(l + 8);
		else if (!strncmp(l, "now=", 4)) snprintf(now_pkg, sizeof now_pkg, "%s", l + 4);
		else if (!strncmp(l, "state=", 6)) snprintf(state, sizeof state, "%s", l + 6);
		else if (!strncmp(l, "result=", 7)) snprintf(result, sizeof result, "%s", l + 7);
		else if (!strncmp(l, "message=", 8)) snprintf(message, sizeof message, "%s", l + 8);
		else if (!strncmp(l, "notice=", 7)) snprintf(notice, sizeof notice, "%s", l + 7);
		else if (!strncmp(l, "restart=", 8)) snprintf(restart, sizeof restart, "%s", l + 8);
		else if (!strncmp(l, "firmware=", 9)) snprintf(firmware, sizeof firmware, "%s", l + 9);
		else if (!strncmp(l, "last=", 5)) last = atol(l + 5);
		else if (!strncmp(l, "checked=", 8)) checked = atol(l + 8);
		else if (!strncmp(l, "pending=", 8)) pending = atoi(l + 8);
		else if (!strncmp(l, "time=", 5)) upd_hour_set(atoi(l + 5));
	}
	bool idle = !strcmp(state, "idle"), needs_restart = !strcmp(restart, "yes"), failed = !strcmp(result, "failed");
	if (upd_requested) {
		time_t now = time(NULL);
		if (!idle || checked >= upd_requested || last >= upd_requested || now - upd_requested > 20)
			upd_requested = 0;          // it's started, it's done, or it never will
		else {
			upd_busy(true);             // not started yet: still as tapped
			upd_bar_show(UPD_SLIDING);
			return;
		}
	}
	char t[96] = "", d[48] = "", w[24];
	uint32_t colour = C_TEXT;
	// The bar: a percentage while there's something to measure; UPD_SLIDING while it's busy
	// with nothing to measure yet (looking, planning, a download not begun); UPD_NO_BAR idle.
	int progress = UPD_NO_BAR;
	if (!strcmp(state, "checking")) { snprintf(t, sizeof t, STR_UPD_CHECKING); progress = UPD_SLIDING; }
	else if (!strcmp(state, "preparing")) { snprintf(t, sizeof t, STR_UPD_PREPARING); progress = UPD_SLIDING; }
	else if (!strcmp(state, "downloading")) {
		snprintf(t, sizeof t, STR_UPD_DOWNLOADING);
		progress = UPD_SLIDING;
		if (size > 0 && got > 0) {
			progress = (int)((long long)got * 100 / size); // (long is 32-bit here: past 21 MB it overflowed)
			if (size >= 1024 * 1024) snprintf(d, sizeof d, STR_UPD_MB_OF, got / 1048576.0, size / 1048576.0);
			else snprintf(d, sizeof d, STR_UPD_KB_OF, got / 1024, size / 1024);
		}
	} else if (!strcmp(state, "installing") && n_items > 0 && n_done == n_items) {
		// all in: unattended-upgrades tidies up for a few seconds more (what it left behind)
		snprintf(t, sizeof t, STR_UPD_FINISHING);
		progress = 100;
	} else if (!strcmp(state, "installing")) {
		// what's going in, by name; before the first, unattended-upgrades works out the order
		if (now_pkg[0]) {
			const char *name = !strncmp(now_pkg, "openframe-", 10) || !strncmp(now_pkg, "linux-image-", 12) ? upd_part(now_pkg)
					   : !strncmp(now_pkg, "ofapp-", 6) ? now_pkg + 6 : now_pkg;
			snprintf(t, sizeof t, STR_UPD_INSTALLING_PKG, name);
		} else if (!n_done) snprintf(t, sizeof t, STR_UPD_INSTALLING_READY);
		else snprintf(t, sizeof t, STR_UPD_INSTALLING);
		progress = UPD_SLIDING;
		if (n_items > 0) {
			// an unpacked one, not yet configured, is half in: so even one update moves
			int p = (n_done * 2 + n_partial) * 50 / n_items;
			if (p > 0) progress = p;
			int rows_in, rows = upd_rows(items, n_items, &rows_in);
			if (rows > 0) snprintf(d, sizeof d, STR_UPD_N_OF, rows_in, rows); // as the list shows them
		}
	}
	else if (needs_restart) { snprintf(t, sizeof t, STR_UPD_RESTART_TO_FINISH); colour = C_WARN; }
	else if (failed) { snprintf(t, sizeof t, STR_UPD_FAILED); colour = C_WARN; }
	else if (pending > 0) {
		int rows = n_items ? upd_rows(items, n_items, NULL) : pending; // (an older of-update: no items)
		if (rows < 1) rows = pending;
		snprintf(t, sizeof t, rows == 1 ? STR_UPD_ONE_WAITING : STR_UPD_N_WAITING, rows);
	}
	else if (last || checked) snprintf(t, sizeof t, STR_UPD_UP_TO_DATE);
	else snprintf(t, sizeof t, STR_UPD_NOT_CHECKED);
	if (idle) {
		long when = failed ? checked : last > checked ? last : checked;
		if (when) { upd_when(when, w, sizeof w); snprintf(d, sizeof d, "%s %s", failed || last < checked ? "checked" : "updated", w); }
	}
	set_text(upd_status_label, t);
	lv_obj_set_style_text_color(upd_status_label, lv_color_hex(colour), 0);
	set_text(upd_detail_label, d);
	// what went wrong, as of-update put it, with a capital; or, all being well, apps it had to
	// remove to finish an update (for a fortnight), so a tile gone isn't a mystery
	const char *why = idle && failed && message[0] ? message : idle && notice[0] ? notice : NULL;
	if (why) {
		char m[208];
		snprintf(m, sizeof m, "%s.", why);
		m[0] = (char)toupper((unsigned char)m[0]);
		set_text(upd_note_label, m);
		lv_obj_set_style_text_color(upd_note_label, lv_color_hex(why == message ? C_WARN : C_DIM), 0);
		show_if(upd_note_label, true);
	} else show_if(upd_note_label, false);
	upd_bar_show(progress);
	upd_show_list(items, idle && needs_restart ? 0 : n_items, !strcmp(firmware, "pending"));
	// A list older than six hours is looked at again as the tab shows it (a check, no more).
	static time_t auto_asked;
	time_t now = time(NULL);
	if (idle && checked && now - checked > 6 * 3600 && now - auto_asked > 600 && settings_tab == TAB_UPDATES && showing(page_settings)) {
		auto_asked = now;
		set_text(upd_status_label, STR_UPD_CHECKING);
		set_text(upd_detail_label, "");
		upd_busy(true);
		upd_requested = now;
		helper_request("update-check\n", upd_started, NULL);
		return;
	}
	upd_busy(!idle);
	if (!idle) return;
	// After an update that didn't work, its button again, to try again: with nothing waiting
	// too, as when an earlier update was left half done (of-update finishes it), so it can
	// always be put right from here, without a command line.
	show_if(upd_now_btn, (pending > 0 || failed) && !needs_restart);
	set_text(lv_obj_get_child(upd_now_btn, 0), failed ? LV_SYMBOL_REFRESH "  " STR_COMMON_TRY_AGAIN : LV_SYMBOL_DOWNLOAD "  " STR_UPD_UPDATE_NOW);
	show_if(upd_restart_btn, needs_restart);
	show_if(upd_check_btn, true);
}

static void upd_refresh(void) {
	if (!upd_status_label || upd_asking) return;
	upd_asking = true;
	helper_request("update-status\n", upd_status_done, NULL);
}

static void upd_started(char *reply, void *data) {
	const char *st = helper_reply_status(reply);
	if (strcmp(st, "ok")) {
		upd_requested = 0; // it won't start: back to the buttons
		lv_obj_t *card = dialog_open(NULL, STR_UPD_COULDNT_START, reason_text(st), true);
		dialog_button(card, STR_COMMON_OK, BUTTON_MAIN, dialog_cancel, NULL);
	}
	upd_refresh();
	update_net(NULL); // the bar's spinner now, not at its next look
}

static void upd_check_clicked(lv_event_t *e) {
	set_text(upd_status_label, STR_UPD_CHECKING);
	set_text(upd_detail_label, "");
	upd_busy(true);
	upd_bar_show(UPD_SLIDING);
	upd_requested = time(NULL);
	helper_request("update-check\n", upd_started, NULL);
}

static void upd_now_clicked(lv_event_t *e) {
	set_text(upd_status_label, STR_UPD_CHECKING);
	set_text(upd_detail_label, "");
	upd_busy(true);
	upd_bar_show(UPD_SLIDING);
	upd_requested = time(NULL);
	helper_request("update-now\n", upd_started, NULL);
}

// Settings: tabs down the left (a rail), the chosen tab's settings on the right. Each panel
// fits without scrolling as things stand; if one grows past the screen, it scrolls on its own.

static void settings_show_tab(int t) {
	settings_tab = t;
	for (int i = 0; i < N_TABS; i++) {
		if (i == t) {
			lv_obj_remove_flag(tab_panel[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_state(tab_btn[i], LV_STATE_CHECKED);
		} else {
			lv_obj_add_flag(tab_panel[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_state(tab_btn[i], LV_STATE_CHECKED);
		}
		lv_obj_set_style_text_color(tab_icon[i], lv_color_hex(i == t ? C_ACCENT : C_TEXT), 0);
	}
	lv_obj_scroll_to_y(tab_panel[t], 0, LV_ANIM_OFF);
	if (t == TAB_UPDATES) upd_refresh();
	if (t == TAB_AUDIO) aud_refresh();
	if (t == TAB_DISPLAY) { bl_limits_ask(); saver_show(); }
	if (t == TAB_NETWORK && wifi_status_label) wifi_refresh(); // (polled only while it shows)
	if (t == TAB_ABOUT) about_show(); // (and polled only while it shows)
}

static void tab_clicked(lv_event_t *e) { settings_show_tab((int)(intptr_t)lv_event_get_user_data(e)); }

// A tab: its glyph and name, the whole row pressable. The chosen one is a lighter box (white
// at 10%) with its glyph in the accent colour; a press is white at 20%, as everywhere.
static lv_obj_t *settings_tab_new(lv_obj_t *rail, int t, const char *glyph, const char *name) {
	lv_obj_t *b = lv_obj_create(rail);
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, LV_PCT(100), TAB_H);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(b, 14, 0);
	lv_obj_set_style_pad_column(b, 14, 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(b, LV_OPA_10, LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
	rounded(b);
	tab_icon[t] = lv_label_create(b);
	lv_label_set_text(tab_icon[t], glyph);
	lv_obj_set_style_text_font(tab_icon[t], &sprite_ui_24, 0);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, name);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	lv_obj_add_event_cb(b, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)t);
	tab_btn[t] = b;
	if (settings_tab_first < 0) settings_tab_first = t; // whichever's put first
	return b;
}

static lv_obj_t *settings_panel_new(int t) {
	lv_obj_t *p = lv_obj_create(page_settings);
	lv_obj_remove_style_all(p);
	lv_obj_set_size(p, SPRITE_W - RAIL_W, SPRITE_H - BAR_H);
	lv_obj_set_pos(p, RAIL_W, 0);
	lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_left(p, CONTENT_X - 8, 0);
	lv_obj_set_style_pad_right(p, CONTENT_X, 0);
	lv_obj_set_style_pad_top(p, CONTENT_TOP, 0);
	lv_obj_set_style_pad_bottom(p, CONTENT_TOP, 0); // scrolled to its end, as far from the bottom as it starts from the top
	lv_obj_set_style_pad_row(p, ROW_GAP, 0);
	lv_obj_set_scroll_dir(p, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
	tab_panel[t] = p;
	return p;
}

/* ---- the screensaver and display sleep: after a while untouched, the screensaver
   (screensaver.c), then the screen off (of-backlight sleep); a touch ends either and goes no
   further (main.c) ---- */

static const int idle_steps[] = { 0, 1, 2, 5, 10, 15, 30, 60 }; // minutes; 0: never
static int ss_min = 5, sleep_min;
static bool sleeping;
static char saver_id[64]; // the screensaver app chosen (sprite.txt's saver=), or "": the flip clock
static lv_obj_t *saver_head, *saver_list;

static struct sprite_app *saver_app(void) {
	for (int i = 0; saver_id[0] && i < apps_count(); i++) {
		struct sprite_app *a = apps_get(i);
		if (a->saver && !strcmp(a->id, saver_id)) return a;
	}
	return NULL;
}

// A row for one of them: its name, and its time with a stepper, kept as key in sprite.txt
struct idle_row { const char *key; int *min; lv_obj_t *label; };
static struct idle_row idle_rows[] = { { "screensaver", &ss_min, NULL }, { "sleep", &sleep_min, NULL } };

static void idle_text(char *o, size_t n, int m) {
	if (!m) snprintf(o, n, STR_DISPLAY_SLEEP_NEVER);
	else if (m == 60) snprintf(o, n, STR_DISPLAY_SLEEP_HOUR);
	else snprintf(o, n, STR_DISPLAY_SLEEP_MIN, m);
}

static void idle_step(lv_event_t *e) {
	intptr_t u = (intptr_t)lv_event_get_user_data(e); // the row's index * 2, +1 for later
	struct idle_row *r = &idle_rows[u / 2];
	int d = u % 2 ? 1 : -1, i = 0, n = sizeof idle_steps / sizeof idle_steps[0];
	while (i < n - 1 && idle_steps[i] != *r->min) i++;
	i += d;
	if (i < 0) i = 0;
	if (i > n - 1) i = n - 1;
	*r->min = idle_steps[i];
	char t[24];
	idle_text(t, sizeof t, *r->min);
	set_text(r->label, t);
	snprintf(t, sizeof t, "%d", *r->min);
	settings_set(r->key, t);
}

// Once a second: the screensaver once it's been left ss_min untouched, the screen off once
// sleep_min, unless something asks it not to (a video playing) or the calibration's screen
// is up. Asleep only if of-backlight's service takes it: without one the screen stays on, and
// a touch isn't swallowed.
static void idle_tick(lv_timer_t *t) {
	if (cal || sprite_sleep_inhibited()) return;
	uint64_t idle = sprite_idle_ms();
	if (ss_min && !sleeping && !screensaver_active() && idle >= (uint64_t)ss_min * 60000) {
		bar_vol_hide();
		screensaver_start(); // the flip clock, and over it the app chosen, once it shows
		sprite_saver_start(saver_app());
	}
	if (sleep_min && !sleeping && idle >= (uint64_t)sleep_min * 60000 && sysinfo_backlight_request("sleep")) {
		sleeping = true;
		sprite_set_asleep(true);
		sprite_saver_end(); // nothing to draw for in the dark (the flip clock costs nothing)
		bar_vol_hide();
	}
}

void shell_wake(void) {
	sleeping = false;
	sysinfo_backlight_request("wake");
	sprite_saver_end(); // awake, and at whatever was under it
	screensaver_stop();
	sprite_request_frame();
}

void shell_screensaver_tap(void) {
	sprite_saver_end();
	screensaver_stop();
}

bool shell_clock_24(void) { return clock_24; }

// The rows in Display's brightness section: each its time, with a stepper
static void idle_build(lv_obj_t *parent) {
	const char *names[] = { STR_DISPLAY_SCREENSAVER, STR_DISPLAY_SLEEP };
	for (int k = 0; k < 2; k++) {
		struct idle_row *ir = &idle_rows[k];
		const char *v = settings_get(ir->key);
		if (v)
			for (size_t i = 0; i < sizeof idle_steps / sizeof idle_steps[0]; i++)
				if (atoi(v) == idle_steps[i]) *ir->min = idle_steps[i];
		lv_obj_t *r = lv_obj_create(parent);
		lv_obj_remove_style_all(r);
		lv_obj_set_size(r, LV_PCT(100), ROW_H);
		lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_right(r, BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0); // its end under Auto's switch, as the others
		lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_t *l = lv_label_create(r);
		lv_label_set_text(l, names[k]);
		lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
		lv_obj_t *st = button_row(r);
		lv_obj_set_size(st, LV_SIZE_CONTENT, LV_SIZE_CONTENT); // (packed from its start: the row puts it at the right)
		lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(st, 8, 0);
		lv_obj_set_width(button(st, LV_SYMBOL_MINUS, BUTTON_OTHER, idle_step, (void *)(intptr_t)(k * 2)), BUTTON_H + 8);
		ir->label = lv_label_create(st);
		lv_obj_set_width(ir->label, 96);
		lv_obj_set_style_text_align(ir->label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(ir->label, lv_color_hex(C_TEXT), 0);
		char t[24];
		idle_text(t, sizeof t, *ir->min);
		lv_label_set_text(ir->label, t);
		lv_obj_set_width(button(st, LV_SYMBOL_PLUS, BUTTON_OTHER, idle_step, (void *)(intptr_t)(k * 2 + 1)), BUTTON_H + 8);
	}
	lv_timer_create(idle_tick, 1000, NULL);
}

// Which screensaver: the flip clock or an app that's one (kind=screensaver), a list with the
// chosen one ticked; only there when such an app's installed
static void saver_choose(lv_event_t *e) {
	struct sprite_app *a = lv_event_get_user_data(e);
	snprintf(saver_id, sizeof saver_id, "%s", a ? a->id : "");
	settings_set("saver", saver_id);
	saver_show();
}

static void saver_row(struct sprite_app *a, bool on) {
	lv_obj_t *r = lv_obj_create(saver_list); // as Audio's list of outputs
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), ROW_H);
	lv_obj_set_style_pad_hor(r, LIST_ROW_PAD, 0);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(r, LV_OPA_20, LV_STATE_PRESSED);
	rounded(r);
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(r, saver_choose, LV_EVENT_CLICKED, a);
	lv_obj_t *n = lv_label_create(r);
	lv_label_set_text(n, a ? a->name : STR_DISPLAY_SAVER_CLOCK);
	lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(n, 1);
	lv_obj_set_style_text_color(n, lv_color_hex(C_TEXT), 0);
	lv_obj_t *k = lv_label_create(r);
	lv_label_set_text(k, on ? LV_SYMBOL_OK : "");
	lv_obj_set_style_text_color(k, lv_color_hex(C_ACCENT), 0);
}

static void saver_show(void) {
	if (!saver_list) return;
	lv_obj_clean(saver_list);
	struct sprite_app *chosen = saver_app();
	int n = 0;
	for (int i = 0; i < apps_count(); i++) n += apps_get(i)->saver;
	show_if(saver_head, n);
	show_if(saver_list, n);
	if (!n) return;
	saver_row(NULL, !chosen);
	for (int i = 0; i < apps_count(); i++)
		if (apps_get(i)->saver) saver_row(apps_get(i), apps_get(i) == chosen);
}

static void saver_build(lv_obj_t *parent) {
	const char *v = settings_get("saver");
	snprintf(saver_id, sizeof saver_id, "%s", v ? v : "");
	saver_head = section(parent, STR_DISPLAY_HEAD_SAVER);
	saver_list = lv_obj_create(parent);
	lv_obj_remove_style_all(saver_list);
	lv_obj_set_size(saver_list, PANEL_TEXT_W + 2 * LIST_ROW_PAD, LV_SIZE_CONTENT); // see aud_list
	lv_obj_set_style_translate_x(saver_list, -LIST_ROW_PAD, 0);
	lv_obj_set_flex_flow(saver_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(saver_list, LV_OBJ_FLAG_SCROLLABLE);
	saver_show();
}

/* ---- About's details: a card with a row each, an icon, its name, and what it is ---- */

#define ABOUT_ROW_H 40

static void about_build(lv_obj_t *parent) {
	static const struct { const char *icon, *name; } rows[N_ABOUT] = {
		[AB_MODEL] = { UI_ICON_MONITOR, STR_ABOUT_ROW_MODEL },
		[AB_FIRMWARE] = { UI_ICON_MICROCHIP, STR_ABOUT_ROW_FIRMWARE },
		[AB_CPU] = { UI_ICON_CPU, STR_ABOUT_ROW_CPU },
		[AB_MEMORY] = { UI_ICON_MEMORY_STICK, STR_ABOUT_ROW_MEMORY },
		[AB_STORAGE] = { UI_ICON_HARD_DRIVE, STR_ABOUT_ROW_STORAGE },
		[AB_ETH] = { UI_ICON_NETWORK, STR_ABOUT_ROW_ETHERNET },
		[AB_WIFI] = { UI_ICON_WIFI, STR_ABOUT_ROW_WIFI },
		[AB_UP] = { UI_ICON_CLOCK, STR_ABOUT_ROW_UP },
		[AB_SPRITE] = { UI_ICON_LAYOUT_GRID, STR_ABOUT_ROW_SPRITE },
		[AB_SYSTEM] = { UI_ICON_PACKAGE, STR_ABOUT_ROW_SYSTEM },
		[AB_KERNEL] = { UI_ICON_TERMINAL, STR_ABOUT_ROW_KERNEL },
		[AB_CRYSTALHD] = { UI_ICON_CLAPPERBOARD, STR_ABOUT_ROW_CRYSTALHD },
		[AB_TEST] = { UI_ICON_FLASK_CONICAL, STR_ABOUT_ROW_TEST_UNIT },
	};
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_pad_hor(card, 16, 0);
	lv_obj_set_style_pad_ver(card, 4, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	for (int i = 0; i < N_ABOUT; i++) {
		lv_obj_t *r = about_row[i] = lv_obj_create(card);
		lv_obj_remove_style_all(r);
		lv_obj_set_size(r, LV_PCT(100), ABOUT_ROW_H);
		lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(r, 12, 0);
		// a hairline under each but the last showing (about_show)
		lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(r, 1, 0);
		lv_obj_set_style_border_color(r, lv_color_hex(C_BG), 0);
		lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_t *ic = lv_label_create(r);
		lv_label_set_text(ic, rows[i].icon);
		lv_obj_set_style_text_font(ic, &sprite_ui_24, 0);
		lv_obj_set_style_text_color(ic, lv_color_hex(C_ACCENT), 0);
		lv_obj_set_width(ic, 24);
		lv_obj_t *nm = lv_label_create(r);
		lv_label_set_text(nm, rows[i].name);
		lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
		lv_obj_set_style_text_color(nm, lv_color_hex(C_DIM), 0);
		lv_obj_set_width(nm, 112);
		lv_obj_t *v = about_val[i] = lv_label_create(r);
		lv_label_set_text(v, "");
		lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
		lv_obj_set_style_text_color(v, lv_color_hex(C_TEXT), 0);
		lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_set_flex_grow(v, 1);
		lv_obj_set_height(v, lv_font_get_line_height(&lv_font_montserrat_16));
	}
	about_show();
}

// The details now (as Settings refreshes): Wi-Fi's row only with a Wi-Fi card, Crystal HD's
// with a card, the test unit's on one
static void about_show(void) {
	if (!about_val[0]) return;
	struct sprite_about a;
	sysinfo_about(&a);
	const char *v[N_ABOUT] = {
		[AB_MODEL] = a.model, [AB_FIRMWARE] = a.firmware, [AB_CPU] = a.cpu, [AB_MEMORY] = a.memory,
		[AB_STORAGE] = a.storage, [AB_ETH] = a.eth, [AB_WIFI] = a.wifi, [AB_UP] = a.up,
		[AB_SPRITE] = a.sprite, [AB_SYSTEM] = a.system, [AB_KERNEL] = a.kernel,
		[AB_CRYSTALHD] = a.crystalhd, [AB_TEST] = a.test_unit ? STR_ABOUT_TEST_UNIT : "",
	};
	lv_obj_t *last = NULL;
	for (int i = 0; i < N_ABOUT; i++) {
		bool show = v[i][0] && (i != AB_WIFI || a.wifi_present);
		show_if(about_row[i], show);
		if (!show) continue;
		set_text(about_val[i], v[i]); // (only if it changed)
		lv_obj_set_style_border_width(about_row[i], 1, 0);
		last = about_row[i];
	}
	if (last) lv_obj_set_style_border_width(last, 0, 0);
}

static void build_settings(lv_obj_t *scr) {
	page_settings = lv_obj_create(scr);
	lv_obj_remove_style_all(page_settings);
	lv_obj_set_size(page_settings, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_set_pos(page_settings, 0, BAR_H);
	lv_obj_remove_flag(page_settings, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *rail = lv_obj_create(page_settings);
	lv_obj_remove_style_all(rail);
	lv_obj_set_size(rail, RAIL_W, SPRITE_H - BAR_H);
	lv_obj_set_style_bg_color(rail, lv_color_hex(C_RAIL), 0);
	lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
	lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(rail, RAIL_PAD, 0);
	lv_obj_set_style_pad_ver(rail, CONTENT_TOP, 0);
	lv_obj_set_style_pad_row(rail, 4, 0);
	lv_obj_remove_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *home = button(rail, LV_SYMBOL_LEFT "  " STR_BACK_HOME, BUTTON_MAIN, back_clicked, NULL);
	lv_obj_set_width(home, BACK_W); // the tabs' width: its arrow and colour set it apart
	lv_obj_set_style_margin_bottom(home, HEADING_SPACE, 0);
	// The tabs for settings, in alphabetical order; then, under a line, About and Power, which
	// aren't settings so much as the unit itself (alphabetical too).
	settings_tab_new(rail, TAB_AUDIO, UI_ICON_VOLUME_2, STR_TAB_AUDIO);
	settings_tab_new(rail, TAB_DISPLAY, UI_ICON_SUN, STR_TAB_DISPLAY);
	lv_obj_t *nt = settings_tab_new(rail, TAB_NETWORK, UI_ICON_WIFI, STR_TAB_NETWORK);
	net_tab_dot = lv_obj_create(nt); // there while the network settings have a problem
	lv_obj_remove_style_all(net_tab_dot);
	lv_obj_add_flag(net_tab_dot, LV_OBJ_FLAG_IGNORE_LAYOUT); // at the end, taking no room from the name
	lv_obj_set_size(net_tab_dot, 10, 10);
	lv_obj_align(net_tab_dot, LV_ALIGN_RIGHT_MID, 0, TAB_DOT_DROP);
	lv_obj_set_style_radius(net_tab_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(net_tab_dot, lv_color_hex(C_WARN), 0);
	lv_obj_set_style_bg_opa(net_tab_dot, LV_OPA_COVER, 0);
	lv_obj_remove_flag(net_tab_dot, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(net_tab_dot, LV_OBJ_FLAG_HIDDEN);
	lv_obj_t *ut = settings_tab_new(rail, TAB_UPDATES, UI_ICON_REFRESH_CW, STR_TAB_UPDATES);
	upd_tab_dot = lv_obj_create(ut); // there while updates are waiting (update_net), as Network's is
	lv_obj_remove_style_all(upd_tab_dot);
	lv_obj_add_flag(upd_tab_dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_set_size(upd_tab_dot, 10, 10);
	lv_obj_align(upd_tab_dot, LV_ALIGN_RIGHT_MID, 0, TAB_DOT_DROP);
	lv_obj_set_style_radius(upd_tab_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(upd_tab_dot, lv_color_hex(C_ACCENT), 0);
	lv_obj_set_style_bg_opa(upd_tab_dot, LV_OPA_COVER, 0);
	lv_obj_remove_flag(upd_tab_dot, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(upd_tab_dot, LV_OBJ_FLAG_HIDDEN);
	lv_obj_t *rule = lv_obj_create(rail); // a hairline, inset as the tabs' glyphs are
	lv_obj_remove_style_all(rule);
	lv_obj_set_size(rule, BACK_W - 2 * 14, 1);
	lv_obj_set_style_margin_left(rule, 14, 0);
	lv_obj_set_style_margin_ver(rule, 4, 0);
	lv_obj_set_style_bg_color(rule, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(rule, LV_OPA_10, 0);
	settings_tab_new(rail, TAB_ABOUT, UI_ICON_INFO, STR_TAB_ABOUT);
	settings_tab_new(rail, TAB_POWER, UI_ICON_POWER, STR_TAB_POWER);

	// Network: Wi-Fi (on units with a card) and remote access
	lv_obj_t *pn = settings_panel_new(TAB_NETWORK);
	net_notes_label = lv_label_create(pn);
	lv_label_set_text(net_notes_label, "");
	lv_obj_set_width(net_notes_label, LV_PCT(100));
	lv_obj_set_style_text_color(net_notes_label, lv_color_hex(C_WARN), 0);
	lv_obj_add_flag(net_notes_label, LV_OBJ_FLAG_HIDDEN);
	struct sprite_net net;
	sysinfo_net(&net);
	if (net.wifi_present) {
		section(pn, STR_NET_HEAD_WIFI);
		lv_obj_t *r = lv_obj_create(pn);
		lv_obj_remove_style_all(r);
		lv_obj_set_size(r, LV_PCT(100), ROW_H);
		lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(r, 20, 0);
		lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
		wifi_status_label = lv_label_create(r);
		lv_label_set_text(wifi_status_label, STR_WIFI_NOT_CONNECTED);
		lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(wifi_status_label, 1);
		lv_obj_set_height(wifi_status_label, lv_font_get_line_height(lv_obj_get_style_text_font(wifi_status_label, 0))); // one line
		lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(C_TEXT), 0);
		wifi_detail_label = lv_label_create(r);
		lv_label_set_text(wifi_detail_label, "");
		lv_obj_set_style_text_color(wifi_detail_label, lv_color_hex(C_DIM), 0);
		net_row_pressable(r, NETINFO_WIFI);
		lv_obj_t *br = button_row(pn);
		button(br, LV_SYMBOL_WIFI "  " STR_WIFI_CHOOSE, BUTTON_MAIN, wifi_choose_clicked, NULL);
		wifi_forget_btn = button(br, STR_WIFI_FORGET, BUTTON_OTHER, wifi_forget_ask, NULL);
		lv_obj_add_flag(wifi_forget_btn, LV_OBJ_FLAG_HIDDEN);
	}
	if (net.eth_present) {
		section(pn, STR_NET_HEAD_ETHERNET);
		lv_obj_t *er = lv_obj_create(pn);
		lv_obj_remove_style_all(er);
		lv_obj_set_size(er, LV_PCT(100), ROW_H);
		lv_obj_set_flex_flow(er, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(er, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(er, 20, 0);
		lv_obj_remove_flag(er, LV_OBJ_FLAG_SCROLLABLE);
		eth_status_label = lv_label_create(er);
		lv_label_set_text(eth_status_label, "");
		lv_label_set_long_mode(eth_status_label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(eth_status_label, 1);
		lv_obj_set_height(eth_status_label, lv_font_get_line_height(lv_obj_get_style_text_font(eth_status_label, 0)));
		lv_obj_set_style_text_color(eth_status_label, lv_color_hex(C_TEXT), 0);
		eth_detail_label = lv_label_create(er);
		lv_label_set_text(eth_detail_label, "");
		lv_obj_set_style_text_color(eth_detail_label, lv_color_hex(C_DIM), 0);
		net_row_pressable(er, NETINFO_ETH);
	}
	// Logging in over SSH as "of": images give it no password (every unit used to have the
	// same one), so this is where an owner sets one, through the helper.
	section(pn, STR_SSH_HEAD);
	lv_obj_t *sr = lv_obj_create(pn);
	lv_obj_remove_style_all(sr);
	lv_obj_set_size(sr, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(sr, 20, 0);
	lv_obj_remove_flag(sr, LV_OBJ_FLAG_SCROLLABLE);
	ssh_status_label = lv_label_create(sr);
	lv_label_set_text(ssh_status_label, "");
	lv_label_set_long_mode(ssh_status_label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(ssh_status_label, 1);
	lv_obj_set_height(ssh_status_label, lv_font_get_line_height(lv_obj_get_style_text_font(ssh_status_label, 0)));
	lv_obj_set_style_text_color(ssh_status_label, lv_color_hex(C_TEXT), 0);
	ssh_detail_label = lv_label_create(sr);
	lv_label_set_text(ssh_detail_label, "");
	lv_obj_set_style_text_color(ssh_detail_label, lv_color_hex(C_DIM), 0);
	lv_obj_t *srow = button_row(pn);
	button(srow, LV_SYMBOL_KEYBOARD "  " STR_SSH_SET_PASSWORD, BUTTON_MAIN, ssh_set_clicked, NULL);
	ssh_off_button = button(srow, STR_SSH_TURN_OFF, BUTTON_OTHER, ssh_off_clicked, NULL);
	lv_obj_add_flag(ssh_off_button, LV_OBJ_FLAG_HIDDEN);

	// Audio: where the sound goes, how loud, and for a DAC whether the OpenFrame sets that
	lv_obj_t *pa_ = settings_panel_new(TAB_AUDIO);
	section(pa_, STR_AUDIO_HEAD_OUTPUT);
	aud_list = lv_obj_create(pa_);
	lv_obj_remove_style_all(aud_list);
	// Its rows' press highlight reaches past the text either side, as a list row's does: the
	// list is as wide as the highlights and moved out to match, so nothing reaches past it (a
	// row reaching past it was clipped by it, square at the ends)
	lv_obj_set_size(aud_list, PANEL_TEXT_W + 2 * LIST_ROW_PAD, LV_SIZE_CONTENT);
	lv_obj_set_style_translate_x(aud_list, -LIST_ROW_PAD, 0);
	lv_obj_set_flex_flow(aud_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(aud_list, LV_OBJ_FLAG_SCROLLABLE);
	aud_dac_head = section(pa_, "");
	lv_obj_add_flag(aud_dac_head, LV_OBJ_FLAG_HIDDEN);
	aud_dac_label = lv_label_create(pa_);
	lv_label_set_text(aud_dac_label, "");
	lv_obj_set_width(aud_dac_label, LV_PCT(100));
	lv_obj_set_style_text_font(aud_dac_label, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(aud_dac_label, lv_color_hex(C_DIM), 0);
	lv_obj_set_style_text_line_space(aud_dac_label, 2, 0);
	lv_obj_add_flag(aud_dac_label, LV_OBJ_FLAG_HIDDEN);
	lv_timer_create(aud_dac_tick, 1000, NULL);
	// on: the audio straight to the DAC as it is (of-audio direct=yes), the OpenFrame's volume
	// control out of its way
	aud_direct_row = switch_row(pa_, STR_AUDIO_DIRECT, false, aud_direct_changed);
	lv_obj_set_style_pad_right(aud_direct_row, BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0); // as Display's switches
	aud_note = lv_label_create(pa_);
	lv_label_set_text(aud_note, "");
	lv_obj_set_width(aud_note, LV_PCT(100));
	lv_obj_set_style_text_font(aud_note, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(aud_note, lv_color_hex(C_DIM), 0);
	// The mixer's rate (of-audio's mixer_rate), while the audio's mixed
	aud_rate_row = switch_row(pa_, STR_AUDIO_RATE, false, aud_rate_changed);
	lv_obj_set_style_pad_right(aud_rate_row, BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0);
	aud_rate_note = lv_label_create(pa_);
	lv_label_set_text(aud_rate_note, STR_AUDIO_RATE_NOTE);
	lv_obj_set_width(aud_rate_note, LV_PCT(100));
	lv_obj_set_style_text_font(aud_rate_note, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(aud_rate_note, lv_color_hex(C_DIM), 0);
	aud_vol_head = lv_obj_create(pa_); // the heading, with the level now at its other end
	lv_obj_remove_style_all(aud_vol_head);
	lv_obj_set_size(aud_vol_head, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(aud_vol_head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(aud_vol_head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(aud_vol_head, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_margin_top(aud_vol_head, HEADING_SPACE, 0);
	lv_obj_set_style_pad_right(aud_vol_head, BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0);
	lv_obj_set_style_margin_top(section(aud_vol_head, STR_AUDIO_HEAD_VOLUME), 0, 0);
	aud_now = section(aud_vol_head, "");
	lv_obj_set_style_margin_top(aud_now, 0, 0);
	aud_slider_row = lv_obj_create(pa_);
	lv_obj_remove_style_all(aud_slider_row);
	lv_obj_set_size(aud_slider_row, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(aud_slider_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(aud_slider_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// room for the knob, held (grown) too, at both ends: nothing beside it, as brightness has
	lv_obj_set_style_pad_hor(aud_slider_row, SLIDER_ROOM(BL_SLIDER_H), 0);
	lv_obj_remove_flag(aud_slider_row, LV_OBJ_FLAG_SCROLLABLE);
	aud_slider = lv_slider_create(aud_slider_row);
	lv_obj_set_height(aud_slider, BL_SLIDER_H);
	lv_obj_set_flex_grow(aud_slider, 1);
	lv_slider_set_range(aud_slider, 0, 100);
	lv_obj_set_ext_click_area(aud_slider, 20);
	lv_obj_add_event_cb(aud_slider, aud_slid, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(aud_slider, aud_released, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(aud_slider, aud_pressed, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(aud_slider, aud_released, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(aud_slider, aud_released, LV_EVENT_INDEV_RESET, NULL);
	lv_obj_t *test_row = button_row(pa_);
	lv_obj_set_flex_align(test_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	aud_test_btn = button(test_row, LV_SYMBOL_AUDIO "  " STR_AUDIO_TEST, BUTTON_OTHER, aud_test_clicked, NULL);
	lv_obj_t *th = lv_label_create(test_row);
	lv_label_set_text(th, STR_AUDIO_TEST_NOTE);
	lv_obj_set_style_text_font(th, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(th, lv_color_hex(C_DIM), 0);
	// Then the DAC's switch and the note, and last the DAC's own section: the volume straight
	// after the outputs, where it's seen without scrolling
	lv_obj_move_to_index(aud_direct_row, -1);
	lv_obj_move_to_index(aud_note, -1);
	lv_obj_move_to_index(aud_rate_row, -1);
	lv_obj_move_to_index(aud_rate_note, -1);
	lv_obj_move_to_index(aud_dac_head, -1);
	lv_obj_move_to_index(aud_dac_label, -1);
	lv_obj_set_style_margin_top(aud_direct_row, HEADING_SPACE / 2, 0);
	lv_obj_set_style_margin_top(aud_rate_row, HEADING_SPACE / 2, 0);
	show_if(aud_rate_row, false);
	show_if(aud_rate_note, false);
	show_if(aud_vol_head, false);
	show_if(aud_slider_row, false);
	show_if(aud_direct_row, false);
	lv_timer_create(aud_watch, 2000, NULL);

	// Display: brightness and touch
	lv_obj_t *pd = settings_panel_new(TAB_DISPLAY);
	// The heading, with the level the backlight has now at its other end (bl_now_tick)
	lv_obj_t *bh = lv_obj_create(pd);
	lv_obj_remove_style_all(bh);
	lv_obj_set_size(bh, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(bh, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bh, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(bh, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *bt = section(bh, STR_DISPLAY_HEAD_BRIGHTNESS);
	lv_obj_set_style_margin_top(bt, 0, 0);
	lv_obj_set_style_margin_top(bh, HEADING_SPACE, 0);
	bl_now = section(bh, "");
	lv_obj_set_style_margin_top(bl_now, 0, 0);
	lv_obj_set_style_pad_right(bh, BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0); // its end over the switches'
	lv_timer_create(bl_now_tick, 250, NULL);
	lv_obj_t *row = lv_obj_create(pd);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 24, 0);
	// room for the slider's knob, held, at the left end (its right end has the gap to Auto),
	// and the row's right end in line with the switches below
	lv_obj_set_style_pad_left(row, SLIDER_ROOM(BL_SLIDER_H), 0);
	lv_obj_set_style_pad_right(row, BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0);
	bl_slider = lv_slider_create(row);
	lv_obj_set_height(bl_slider, BL_SLIDER_H);
	lv_obj_set_flex_grow(bl_slider, 1);
	lv_slider_set_range(bl_slider, 1, 100);
	lv_obj_set_ext_click_area(bl_slider, 20); // easier to grab with a finger
	lv_obj_add_event_cb(bl_slider, bl_released, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(bl_slider, bl_released, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(bl_slider, bl_released, LV_EVENT_INDEV_RESET, NULL);
	lv_obj_add_event_cb(bl_slider, bl_pressed, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(bl_slider, bl_dragged, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_t *al = lv_label_create(row);
	lv_label_set_text(al, STR_DISPLAY_AUTO);
	lv_obj_set_style_text_font(al, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(al, lv_color_hex(C_TEXT), 0);
	bl_auto = lv_switch_create(row);
	lv_obj_set_size(bl_auto, 72, 38);
	lv_obj_add_event_cb(bl_auto, bl_auto_changed, LV_EVENT_VALUE_CHANGED, NULL);
	bl_hint = lv_label_create(pd);
	lv_label_set_text(bl_hint, STR_DISPLAY_AUTO_NOTE);
	lv_obj_set_width(bl_hint, LV_PCT(100));
	lv_obj_set_style_margin_top(bl_hint, -HINT_GAP, 0);
	lv_obj_set_style_text_font(bl_hint, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(bl_hint, lv_color_hex(C_DIM), 0);
	lv_obj_add_flag(bl_hint, LV_OBJ_FLAG_HIDDEN);
	bl_off_row = switch_row(pd, STR_DISPLAY_DARK_OFF, true, bl_off_changed);
	lv_obj_set_style_pad_right(bl_off_row, BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0); // its switch under Auto's (that row's inset for the knob)
	lv_obj_add_flag(bl_off_row, LV_OBJ_FLAG_HIDDEN);
	idle_build(pd);
	saver_build(pd);
	// The clock's format (and, later, the time zone): after brightness, before touch (Time had
	// a tab of its own, until Audio needed it)
	section(pd, STR_DISPLAY_HEAD_CLOCK);
	// their switches under Auto's, as the screen-off one is
	lv_obj_set_style_pad_right(switch_row(pd, STR_DISPLAY_CLOCK24, clock_24, clock24_changed), BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0);
	lv_obj_set_style_pad_right(switch_row(pd, STR_DISPLAY_SECONDS, clock_secs, seconds_changed), BL_SLIDER_H / 2 + SLIDER_KNOB_PAD, 0);

	section(pd, STR_DISPLAY_HEAD_TOUCH);
	lv_obj_t *trow = button_row(pd);
	button(trow, LV_SYMBOL_EDIT "  " STR_DISPLAY_CALIBRATE, BUTTON_MAIN, cal_start, NULL);
	button(trow, LV_SYMBOL_REFRESH "  " STR_DISPLAY_CALIBRATE_RESET, BUTTON_OTHER, cal_reset, NULL);

	// Updates: what of-update's doing, and asking for a look, or for the updates, now
	lv_obj_t *pu = settings_panel_new(TAB_UPDATES);
	section(pu, STR_UPD_HEAD);
	lv_obj_t *ur = lv_obj_create(pu);
	lv_obj_remove_style_all(ur);
	lv_obj_set_size(ur, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(ur, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(ur, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(ur, 20, 0);
	lv_obj_remove_flag(ur, LV_OBJ_FLAG_SCROLLABLE);
	upd_status_label = lv_label_create(ur);
	lv_label_set_text(upd_status_label, "");
	lv_label_set_long_mode(upd_status_label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(upd_status_label, 1);
	lv_obj_set_height(upd_status_label, lv_font_get_line_height(lv_obj_get_style_text_font(upd_status_label, 0)));
	lv_obj_set_style_text_color(upd_status_label, lv_color_hex(C_TEXT), 0);
	upd_detail_label = lv_label_create(ur);
	lv_label_set_text(upd_detail_label, "");
	lv_obj_set_style_text_color(upd_detail_label, lv_color_hex(C_DIM), 0);
	upd_bar = lv_bar_create(pu); // while downloading or installing
	lv_obj_set_size(upd_bar, LV_PCT(100), 8);
	lv_bar_set_range(upd_bar, 0, 100);
	lv_obj_set_style_radius(upd_bar, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_radius(upd_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(upd_bar, lv_color_hex(C_BUTTON_OTHER), 0);
	lv_obj_set_style_bg_opa(upd_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(upd_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
	lv_obj_add_flag(upd_bar, LV_OBJ_FLAG_HIDDEN);
	upd_note_label = lv_label_create(pu);
	lv_label_set_text(upd_note_label, "");
	lv_obj_set_width(upd_note_label, LV_PCT(100));
	lv_obj_set_style_text_color(upd_note_label, lv_color_hex(C_WARN), 0);
	lv_obj_add_flag(upd_note_label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_t *ub = upd_buttons = button_row(pu);
	upd_now_btn = button(ub, LV_SYMBOL_DOWNLOAD "  " STR_UPD_UPDATE_NOW, BUTTON_MAIN, upd_now_clicked, NULL);
	upd_restart_btn = button(ub, LV_SYMBOL_REFRESH "  " STR_UPD_RESTART_NOW, BUTTON_MAIN, power_clicked, "reboot");
	upd_check_btn = button(ub, LV_SYMBOL_REFRESH "  " STR_UPD_CHECK_NOW, BUTTON_OTHER, upd_check_clicked, NULL);
	lv_obj_add_flag(upd_now_btn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(upd_restart_btn, LV_OBJ_FLAG_HIDDEN);
	upd_list = lv_obj_create(pu); // what's waiting (upd_show_list)
	lv_obj_remove_style_all(upd_list);
	lv_obj_set_height(upd_list, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(upd_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(upd_list, 8, 0);
	lv_obj_set_style_margin_top(upd_list, HEADING_SPACE, 0);
	lv_obj_remove_flag(upd_list, LV_OBJ_FLAG_SCROLLABLE);
	// Its rows' highlights reach LIST_ROW_PAD past the text (row_pressable), so the list
	// reaches that far too, its padding keeping everything else where it was: it would clip
	// them otherwise.
	lv_obj_set_width(upd_list, PANEL_TEXT_W + 2 * LIST_ROW_PAD);
	lv_obj_set_style_margin_left(upd_list, -LIST_ROW_PAD, 0);
	lv_obj_set_style_margin_right(upd_list, -LIST_ROW_PAD, 0);
	lv_obj_set_style_pad_hor(upd_list, LIST_ROW_PAD, 0);
	lv_obj_add_flag(upd_list, LV_OBJ_FLAG_HIDDEN);
	section(pu, STR_UPD_HEAD_SCHEDULE);
	lv_obj_t *tr = lv_obj_create(pu); // the update time, opening upd_time_clicked's picker
	lv_obj_remove_style_all(tr);
	lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(tr, 20, 0);
	lv_obj_remove_flag(tr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *tn = lv_label_create(tr);
	lv_label_set_text(tn, STR_UPD_TIME);
	lv_obj_set_style_text_color(tn, lv_color_hex(C_TEXT), 0);
	lv_obj_set_flex_grow(tn, 1);
	upd_time_label = lv_label_create(tr);
	lv_obj_set_style_text_color(upd_time_label, lv_color_hex(C_DIM), 0);
	lv_obj_set_style_text_font(upd_time_label, &lv_font_montserrat_16, 0);
	upd_hour_set(upd_hour);
	row_pressable(tr, upd_time_clicked, NULL);
	lv_obj_t *uh = lv_label_create(pu);
	lv_label_set_text(uh, STR_UPD_TIME_NOTE);
	lv_obj_set_width(uh, LV_PCT(100));
	lv_obj_set_style_margin_top(uh, HINT_GAP, 0);
	lv_obj_set_style_text_font(uh, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(uh, lv_color_hex(C_DIM), 0);

	// About
	lv_obj_t *pa = settings_panel_new(TAB_ABOUT);
	// Its name (name.txt), and the hostname made from it
	section(pa, STR_NAME_HEAD);
	lv_obj_t *nr = lv_obj_create(pa);
	lv_obj_remove_style_all(nr);
	lv_obj_set_flex_flow(nr, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(nr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(nr, 20, 0);
	lv_obj_remove_flag(nr, LV_OBJ_FLAG_SCROLLABLE);
	name_label = lv_label_create(nr);
	lv_label_set_text(name_label, "");
	lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_color(name_label, lv_color_hex(C_TEXT), 0);
	lv_obj_set_flex_grow(name_label, 1);
	lv_obj_set_height(name_label, lv_font_get_line_height(lv_obj_get_style_text_font(name_label, 0)));
	name_host_label = lv_label_create(nr);
	lv_label_set_text(name_host_label, "");
	lv_obj_set_style_text_color(name_host_label, lv_color_hex(C_DIM), 0);
	lv_obj_set_style_text_font(name_host_label, &lv_font_montserrat_16, 0);
	row_pressable(nr, name_clicked, NULL);
	name_refresh();
	section(pa, STR_ABOUT_HEAD);
	about_build(pa);
	// Erasing it for its next owner (openframe-scripts' of-reset), asked twice
	section(pa, STR_ERASE_HEAD);
	lv_obj_t *el = lv_label_create(pa);
	lv_label_set_text(el, STR_ERASE_NOTE);
	lv_obj_set_width(el, LV_PCT(100));
	lv_obj_set_style_text_font(el, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(el, lv_color_hex(C_DIM), 0);
	lv_obj_t *erow = button_row(pa);
	button(erow, LV_SYMBOL_TRASH "  " STR_ERASE_BUTTON, BUTTON_DANGER, reset_ask, NULL);
	// Information: what sprite-ping sends (system/sprite-ping), said where an owner can find it
	section(pa, STR_INFO_HEAD);
	lv_obj_t *tl = lv_label_create(pa);
	lv_label_set_text(tl, STR_INFO_PING);
	lv_obj_set_width(tl, LV_PCT(100));
	lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(tl, lv_color_hex(C_DIM), 0);

	// Power
	lv_obj_t *pp = settings_panel_new(TAB_POWER);
	section(pp, STR_POWER_HEAD);
	lv_obj_t *prow = button_row(pp);
	button(prow, LV_SYMBOL_REFRESH "  " STR_POWER_RESTART, BUTTON_OTHER, power_clicked, "reboot");
	button(prow, LV_SYMBOL_POWER "  " STR_POWER_OFF, BUTTON_DANGER, power_clicked, "poweroff");

	// the first heading of each panel sits at its top, not a heading's space below it
	for (int t = 0; t < N_TABS; t++) lv_obj_set_style_margin_top(lv_obj_get_child(tab_panel[t], 0), 0, 0);
	lv_obj_set_style_margin_top(lv_obj_get_child(pn, 1), 0, 0); // under the notes, while there are none
	settings_show_tab(settings_tab_first);
	update_net(NULL); // the Network tab's dot, now there is one
	lv_timer_create(refresh_settings, 3000, NULL);
	lv_obj_add_flag(page_settings, LV_OBJ_FLAG_HIDDEN);
}

// sprite's rules over LVGL's default theme: its buttons and message boxes follow the radius
// rule as they're sized, so nothing made later can drift from it.
static lv_theme_t theme;

// A slider of the shell's being held: main.c then holds a lift longer before it's a release
// (SPRITE_SLIDER_HOLD_MS), so a finger losing contact as it works it isn't letting go.
static bool slider_held;
static void slider_held_cb(lv_event_t *e) { slider_held = lv_event_get_user_data(e) != NULL; }
uint32_t shell_release_hold_ms(void) { return slider_held ? SPRITE_SLIDER_HOLD_MS : SPRITE_DEBOUNCE_MS; }

// A slider's extra drawing room (beyond its edges) as big as its knob held: LVGL works it out
// from the knob as it is, at rest, and a held knob, SLIDER_KNOB_GROW bigger all round, was
// clipped at the ends.
static void slider_ext_cb(lv_event_t *e) {
	lv_obj_t *o = lv_event_get_target(e);
	int32_t *ext = lv_event_get_param(e);
	int32_t knob = LV_MIN(lv_obj_get_width(o), lv_obj_get_height(o)) / 2 + SLIDER_KNOB_PAD + SLIDER_KNOB_GROW + 2;
	if (*ext < knob) *ext = knob;
}

static void theme_apply(lv_theme_t *th, lv_obj_t *obj) {
	if (lv_obj_check_type(obj, &lv_button_class) || lv_obj_check_type(obj, &lv_msgbox_class))
		lv_obj_add_event_cb(obj, radius_on_size, LV_EVENT_SIZE_CHANGED, NULL);
	// A slider's knobs white, as a switch's is, so they stand out from the bar, and a known
	// size past it (SLIDER_KNOB_PAD), so what holds a slider can leave room for them.
	if (lv_obj_check_type(obj, &lv_slider_class)) {
		lv_obj_add_event_cb(obj, slider_held_cb, LV_EVENT_PRESSED, (void *)1);
		lv_obj_add_event_cb(obj, slider_held_cb, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(obj, slider_held_cb, LV_EVENT_PRESS_LOST, NULL);
		// and when a drag that starts on it is taken as a scroll (drag_input's lv_indev_reset:
		// LVGL sends only this then, no release), or it stayed held: taps 0.4 s late after
		lv_obj_add_event_cb(obj, slider_held_cb, LV_EVENT_INDEV_RESET, NULL);
		lv_obj_add_event_cb(obj, slider_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
		static lv_style_t knob;
		if (!knob.prop_cnt) {
			lv_style_init(&knob);
			lv_style_set_bg_color(&knob, lv_color_white());
			lv_style_set_bg_opa(&knob, LV_OPA_COVER);
			lv_style_set_pad_all(&knob, SLIDER_KNOB_PAD);
		}
		static lv_style_t held; // in place of the theme's grow, so its size is known here
		if (!held.prop_cnt) {
			lv_style_init(&held);
			lv_style_set_transform_width(&held, SLIDER_KNOB_GROW);
			lv_style_set_transform_height(&held, SLIDER_KNOB_GROW);
		}
		lv_obj_add_style(obj, &knob, LV_PART_KNOB);
		lv_obj_add_style(obj, &held, LV_PART_KNOB | LV_STATE_PRESSED);
	}
}

/* ---- the welcome: once, at the first start of a new system (or one that's been erased) ---- */

// Marked done in sprite's state directory (welcomed), which of-reset empties.
static void welcome_done(void) {
	FILE *f = fopen(state_path("welcomed"), "w");
	if (f) fclose(f);
	upd_time_save_hour(time_pick); // the time picked on it, if it's not the one there was
}

static void welcome_start(lv_event_t *e) {
	welcome_done();
	dialog_close();
}

static void welcome_wifi(lv_event_t *e) {
	welcome_done();
	dialog_close();
	settings_show_tab(TAB_NETWORK);
	show_page(page_settings);
	refresh_settings(NULL);
	wifi_choose_clicked(NULL);
}

static void welcome_password(lv_event_t *e) {
	welcome_done();
	dialog_close();
	settings_show_tab(TAB_NETWORK);
	show_page(page_settings);
	refresh_settings(NULL);
	ssh_set_clicked(NULL);
}

// A row on the welcome card: a name, a dim value at the right, and a chevron (row_pressable).
static lv_obj_t *welcome_row(lv_obj_t *card, const char *name, const char *value, lv_event_cb_t cb) {
	lv_obj_t *r = lv_obj_create(card);
	lv_obj_remove_style_all(r);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 20, 0);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *n = lv_label_create(r);
	lv_label_set_text(n, name);
	lv_obj_set_style_text_color(n, lv_color_hex(C_TEXT), 0);
	lv_obj_set_flex_grow(n, 1);
	if (value) {
		lv_obj_t *v = lv_label_create(r);
		lv_label_set_text(v, value);
		lv_obj_set_style_text_color(v, lv_color_hex(C_DIM), 0);
		lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
	}
	if (cb) row_pressable(r, cb, NULL);
	else lv_obj_set_size(r, PANEL_TEXT_W, ROW_H);
	return r;
}

static void welcome_show(lv_timer_t *t) {
	if (dialog_backdrop || !showing(page_home)) return; // something else is in front: next start
	lv_obj_t *card = dialog_open(NULL, STR_WELCOME_TITLE,
				     STR_WELCOME_NOTE, false);
	lv_obj_set_width(card, PANEL_TEXT_W + 2 * DIALOG_PAD);
	lv_obj_set_style_pad_row(card, 10, 0);
	lv_obj_set_style_text_font(lv_obj_get_child(card, 1), &lv_font_montserrat_16, 0);
	struct sprite_net n;
	sysinfo_net(&n);
	welcome_row(card, STR_WELCOME_WIFI, n.wifi_up ? STR_WELCOME_CONNECTED : n.eth_up ? STR_WELCOME_CABLE : STR_WIFI_NOT_CONNECTED, welcome_wifi);
	welcome_row(card, STR_WELCOME_PASSWORD, STR_WELCOME_OPTIONAL, welcome_password);
	lv_obj_t *ur = welcome_row(card, STR_WELCOME_UPDATES_AT, NULL, NULL);
	time_stepper(ur);
	lv_obj_t *h = lv_label_create(card);
	lv_label_set_text(h, STR_WELCOME_UPDATES_AT_NOTE);
	lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(h, LV_PCT(100));
	lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(h, lv_color_hex(C_DIM), 0);
	lv_obj_set_style_margin_bottom(h, 8, 0);
	dialog_button(card, STR_WELCOME_START, BUTTON_MAIN, welcome_start, NULL);
}

/* ---- updating Sprite itself: its package's postinst says so (/run/sprite-updating, with the
   new version) a few seconds before it restarts Sprite, so it doesn't just vanish ---- */

#define UPDATING_FILE "/run/sprite-updating"

static void updating_check(void) {
	static bool shown;
	if (shown || access(UPDATING_FILE, F_OK)) return;
	shown = true;
	char ver[48] = "";
	FILE *f = fopen(UPDATING_FILE, "r");
	if (f) { if (fgets(ver, sizeof ver, f)) ver[strcspn(ver, "\n")] = 0; fclose(f); }
	// where to come back to: the Updates tab, if that's what's in front (state directory: it
	// outlasts the restart, as /run/user doesn't)
	f = fopen(state_path("resume"), "w");
	if (f) {
		bool at_updates = (showing(page_settings) && settings_tab == TAB_UPDATES) || showing(page_news);
		fprintf(f, "%s %s\n", at_updates ? "updates" : "home", ver[0] ? ver : "-");
		fclose(f);
	}
	char body[160];
	// (the version first: LVGL breaks lines at full stops, and would split it)
	if (ver[0]) snprintf(body, sizeof body, STR_UPD_SPRITE_RESTARTS, ver);
	else snprintf(body, sizeof body, STR_UPD_SPRITE_RESTARTS_NOVER);
	dialog_open(NULL, STR_UPD_SPRITE_UPDATED, body, false);
	sprite_lvgl_kick();
}

// After that restart: back where it was, and a word that it worked.
static void updating_resume(void) {
	const char *p = state_path("resume");
	struct stat st;
	if (stat(p, &st)) return;
	char where[16] = "", ver[48] = "";
	FILE *f = fopen(p, "r");
	if (f) { if (fscanf(f, "%15s %47s", where, ver) < 1) where[0] = 0; fclose(f); }
	unlink(p);
	if (time(NULL) - st.st_mtime > 120) return; // not this restart's
	if (!strcmp(where, "updates")) {
		settings_show_tab(TAB_UPDATES);
		show_page(page_settings);
	}
	char t[80];
	snprintf(t, sizeof t, strcmp(ver, "-") && ver[0] ? STR_UPD_SPRITE_IS_NOW : STR_UPD_SPRITE_UPDATED, ver);
	show_toast(t, 5000);
}

void shell_init(void *pixels) {
	lv_init();
	lv_tick_set_cb(tick_ms);
	disp = lv_display_create(SPRITE_W, SPRITE_H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, pixels, NULL, SPRITE_W * SPRITE_H * 4, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);
	// Drawing happens in step with the display (shell_frame, from the compositor's frame event),
	// not on LVGL's own timer, which would fire out of step with the refresh. The timer stays,
	// as lv_refr_now uses it, but never comes due.
	lv_timer_set_period(lv_display_get_refr_timer(disp), 1000000);
	lv_display_add_event_cb(disp, invalidated, LV_EVENT_INVALIDATE_AREA, NULL);
	lv_display_add_event_cb(disp, refr_event, LV_EVENT_RENDER_START, NULL);
	lv_display_add_event_cb(disp, refr_event, LV_EVENT_RENDER_READY, NULL);
	lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(C_ACCENT), lv_color_hex(0x45d483), true,
					       &lv_font_montserrat_20);
	theme = *th;
	lv_theme_set_parent(&theme, th);
	lv_theme_set_apply_cb(&theme, theme_apply);
	lv_display_set_theme(disp, &theme);

	indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, indev_read);
	lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT); // read when the compositor has input, not on a timer
	lv_indev_set_scroll_limit(indev, 24); // above PAGE_DRAG_START: sprite's drags take over first

	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	build_bar(scr);
	build_home(scr);
	build_settings(scr);
	build_wifi(scr);
	build_netinfo(scr);
	build_news(scr);
	build_entry(scr);
	screensaver_build();
	updating_resume();
	if (access(state_path("welcomed"), F_OK)) {
		lv_timer_t *w = lv_timer_create(welcome_show, 1500, NULL);
		lv_timer_set_repeat_count(w, 1);
	}
}
