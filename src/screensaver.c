// screensaver.c: Sprite's own screensaver, a split-flap clock: the hours and minutes on four
// flaps that flip as they change, and the date under them, its day's number in a box beside
// the weekday and month (as the OpenFrame's first screensaver had them). Drawn on LVGL's
// system layer, over everything; shell.c starts and stops it (display sleep's timing) and
// main.c shows the shell over any app while it's up.
//
// Each flap is a card of four halves: the new digit's top and the old one's bottom stay put,
// and over them the old top folds down onto the hinge, then the new bottom falls from it.

#include <stdio.h>
#include <time.h>
#include <string.h>
#include "lvgl.h"
#include "sprite.h"

extern const lv_font_t sprite_clock_180, sprite_clock_56;

#define C_SS_TOP 0x131920    // the background, a gradient down to C_SS_BOTTOM (Sprite's own)
#define C_SS_BOTTOM 0x173a5a
#define C_FLAP 0x232d38      // a flap's card
#define C_FLAP_LINE 0x0d1217 // the split across it
#define C_DIGIT 0xe8edf2
#define C_DATE_DIM 0x8b97a3

#define FLAP_W 150
#define FLAP_H 196
#define FLAP_GAP 14          // between a pair's two flaps
#define COLON_W 56           // between the hours and the minutes
#define CLOCK_TOP 56
#define DATE_BOX_BORDER 3
#define DATE_BOX_PAD 2
#define FLIP_MS 180          // each half of a flip

enum { HALF_TOP_NEW, HALF_BOT_OLD, HALF_FLAP_TOP, HALF_FLAP_BOT, N_HALVES };
struct flap {
	lv_obj_t *card, *half[N_HALVES], *label[N_HALVES];
	char now;               // the digit it shows (' ' for none)
};

static lv_obj_t *ss, *ampm, *date_row, *date_num, *date_day, *date_month;
static struct flap flaps[4];
static lv_timer_t *ss_timer;
static int shown_min = -1;
static bool shown_known;

// A half of a flap: the card's colour, clipped to its half, the digit centred on the whole
// card's height (so each half shows its part)
static lv_obj_t *half_new(lv_obj_t *card, bool top, lv_obj_t **label) {
	lv_obj_t *h = lv_obj_create(card);
	lv_obj_remove_style_all(h);
	lv_obj_set_size(h, FLAP_W, FLAP_H / 2);
	lv_obj_set_pos(h, 0, top ? 0 : FLAP_H / 2);
	lv_obj_set_style_bg_color(h, lv_color_hex(C_FLAP), 0);
	lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
	lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	// folding about the hinge, the card's middle
	lv_obj_set_style_transform_pivot_x(h, FLAP_W / 2, 0);
	lv_obj_set_style_transform_pivot_y(h, top ? FLAP_H / 2 : 0, 0);
	lv_obj_t *holder = lv_obj_create(h);
	lv_obj_remove_style_all(holder);
	lv_obj_set_size(holder, FLAP_W, FLAP_H);
	lv_obj_set_pos(holder, 0, top ? 0 : -FLAP_H / 2);
	lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *l = *label = lv_label_create(holder);
	lv_obj_set_style_text_font(l, &sprite_clock_180, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_DIGIT), 0);
	lv_label_set_text(l, "");
	lv_obj_center(l);
	return h;
}

static void flap_new(struct flap *f, lv_obj_t *parent, int x) {
	lv_obj_t *c = f->card = lv_obj_create(parent);
	lv_obj_remove_style_all(c);
	lv_obj_set_size(c, FLAP_W, FLAP_H);
	lv_obj_set_pos(c, x, CLOCK_TOP);
	lv_obj_set_style_radius(c, 14, 0);
	lv_obj_set_style_clip_corner(c, true, 0);
	// lifted off the background by a soft shadow below (the four the same: one blur, cached)
	lv_obj_set_style_shadow_width(c, 28, 0);
	lv_obj_set_style_shadow_offset_y(c, 10, 0);
	lv_obj_set_style_shadow_color(c, lv_color_black(), 0);
	lv_obj_set_style_shadow_opa(c, LV_OPA_40, 0);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	for (int i = 0; i < N_HALVES; i++) f->half[i] = half_new(c, i == HALF_TOP_NEW || i == HALF_FLAP_TOP, &f->label[i]);
	lv_obj_add_flag(f->half[HALF_FLAP_TOP], LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(f->half[HALF_FLAP_BOT], LV_OBJ_FLAG_HIDDEN);
	// the split, over everything
	lv_obj_t *line = lv_obj_create(c);
	lv_obj_remove_style_all(line);
	lv_obj_set_size(line, FLAP_W, 3);
	lv_obj_set_pos(line, 0, FLAP_H / 2 - 1);
	lv_obj_set_style_bg_color(line, lv_color_hex(C_FLAP_LINE), 0);
	lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
	f->now = ' ';
}

static void set_digit(lv_obj_t *l, char d) {
	char t[2] = { d == ' ' ? 0 : d, 0 };
	lv_label_set_text(l, t);
}

// A half folding: its height by v (256 is flat, 0 edge on), darker as it turns away
static void fold(lv_obj_t *h, int32_t v) {
	lv_obj_set_style_transform_scale_y(h, v, 0);
	lv_obj_set_style_bg_color(h, lv_color_mix(lv_color_hex(C_FLAP), lv_color_black(), (uint8_t)(127 + v / 2)), 0);
}
static void fold_cb(void *h, int32_t v) { fold(h, v); }

static void fall_done(lv_anim_t *a) {
	struct flap *f = lv_anim_get_user_data(a);
	set_digit(f->label[HALF_BOT_OLD], f->now); // the new digit's bottom, landed
	lv_obj_add_flag(f->half[HALF_FLAP_BOT], LV_OBJ_FLAG_HIDDEN);
}

static void fold_done(lv_anim_t *a) {
	struct flap *f = lv_anim_get_user_data(a);
	lv_obj_add_flag(f->half[HALF_FLAP_TOP], LV_OBJ_FLAG_HIDDEN);
	lv_obj_t *b = f->half[HALF_FLAP_BOT];
	fold(b, 0);
	lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
	lv_anim_t n;
	lv_anim_init(&n);
	lv_anim_set_var(&n, b);
	lv_anim_set_values(&n, 0, 256);
	lv_anim_set_duration(&n, FLIP_MS);
	lv_anim_set_exec_cb(&n, fold_cb);
	lv_anim_set_path_cb(&n, lv_anim_path_ease_out);
	lv_anim_set_user_data(&n, f);
	lv_anim_set_completed_cb(&n, fall_done);
	lv_anim_start(&n);
}

// To digit d: flipped over (after delay ms), or set at once if !animate
static void flap_to(struct flap *f, char d, bool animate, uint32_t delay) {
	if (d == f->now) return;
	char was = f->now;
	f->now = d;
	lv_anim_delete(f->half[HALF_FLAP_TOP], fold_cb);
	lv_anim_delete(f->half[HALF_FLAP_BOT], fold_cb);
	if (!animate) {
		set_digit(f->label[HALF_TOP_NEW], d);
		set_digit(f->label[HALF_BOT_OLD], d);
		lv_obj_add_flag(f->half[HALF_FLAP_TOP], LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(f->half[HALF_FLAP_BOT], LV_OBJ_FLAG_HIDDEN);
		return;
	}
	set_digit(f->label[HALF_TOP_NEW], d);   // behind the falling top: the new one
	set_digit(f->label[HALF_BOT_OLD], was); // the old bottom until the new one lands on it
	set_digit(f->label[HALF_FLAP_TOP], was);
	set_digit(f->label[HALF_FLAP_BOT], d);
	lv_obj_t *t = f->half[HALF_FLAP_TOP];
	fold(t, 256);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(f->half[HALF_FLAP_BOT], LV_OBJ_FLAG_HIDDEN);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, t);
	lv_anim_set_values(&a, 256, 0);
	lv_anim_set_duration(&a, FLIP_MS);
	lv_anim_set_delay(&a, delay);
	lv_anim_set_exec_cb(&a, fold_cb);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
	lv_anim_set_user_data(&a, f);
	lv_anim_set_completed_cb(&a, fold_done);
	lv_anim_start(&a);
}

// The time and date now; flipped if they've changed while it shows
static void ss_show(bool animate) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	bool known = shell_time_known();
	if (animate && tm.tm_min == shown_min && known == shown_known) return;
	shown_min = tm.tm_min;
	shown_known = known;
	bool h24 = shell_clock_24();
	int h = h24 ? tm.tm_hour : (tm.tm_hour % 12 ? tm.tm_hour % 12 : 12);
	char d[4] = { h / 10 ? (char)('0' + h / 10) : (h24 ? '0' : ' '), (char)('0' + h % 10),
		      (char)('0' + tm.tm_min / 10), (char)('0' + tm.tm_min % 10) };
	// Till the time's been synced (see shell_time_known): zeros, no am or pm, no date; the
	// flaps turn to the time once it is
	if (!known) memset(d, '0', sizeof d);
	lv_obj_set_style_opa(date_row, known ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	// the units first, then the tens a little after: as one tumbles into the next
	for (int i = 3; i >= 0; i--) flap_to(&flaps[i], d[i], animate, (uint32_t)(3 - i) * 90);
	char t[48];
	if (!h24) { strftime(t, sizeof t, "%P", &tm); lv_label_set_text(ampm, t); }
	lv_obj_set_style_opa(ampm, h24 || !known ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
	snprintf(t, sizeof t, "%d", tm.tm_mday);
	lv_label_set_text(date_num, t);
	strftime(t, sizeof t, "%A", &tm);
	lv_label_set_text(date_day, t);
	strftime(t, sizeof t, "%B", &tm);
	lv_label_set_text(date_month, t);
}

static void ss_tick(lv_timer_t *t) { ss_show(true); }

void screensaver_build(void) {
	ss = lv_obj_create(lv_layer_sys());
	lv_obj_remove_style_all(ss);
	lv_obj_set_size(ss, SPRITE_W, SPRITE_H);
	lv_obj_set_style_bg_color(ss, lv_color_hex(C_SS_TOP), 0);
	lv_obj_set_style_bg_grad_color(ss, lv_color_hex(C_SS_BOTTOM), 0);
	lv_obj_set_style_bg_grad_dir(ss, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(ss, LV_OPA_COVER, 0);
	lv_obj_remove_flag(ss, LV_OBJ_FLAG_SCROLLABLE);
	// The time, across the middle: two pairs of flaps, a colon's room between them
	int total = 4 * FLAP_W + 2 * FLAP_GAP + COLON_W, x = (SPRITE_W - total) / 2;
	for (int i = 0; i < 4; i++) {
		flap_new(&flaps[i], ss, x);
		x += FLAP_W + (i == 1 ? COLON_W : FLAP_GAP);
	}
	for (int i = 0; i < 2; i++) { // the colon: two dots
		lv_obj_t *dot = lv_obj_create(ss);
		lv_obj_remove_style_all(dot);
		lv_obj_set_size(dot, 12, 12);
		lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(dot, lv_color_hex(C_DIGIT), 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		lv_obj_set_pos(dot, (SPRITE_W - 12) / 2, CLOCK_TOP + FLAP_H / 2 + (i ? 28 : -40));
	}
	// am or pm (a 12-hour clock), under the minutes' last flap, at its right
	ampm = lv_label_create(ss);
	lv_obj_set_style_text_font(ampm, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(ampm, lv_color_hex(C_DATE_DIM), 0);
	lv_label_set_text(ampm, "");
	lv_obj_align_to(ampm, flaps[3].card, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 6);
	// The date under it: the day's number in a box, then the weekday over the month, the month
	// level with the number (the row's ends aligned, the column raised by the box's border and
	// padding, so the two labels, the same font, sit on one line)
	lv_obj_t *row = date_row = lv_obj_create(ss);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(row, 20, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *box = lv_obj_create(row);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_border_color(box, lv_color_hex(C_DIGIT), 0);
	lv_obj_set_style_border_width(box, DATE_BOX_BORDER, 0);
	lv_obj_set_style_radius(box, 12, 0);
	lv_obj_set_style_pad_hor(box, 16, 0);
	lv_obj_set_style_pad_ver(box, DATE_BOX_PAD, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	date_num = lv_label_create(box);
	lv_obj_set_style_text_font(date_num, &sprite_clock_56, 0);
	lv_obj_set_style_text_color(date_num, lv_color_hex(C_DIGIT), 0);
	lv_obj_t *col = lv_obj_create(row);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_bottom(col, DATE_BOX_BORDER + DATE_BOX_PAD, 0);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	date_day = lv_label_create(col);
	lv_obj_set_style_text_font(date_day, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(date_day, lv_color_hex(C_DATE_DIM), 0);
	date_month = lv_label_create(col);
	lv_obj_set_style_text_font(date_month, &sprite_clock_56, 0);
	lv_obj_set_style_text_color(date_month, lv_color_hex(C_DIGIT), 0);
	lv_obj_set_style_margin_top(date_month, -10, 0); // its capitals close under the weekday
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, CLOCK_TOP + FLAP_H + 52);
	lv_obj_add_flag(ss, LV_OBJ_FLAG_HIDDEN);
}

bool screensaver_active(void) { return ss && !lv_obj_has_flag(ss, LV_OBJ_FLAG_HIDDEN); }

void screensaver_start(void) {
	if (!ss || screensaver_active()) return;
	ss_show(false);
	lv_obj_remove_flag(ss, LV_OBJ_FLAG_HIDDEN);
	ss_timer = lv_timer_create(ss_tick, 1000, NULL);
	sprite_set_covered(true);
}

void screensaver_stop(void) {
	if (!screensaver_active()) return;
	lv_obj_add_flag(ss, LV_OBJ_FLAG_HIDDEN);
	if (ss_timer) lv_timer_delete(ss_timer);
	ss_timer = NULL;
	for (int i = 0; i < 4; i++) {
		lv_anim_delete(flaps[i].half[HALF_FLAP_TOP], fold_cb);
		lv_anim_delete(flaps[i].half[HALF_FLAP_BOT], fold_cb);
		flaps[i].now = 0; // none: the next start sets every flap, a blank one too
	}
	shown_min = -1;
	sprite_set_covered(false);
}
