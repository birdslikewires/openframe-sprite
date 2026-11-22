// SPDX-License-Identifier: GPL-3.0-or-later
// The shell: status bar, home screen of app tiles, settings. LVGL, rendered in software
// into the buffer the compositor shows whenever no app is on screen.
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "lvgl/lvgl.h"
#include "sprite.h"

#define BAR_H 40
// The home grid: 6 columns by 3 rows of apps, as OpenPeak's own home screen had. Each app has a
// cell (the whole cell takes the touch) with an icon and a label of up to two lines below it.
#define GRID_COLS 6
#define GRID_ROWS 3
#define GRID_PAD_X 16                                       // 16 + 6 x 128 + 16 = 800
#define GRID_PAD_TOP 14                                     // 40 + 14 + 3 x 140 = 474 of 480
#define TILE_W ((SPRITE_W - 2 * GRID_PAD_X) / GRID_COLS)  // 128
#define TILE_H 140
#define ICON_SIZE 88
#define ICON_RADIUS 18
#define ICON_TOP 6
#define LABEL_GAP 6
#define LABEL_H 38                                          // two lines of 16 px Montserrat
#define PER_PAGE (GRID_COLS * GRID_ROWS)                    // further apps go on further pages

#define C_BG 0x0e1217
#define C_BAR 0x070a0d
#define C_TEXT 0xe8edf2
#define C_DIM 0x8b97a3
#define C_ACCENT 0x3d9be9

static lv_display_t *disp;
static lv_indev_t *indev;
static int ptr_x, ptr_y;
static bool ptr_pressed;
static lv_obj_t *bar_clock, *bar_date, *bar_net, *bar_title;
static lv_obj_t *page_home, *pager, *strip, *page_settings, *toast;
static lv_obj_t *dots, *gear;         // page dots and the Settings gear, in the status bar
static int cur_page, n_pages;

// Paging: the pages sit side by side on a strip, which follows a sideways drag and then
// slides to a page. A touch becomes a page drag once it has moved PAGE_DRAG_START sideways
// (until then it can still be a tap or long press on an app); it turns the page if it went
// PAGE_TURN_PX, or was moving at PAGE_FLICK_SPEED when it lifted.
#define PAGE_DRAG_START 12
#define PAGE_TURN_PX 60
#define PAGE_FLICK_SPEED 0.4       // px/ms
#define PAGE_SLIDE_MS 180
static enum { DRAG_NONE, DRAG_MAYBE, DRAG_PAGING } drag;
static int drag_x0, drag_y0;
static void pager_input(int x, int y, bool pressed, bool was_pressed);
static lv_obj_t *bl_slider, *bl_auto, *about_label;
static bool visible = true, rebuild_pending;
static struct sprite_app *opening;
static lv_timer_t *toast_timer;
static lv_timer_t *hold_timer;        // polls input while a finger is down (long press)
static lv_obj_t *sheet;               // the long-press sheet for one app
static struct sprite_app *sheet_app;
static void sheet_dismiss(void);

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

static void refr_event(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_RENDER_START) render_start = now_ms();
	else perf_render(now_ms() - render_start);
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
	// While paging, the frames move the strip, from interpolated positions (shell_frame)
	if (!(drag == DRAG_PAGING && pressed && ptr_pressed)) pager_input(x, y, pressed, ptr_pressed);
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
		// Page drags carry on at the finger's last speed past its last report, for at most
		// SPRITE_DEBOUNCE_MS: a lifting finger's release is held that long (it may be a
		// contact bounce), and the strip shouldn't stop dead while it waits.
		double kmax = drag == DRAG_PAGING ? 1 + SPRITE_DEBOUNCE_MS / (smp[1].t - smp[0].t) : 1;
		if (k > kmax) k = kmax;
		ptr_x = (int)(smp[0].x + (smp[1].x - smp[0].x) * k + 0.5);
		ptr_y = (int)(smp[0].y + (smp[1].y - smp[0].y) * k + 0.5);
		if (drag == DRAG_PAGING) pager_input(ptr_x, ptr_y, true, true); // the strip, smoothly
		else lv_indev_read(indev);
		if (k < kmax) sprite_request_frame(); // still catching up, or carrying on
	}
	lv_refr_now(disp);
}

// Anything LVGL marks for redrawing asks for the next frame.
static void invalidated(lv_event_t *e) {
	if (visible) sprite_request_frame();
}

void shell_set_visible(bool v) {
	visible = v;
	if (v) sprite_request_frame();
	if (!v) {
		ptr_pressed = false;
		lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
		if (toast_timer) { lv_timer_delete(toast_timer); toast_timer = NULL; }
		if (hold_timer) { lv_timer_delete(hold_timer); hold_timer = NULL; }
		opening = NULL;
		sheet_dismiss();
	}
}

/* ---- icons ---- */

static const char *icon_text(const struct sprite_app *a, char *buf, size_t len) {
	static const struct { const char *k, *sym; } map[] = {
		{ "audio", LV_SYMBOL_AUDIO }, { "music", LV_SYMBOL_AUDIO }, { "video", LV_SYMBOL_VIDEO },
		{ "image", LV_SYMBOL_IMAGE }, { "photo", LV_SYMBOL_IMAGE }, { "settings", LV_SYMBOL_SETTINGS },
		{ "home", LV_SYMBOL_HOME }, { "gps", LV_SYMBOL_GPS }, { "map", LV_SYMBOL_GPS },
		{ "wifi", LV_SYMBOL_WIFI }, { "keyboard", LV_SYMBOL_KEYBOARD }, { "terminal", LV_SYMBOL_KEYBOARD },
		{ "eye", LV_SYMBOL_EYE_OPEN }, { "clock", LV_SYMBOL_BELL }, { "bell", LV_SYMBOL_BELL },
		{ "list", LV_SYMBOL_LIST }, { "file", LV_SYMBOL_FILE }, { "play", LV_SYMBOL_PLAY },
		{ "refresh", LV_SYMBOL_REFRESH }, { "touch", LV_SYMBOL_EDIT }, { "power", LV_SYMBOL_POWER },
		{ "radio", LV_SYMBOL_VOLUME_MAX }, { "chart", LV_SYMBOL_BARS },
	};
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
		if (!strcasecmp(a->icon, map[i].k)) return map[i].sym;
	const char *src = a->icon[0] ? a->icon : a->name;
	snprintf(buf, len, "%.2s", src);
	if (!a->icon[0]) buf[1] = 0; // one initial from the name
	return buf;
}

/* ---- status bar ---- */

static void update_clock(lv_timer_t *t) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char s[32];
	strftime(s, sizeof s, "%H:%M", &tm);
	lv_label_set_text(bar_clock, s);
	strftime(s, sizeof s, "%a %e %b", &tm);
	lv_label_set_text(bar_date, s);
}

static void update_net(lv_timer_t *t) {
	struct sprite_net n;
	sysinfo_net(&n);
	char s[96];
	if (n.wifi_ip[0] && n.wifi_quality >= 0)
		snprintf(s, sizeof s, "%s  " LV_SYMBOL_WIFI " %d%%", n.eth_ip[0] ? "LAN" : "", n.wifi_quality);
	else if (n.eth_ip[0])
		snprintf(s, sizeof s, "LAN %s", n.eth_ip);
	else if (n.wifi_up || n.eth_up)
		snprintf(s, sizeof s, LV_SYMBOL_WARNING " No address");
	else
		snprintf(s, sizeof s, LV_SYMBOL_CLOSE " Offline");
	lv_label_set_text(bar_net, s);
}

static void gear_clicked(lv_event_t *e);

static void build_bar(lv_obj_t *scr) {
	lv_obj_t *bar = lv_obj_create(scr);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, SPRITE_W, BAR_H);
	lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

	bar_clock = lv_label_create(bar);
	lv_obj_set_style_text_font(bar_clock, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(bar_clock, lv_color_hex(C_TEXT), 0);
	lv_obj_align(bar_clock, LV_ALIGN_LEFT_MID, 16, 0);

	bar_date = lv_label_create(bar);
	lv_obj_set_style_text_font(bar_date, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(bar_date, lv_color_hex(C_DIM), 0);
	lv_obj_align(bar_date, LV_ALIGN_LEFT_MID, 84, 1);

	bar_title = lv_label_create(bar);
	lv_obj_set_style_text_font(bar_title, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(bar_title, lv_color_hex(C_DIM), 0);
	lv_label_set_text(bar_title, "");
	lv_obj_align(bar_title, LV_ALIGN_CENTER, 0, 0);

	dots = lv_obj_create(bar);
	lv_obj_remove_style_all(dots);
	lv_obj_set_size(dots, LV_SIZE_CONTENT, BAR_H);
	lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(dots, 14, 0);
	lv_obj_align(dots, LV_ALIGN_CENTER, 0, 0);
	lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);

	// Settings: a gear at the end of the bar, the whole bar's height to touch
	gear = lv_obj_create(bar);
	lv_obj_remove_style_all(gear);
	lv_obj_set_size(gear, 48, BAR_H);
	lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -4, 0);
	lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(gear, lv_color_white(), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(gear, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_set_style_radius(gear, 8, 0);
	lv_obj_t *gl = lv_label_create(gear);
	lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
	lv_obj_set_style_text_font(gl, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(gl, lv_color_hex(C_TEXT), 0);
	lv_obj_center(gl);
	lv_obj_add_event_cb(gear, gear_clicked, LV_EVENT_CLICKED, NULL);

	bar_net = lv_label_create(bar);
	lv_obj_set_style_text_font(bar_net, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(bar_net, lv_color_hex(C_TEXT), 0);
	lv_obj_align(bar_net, LV_ALIGN_RIGHT_MID, -60, 0);

	update_clock(NULL);
	update_net(NULL);
	lv_timer_create(update_clock, 1000, NULL);
	lv_timer_create(update_net, 5000, NULL);
}

/* ---- pages ---- */

static void update_dots(void);

static void show_page(lv_obj_t *page) {
	lv_obj_add_flag(page_home, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(page_settings, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(bar_title, page == page_settings ? "Settings" : "");
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
	if (toast_timer) lv_timer_delete(toast_timer);
	toast_timer = lv_timer_create(toast_expire, ms, NULL);
	lv_timer_set_repeat_count(toast_timer, 1);
}

void shell_notice(const char *text) {
	show_toast(text, 6000);
	sprite_lvgl_kick();
}

static void refresh_settings(lv_timer_t *t) {
	if (lv_obj_has_flag(page_settings, LV_OBJ_FLAG_HIDDEN) || !visible) return;
	static char text[768];
	sysinfo_summary(text, sizeof text);
	lv_label_set_text(about_label, text);
	bool autobl = sysinfo_backlight_auto();
	if (autobl) lv_obj_add_state(bl_auto, LV_STATE_CHECKED);
	else lv_obj_remove_state(bl_auto, LV_STATE_CHECKED);
	if (!lv_obj_has_state(bl_slider, LV_STATE_PRESSED)) {
		int b = sysinfo_backlight();
		if (b >= 0) lv_slider_set_value(bl_slider, b, LV_ANIM_OFF);
	}
}

static void open_app(struct sprite_app *a) {
	if (a->builtin) {
		show_page(page_settings);
		refresh_settings(NULL);
		return;
	}
	if (!a->pid && !a->pending) {
		char s[96];
		snprintf(s, sizeof s, "Opening %s...", a->name);
		show_toast(s, 20000);
		opening = a;
	}
	sprite_open_app(a);
}

static void tile_clicked(lv_event_t *e) { open_app(lv_event_get_user_data(e)); }

// The gear opens Settings, and closes it again.
static void gear_clicked(lv_event_t *e) {
	if (!lv_obj_has_flag(page_settings, LV_OBJ_FLAG_HIDDEN)) {
		show_page(page_home);
		return;
	}
	show_page(page_settings);
	refresh_settings(NULL);
}

/* ---- long-press sheet: open, close, open at startup ---- */

static void sheet_dismiss(void) {
	if (!sheet) return;
	lv_obj_delete_async(sheet);
	sheet = NULL;
	sheet_app = NULL;
}

static void sheet_backdrop(lv_event_t *e) {
	if (lv_event_get_target(e) == lv_event_get_current_target(e)) sheet_dismiss(); // a tap outside the card
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
	snprintf(s, sizeof s, "Closing %s...", a->name);
	show_toast(s, 4000);
	sprite_close_app(a);
}

static void sheet_startup(lv_event_t *e) {
	apps_set_autostart(sheet_app, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	shell_apps_changed();
}

static lv_obj_t *sheet_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_size(b, 180, 56);
	lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

static void open_sheet(struct sprite_app *a) {
	if (a->builtin || sheet) return;
	sheet_app = a;
	sheet = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(sheet);
	lv_obj_set_size(sheet, SPRITE_W, SPRITE_H);
	lv_obj_set_style_bg_color(sheet, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(sheet, LV_OPA_60, 0);
	lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(sheet, sheet_backdrop, LV_EVENT_CLICKED, NULL);

	lv_obj_t *card = lv_obj_create(sheet);
	lv_obj_set_size(card, 460, LV_SIZE_CONTENT);
	lv_obj_center(card);
	lv_obj_set_style_bg_color(card, lv_color_hex(0x1c242c), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_radius(card, 22, 0);
	lv_obj_set_style_pad_all(card, 24, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text(title, a->name);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 20, 0);
	sheet_button(row, LV_SYMBOL_PLAY "  Open", C_ACCENT, sheet_open);
	if (a->pid || a->pending) sheet_button(row, LV_SYMBOL_CLOSE "  Close", 0xb3403a, sheet_close_app);

	lv_obj_t *srow = lv_obj_create(card);
	lv_obj_remove_style_all(srow);
	lv_obj_set_size(srow, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *text = lv_obj_create(srow);
	lv_obj_remove_style_all(text);
	lv_obj_set_size(text, 300, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_t *l1 = lv_label_create(text);
	lv_label_set_text(l1, "Open at startup");
	lv_obj_set_style_text_color(l1, lv_color_hex(C_TEXT), 0);
	lv_obj_t *l2 = lv_label_create(text);
	lv_label_set_text(l2, "Opens when the OpenFrame starts, and again if it closes.");
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

static void tile_long_pressed(lv_event_t *e) { open_sheet(lv_event_get_user_data(e)); }

/* ---- the home grid, in pages ---- */

// An app's icon: a rounded square of its colour, with a white glyph.
static lv_obj_t *make_badge(lv_obj_t *cell, const struct sprite_app *a) {
	lv_obj_t *badge = lv_obj_create(cell);
	lv_obj_remove_style_all(badge);
	lv_obj_set_size(badge, ICON_SIZE, ICON_SIZE);
	lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, ICON_TOP);
	lv_obj_set_style_radius(badge, ICON_RADIUS, 0);
	lv_obj_set_style_bg_color(badge, lv_color_hex(a->color), 0);
	lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
	lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	char buf[8];
	lv_obj_t *icon = lv_label_create(badge);
	lv_label_set_text(icon, icon_text(a, buf, sizeof buf));
	lv_obj_set_style_text_font(icon, &lv_font_montserrat_40, 0);
	lv_obj_set_style_text_color(icon, lv_color_white(), 0);
	lv_obj_center(icon);
	return badge;
}

static void make_cell(lv_obj_t *page, struct sprite_app *a) {
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
		lv_obj_set_style_bg_color(dot, lv_color_hex(a->view ? 0x45d483 : 0xf2b33d), 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(dot, lv_color_white(), 0);
		lv_obj_set_style_border_width(dot, 2, 0);
		lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -6, 6);
	}

	if (a == apps_autostart()) { // the app that opens at startup: a small power badge
		lv_obj_t *mark = lv_label_create(badge);
		lv_label_set_text(mark, LV_SYMBOL_POWER);
		lv_obj_set_style_text_font(mark, &lv_font_montserrat_14, 0);
		lv_obj_set_style_text_color(mark, lv_color_white(), 0);
		lv_obj_set_style_bg_color(mark, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(mark, LV_OPA_40, 0);
		lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_pad_all(mark, 4, 0);
		lv_obj_align(mark, LV_ALIGN_BOTTOM_LEFT, 5, -5);
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
	lv_obj_set_style_radius(tile, ICON_RADIUS, 0);
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

static void dot_clicked(lv_event_t *e) {
	go_to_page((int)(intptr_t)lv_event_get_user_data(e), true);
}

// Called with each position the shell is given, before LVGL sees it.
static void pager_input(int x, int y, bool pressed, bool was_pressed) {
	bool home = pager && !lv_obj_has_flag(page_home, LV_OBJ_FLAG_HIDDEN) && !sheet && !cal && n_pages > 1;
	if (pressed && !was_pressed) { // a touch begins
		drag = home && y >= BAR_H ? DRAG_MAYBE : DRAG_NONE;
		drag_x0 = x;
		drag_y0 = y;
		return;
	}
	if (drag == DRAG_NONE) return;
	int dx = x - drag_x0;
	if (pressed) {
		if (drag == DRAG_MAYBE) {
			if (abs(dx) < PAGE_DRAG_START || abs(dx) < abs(y - drag_y0)) return;
			drag = DRAG_PAGING; // from here on it's the pager's: no tap or long press on an app
			lv_anim_delete(strip, slide_exec);
			lv_indev_reset(indev, NULL);
			lv_indev_wait_release(indev);
		}
		// past the first or last page, the strip moves a third as far as the finger
		bool edge = (cur_page == 0 && dx > 0) || (cur_page == n_pages - 1 && dx < 0);
		lv_obj_set_x(strip, -cur_page * SPRITE_W + (edge ? dx / 3 : dx));
		return;
	}
	if (drag == DRAG_PAGING) { // released: turn the page, or settle back (from where the strip is)
		double v = 0; // px/ms over the last two reports
		if (nsmp == 2 && smp[1].t > smp[0].t) v = (smp[1].x - smp[0].x) / (smp[1].t - smp[0].t);
		int p = cur_page;
		if (dx <= -PAGE_TURN_PX || v <= -PAGE_FLICK_SPEED) p++;
		else if (dx >= PAGE_TURN_PX || v >= PAGE_FLICK_SPEED) p--;
		go_to_page(p, true);
	}
	drag = DRAG_NONE;
}

// Dots in the status bar for the pages, the current one bright; none with only one page.
static void update_dots(void) {
	if (!dots) return;
	lv_obj_clean(dots);
	bool show = n_pages > 1 && !lv_obj_has_flag(page_home, LV_OBJ_FLAG_HIDDEN);
	if (!show) return;
	for (int i = 0; i < n_pages; i++) {
		lv_obj_t *d = lv_obj_create(dots);
		lv_obj_remove_style_all(d);
		lv_obj_set_size(d, 8, 8);
		lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(d, lv_color_hex(i == cur_page ? C_TEXT : 0x4a5560), 0);
		lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
		lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(d, 12);
		lv_obj_add_event_cb(d, dot_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}
}

static void rebuild_grid(void *unused) {
	rebuild_pending = false;
	lv_obj_clean(strip);
	int n = 0;
	for (int i = 0; i < apps_count(); i++) { // Settings is the gear in the bar, not a tile
		struct sprite_app *a = apps_get(i);
		if (!a->hidden && !a->builtin) n++;
	}
	n_pages = n ? (n + PER_PAGE - 1) / PER_PAGE : 1;
	lv_obj_set_width(strip, n_pages * SPRITE_W);
	lv_obj_t *page = NULL;
	int k = 0, p = 0;
	for (int i = 0; i < apps_count() || !page; i++) {
		struct sprite_app *a = i < apps_count() ? apps_get(i) : NULL;
		if (a && (a->hidden || a->builtin)) continue;
		if (!page || k == PER_PAGE) {
			page = lv_obj_create(strip);
			lv_obj_remove_style_all(page);
			lv_obj_set_size(page, SPRITE_W, SPRITE_H - BAR_H);
			lv_obj_set_pos(page, p++ * SPRITE_W, 0);
			lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
			lv_obj_set_style_pad_hor(page, GRID_PAD_X, 0);
			lv_obj_set_style_pad_top(page, GRID_PAD_TOP, 0);
			lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
			k = 0;
		}
		if (!a) break;
		make_cell(page, a);
		k++;
	}
	go_to_page(cur_page, false);
	if (opening && opening->pid == 0 && !opening->pending) { // it exited before showing a window
		char s[96];
		snprintf(s, sizeof s, "%s closed", opening->name);
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

	// The pages of the grid, side by side on a strip that the pager moves (see pager_input)
	pager = lv_obj_create(page_home);
	lv_obj_remove_style_all(pager);
	lv_obj_set_size(pager, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_remove_flag(pager, LV_OBJ_FLAG_SCROLLABLE);
	strip = lv_obj_create(pager);
	lv_obj_remove_style_all(strip);
	lv_obj_set_size(strip, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

	toast = lv_obj_create(scr);
	lv_obj_remove_style_all(toast);
	lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(toast, lv_color_hex(0x26313c), 0);
	lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(toast, 20, 0);
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

static void bl_released(lv_event_t *e) {
	char v[8];
	snprintf(v, sizeof v, "%d", (int)lv_slider_get_value(bl_slider));
	sysinfo_set_backlight(v);
	lv_obj_remove_state(bl_auto, LV_STATE_CHECKED);
}

static void bl_auto_changed(lv_event_t *e) {
	if (lv_obj_has_state(bl_auto, LV_STATE_CHECKED)) sysinfo_set_backlight("auto");
	else bl_released(NULL);
}

static void confirm_cb(lv_event_t *e) {
	sysinfo_helper(lv_event_get_user_data(e)); // "reboot" or "poweroff"
}

static void cancel_cb(lv_event_t *e) { lv_msgbox_close(lv_event_get_user_data(e)); }

static void power_clicked(lv_event_t *e) {
	const char *what = lv_event_get_user_data(e);
	bool reboot = !strcmp(what, "reboot");
	lv_obj_t *mb = lv_msgbox_create(NULL);
	lv_msgbox_add_title(mb, reboot ? "Restart the OpenFrame?" : "Switch off the OpenFrame?");
	lv_msgbox_add_text(mb, reboot ? "It will be back in about half a minute."
				       : "Unplug it and plug it back in to switch it on again.");
	lv_obj_t *yes = lv_msgbox_add_footer_button(mb, reboot ? "Restart" : "Switch off");
	lv_obj_add_event_cb(yes, confirm_cb, LV_EVENT_CLICKED, reboot ? "reboot" : "poweroff");
	lv_obj_t *no = lv_msgbox_add_footer_button(mb, "Cancel");
	lv_obj_add_event_cb(no, cancel_cb, LV_EVENT_CLICKED, mb);
}

static lv_obj_t *section(lv_obj_t *parent, const char *title) {
	lv_obj_t *l = lv_label_create(parent);
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
	show_toast("Touch calibration saved", 3000);
}

static lv_obj_t *cal_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_size(b, 200, 60);
	lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

static void cal_result(void) {
	cal_collecting = false;
	sprite_calibrating(false);
	lv_obj_clean(cal);
	lv_obj_t *box = lv_obj_create(cal);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, 640, LV_SIZE_CONTENT);
	lv_obj_center(box);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(box, 22, 0);
	lv_obj_t *l = lv_label_create(box);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_t *row = lv_obj_create(box);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 20, 0);

	double before, before_worst, after, after_worst;
	cal_errors(cal_prev, &before, &before_worst);
	if (!cal_fit(cal_new)) after_worst = 1e9;
	else cal_errors(cal_new, &after, &after_worst);
	if (after_worst > 25) { // one tap well away from its cross, most likely
		lv_label_set_text(l, "That didn't look right: one of the taps was probably off its cross.\n"
				      "Nothing has changed.");
		cal_button(row, LV_SYMBOL_REFRESH "  Again", C_ACCENT, cal_start);
		cal_button(row, "Cancel", 0x3a4a5a, cal_cancel);
		return;
	}
	// Try the new calibration straight away, so the buttons below are tapped with it.
	sprite_set_calibration(cal_new);
	lv_label_set_text_fmt(l, "Touches were landing %.0f px from the crosses on average (%.0f at worst).\n"
			      "With this calibration: %.0f px (%.0f at worst).", before, before_worst, after, after_worst);
	cal_button(row, LV_SYMBOL_OK "  Save", 0x2e8b57, cal_save);
	cal_button(row, LV_SYMBOL_REFRESH "  Again", 0x3a4a5a, cal_start);
	cal_button(row, "Cancel", 0x3a4a5a, cal_cancel);
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
	lv_label_set_text_fmt(l, "Tap the centre of the cross (%d of %d)", cal_i + 1, CAL_N);
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
	show_toast("Standard touch calibration", 3000);
}

static void build_settings(lv_obj_t *scr) {
	page_settings = lv_obj_create(scr);
	lv_obj_remove_style_all(page_settings);
	lv_obj_set_size(page_settings, SPRITE_W, SPRITE_H - BAR_H);
	lv_obj_set_pos(page_settings, 0, BAR_H);
	lv_obj_set_flex_flow(page_settings, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(page_settings, 30, 0);
	lv_obj_set_style_pad_ver(page_settings, 14, 0);
	lv_obj_set_style_pad_row(page_settings, 12, 0);
	lv_obj_set_scroll_dir(page_settings, LV_DIR_VER);

	lv_obj_t *back = lv_button_create(page_settings);
	lv_obj_set_size(back, 160, 52);
	lv_obj_t *bl = lv_label_create(back);
	lv_label_set_text(bl, LV_SYMBOL_LEFT "  Home");
	lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
	lv_obj_center(bl);
	lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

	section(page_settings, "BRIGHTNESS");
	lv_obj_t *row = lv_obj_create(page_settings);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), 56);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 24, 0);
	bl_slider = lv_slider_create(row);
	lv_obj_set_size(bl_slider, 520, 24);
	lv_slider_set_range(bl_slider, 5, 100);
	lv_obj_set_ext_click_area(bl_slider, 20); // easier to grab with a finger
	lv_obj_add_event_cb(bl_slider, bl_released, LV_EVENT_RELEASED, NULL);
	lv_obj_t *al = lv_label_create(row);
	lv_label_set_text(al, "Auto");
	lv_obj_set_style_text_font(al, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(al, lv_color_hex(C_TEXT), 0);
	bl_auto = lv_switch_create(row);
	lv_obj_set_size(bl_auto, 72, 38);
	lv_obj_add_event_cb(bl_auto, bl_auto_changed, LV_EVENT_VALUE_CHANGED, NULL);

	section(page_settings, "TOUCH");
	lv_obj_t *trow = lv_obj_create(page_settings);
	lv_obj_remove_style_all(trow);
	lv_obj_set_size(trow, LV_PCT(100), 64);
	lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(trow, 20, 0);
	lv_obj_t *tb = lv_button_create(trow);
	lv_obj_set_size(tb, 260, 56);
	lv_obj_t *tl = lv_label_create(tb);
	lv_label_set_text(tl, LV_SYMBOL_EDIT "  Calibrate touch");
	lv_obj_center(tl);
	lv_obj_add_event_cb(tb, cal_start, LV_EVENT_CLICKED, NULL);
	lv_obj_t *rb = lv_button_create(trow);
	lv_obj_set_size(rb, 220, 56);
	lv_obj_set_style_bg_color(rb, lv_color_hex(0x3a4a5a), 0);
	lv_obj_t *rl = lv_label_create(rb);
	lv_label_set_text(rl, "Standard");
	lv_obj_center(rl);
	lv_obj_add_event_cb(rb, cal_reset, LV_EVENT_CLICKED, NULL);

	section(page_settings, "ABOUT");
	about_label = lv_label_create(page_settings);
	lv_obj_set_style_text_font(about_label, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(about_label, lv_color_hex(C_TEXT), 0);
	lv_obj_set_style_text_line_space(about_label, 4, 0);

	section(page_settings, "POWER");
	lv_obj_t *prow = lv_obj_create(page_settings);
	lv_obj_remove_style_all(prow);
	lv_obj_set_size(prow, LV_PCT(100), 64);
	lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(prow, 20, 0);
	const char *labels[] = { LV_SYMBOL_REFRESH "  Restart", LV_SYMBOL_POWER "  Switch off" };
	const char *whats[] = { "reboot", "poweroff" };
	for (int i = 0; i < 2; i++) {
		lv_obj_t *b = lv_button_create(prow);
		lv_obj_set_size(b, 220, 56);
		lv_obj_set_style_bg_color(b, lv_color_hex(i ? 0xb3403a : 0x3a4a5a), 0);
		lv_obj_t *l = lv_label_create(b);
		lv_label_set_text(l, labels[i]);
		lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
		lv_obj_center(l);
		lv_obj_add_event_cb(b, power_clicked, LV_EVENT_CLICKED, (void *)whats[i]);
	}
	lv_timer_create(refresh_settings, 3000, NULL);
	lv_obj_add_flag(page_settings, LV_OBJ_FLAG_HIDDEN);
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
	lv_display_set_theme(disp, th);

	indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, indev_read);
	lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT); // read when the compositor has input, not on a timer

	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	build_bar(scr);
	build_home(scr);
	build_settings(scr);
}
