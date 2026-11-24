// SPDX-License-Identifier: GPL-3.0-or-later
// sprite compositor: wlroots with the pixman renderer, one fullscreen app at a time,
// LVGL shell drawn into a buffer of our own in the same scene.
#define _GNU_SOURCE
#include <assert.h>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

#include "lvgl/lvgl.h"
#include "sprite.h"

enum gesture { G_NONE, G_SHELL, G_APP, G_DONE };

struct sprite_view {
	struct wl_list link;
	struct sprite_app *app;
	struct wlr_scene_tree *tree;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_xwayland_surface *xw;
	bool mapped, primary;
	struct wl_listener map, unmap, commit, destroy, associate, dissociate, request_configure,
		request_fullscreen;
};

// A buffer in our own memory: the shell (LVGL draws into it) and the home handle.
struct shell_buffer {
	struct wlr_buffer base;
	uint32_t *pixels;
	uint32_t format;
};

static struct {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *layout;
	struct wlr_scene_tree *apps_tree;
	struct wlr_output *output;      // the panel
	struct wlr_scene_buffer *shell_node;
	struct wlr_scene_buffer *handle; // where to touch to go home, over apps
	struct shell_buffer handlebuf;
	struct wl_event_source *handle_timer;
	float handle_from, handle_to;
	uint32_t handle_fade_start;
	bool handle_fading;
	struct sprite_app *handle_app;   // the app the handle was last shown for
	struct shell_buffer shellbuf;
	pixman_region32_t shell_damage;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xwayland *xwayland;
	struct wlr_cursor *cursor;
	struct wlr_seat *seat;
	struct wl_list views;
	struct wl_list keyboards;
	struct sprite_app *fg;          // app on screen, NULL = home
	struct sprite_app *hold;        // black screen until this app's window appears
	struct wl_event_source *hold_timer;
	// The boot splash carried on (see splash_build): its black, its bird and bar, and the fade
	struct wlr_scene_rect *splash_black;
	struct wlr_scene_buffer *splash_bird;
	struct shell_buffer splashbuf;
	struct wl_event_source *splash_timer;
	uint64_t splash_t0;
	enum gesture gesture;
	bool home_bottom;       // the home handle is at the bottom edge
	double app_press_y;     // where a touch given to the app began
	bool app_press_home_x;  // ... and whether that was in the middle, under the handle
	bool button_down;
	bool trace_input;       // SPRITE_TRACE_INPUT=1: log presses given to apps
	int trace_perf;         // SPRITE_TRACE_PERF=1: frame timings, 2: and each late frame
	// one burst of motion: frames shown with under 200 ms between them
	struct { int renders, frames, missed; double render_sum, render_max, gap_max, first, last; } perf;
	double calib[6];        // touch correction (see sprite.h)
	bool calibrating;
	// Display sleep (the shell decides when): the time of the last touch or key, whether the
	// screen's asleep (a touch then only wakes it); surfaces asking it not to sleep (idle-inhibit:
	// a video playing, say) are wlroots' list, counted only for the app on screen
	uint64_t last_input_ms;
	bool asleep;
	bool covered; // the shell full screen over everything, an app's too (the screensaver)
	struct sprite_app *saver; // a screensaver app: over everything once its window maps
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wl_listener new_inhibitor;
	struct wl_event_source *release_timer; // see SPRITE_DEBOUNCE_MS
	bool release_pending;
	uint32_t release_button, release_time;
	double release_x, release_y;            // where a held lift left off (SPRITE_BRIDGE_PX)
	struct wl_event_source *lv_timer, *sigchld;
	struct wl_listener new_output, new_input, new_xdg_toplevel, new_xdg_popup, new_xw_surface,
		xw_ready, cursor_motion, cursor_motion_abs, cursor_button, cursor_axis, cursor_frame,
		touch_down, touch_up, touch_motion, touch_frame;
} S;

struct keyboard {
	struct wl_list link;
	struct wlr_keyboard *wlr;
	struct wl_listener key, modifiers, destroy;
};

struct wl_event_loop *sprite_event_loop(void) { return S.loop; }

/* ---- display sleep: how long since a touch, and waking with the next ---- */

static uint64_t mono_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void input_seen(void) { S.last_input_ms = mono_ms(); }

uint64_t sprite_idle_ms(void) { return S.last_input_ms ? mono_ms() - S.last_input_ms : 0; }
// Idle is held off only by the app on screen (the protocol's "while its surface is visible"):
// one left running behind the home screen (an SDL game: SDL asks by default, through Xwayland
// too), or a screensaver app, doesn't keep the screen awake.
bool sprite_sleep_inhibited(void) {
	if (!S.fg || S.fg->saver) return false;
	struct wlr_idle_inhibitor_v1 *in;
	wl_list_for_each(in, &S.idle_inhibit->inhibitors, link) {
		struct wlr_surface *root = in->surface ? wlr_surface_get_root_surface(in->surface) : NULL;
		struct sprite_view *v;
		wl_list_for_each(v, &S.views, link) {
			if (!v->mapped || v->app != S.fg) continue;
			struct wlr_surface *s = v->xdg ? v->xdg->base->surface : v->xw ? v->xw->surface : NULL;
			if (s && s == root) return true;
		}
	}
	return false;
}
void sprite_set_asleep(bool asleep) { S.asleep = asleep; }
static void apply_foreground(void);
static void splash_raise(void);
// A screensaver app runs over the flip clock, which covers the screen while it starts (and
// if it fails, or quits); it's never the foreground app, and gets no input: a touch ends it.
void sprite_saver_start(struct sprite_app *app) {
	if (S.saver || !app) return;
	S.saver = app;
	wlr_log(WLR_INFO, "screensaver %s", app->name);
	if (app->pid == 0) apps_launch(app);
	else if (app->closing) app->reopening = true; // still going from the last time: again once it's gone
	apply_foreground();
}
void sprite_saver_end(void) {
	struct sprite_app *a = S.saver;
	if (!a) return;
	S.saver = NULL;
	sprite_close_app(a);
	apply_foreground();
}
void sprite_set_covered(bool covered) {
	if (S.covered == covered) return;
	S.covered = covered;
	apply_foreground();
}

// A touch (or key) while asleep: it wakes the screen and goes no further, so it can't tap
// something nobody could see. True if it did.
static bool wake_on_input(void) {
	input_seen();
	if (!S.asleep) return false;
	S.asleep = false;
	wlr_log(WLR_INFO, "awake: a touch");
	shell_wake();
	return true;
}

struct inhibitor { struct wl_listener destroy; };
static void inhibitor_destroy(struct wl_listener *l, void *data) {
	struct inhibitor *in = wl_container_of(l, in, destroy);
	wl_list_remove(&in->destroy.link);
	free(in);
	input_seen(); // the idle time counts from when it stopped (the film ended), not from long before
}
static void new_inhibitor(struct wl_listener *l, void *data) {
	struct wlr_idle_inhibitor_v1 *w = data;
	struct inhibitor *in = calloc(1, sizeof *in);
	if (!in) return;
	in->destroy.notify = inhibitor_destroy;
	wl_signal_add(&w->events.destroy, &in->destroy);
}

/* ---- shell buffer: LVGL renders straight into this memory, the scene samples it ---- */

static void shellbuf_destroy(struct wlr_buffer *b) { (void)b; }
static bool shellbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct shell_buffer *sb = wl_container_of(b, sb, base);
	*data = sb->pixels;
	*format = sb->format;
	*stride = b->width * 4;
	return true;
}
static void shellbuf_end(struct wlr_buffer *b) { (void)b; }
static const struct wlr_buffer_impl shellbuf_impl = {
	.destroy = shellbuf_destroy,
	.begin_data_ptr_access = shellbuf_begin,
	.end_data_ptr_access = shellbuf_end,
};

// The home handle: a translucent pill with anti-aliased ends, premultiplied ARGB.
static void draw_handle(uint32_t *px, int w, int h) {
	const double r = h / 2.0, alpha = 1.0, grey = 0.85; // opacity is set on the scene node
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			// distance from the pill's centre line, minus its radius: <0 inside
			double cx = x + 0.5, cy = y + 0.5;
			double ex = cx < r ? r : cx > w - r ? w - r : cx;
			double d = hypot(cx - ex, cy - r) - r;
			double cov = d <= -0.5 ? 1.0 : d >= 0.5 ? 0.0 : 0.5 - d;
			double a = alpha * cov;
			uint32_t A = (uint32_t)(a * 255 + 0.5), C = (uint32_t)(grey * a * 255 + 0.5);
			px[y * w + x] = A << 24 | C << 16 | C << 8 | C;
		}
	}
}

void sprite_shell_damage(int x, int y, int w, int h, bool commit) {
	pixman_region32_union_rect(&S.shell_damage, &S.shell_damage, x, y, w, h);
	if (!commit) return;
	wlr_scene_buffer_set_buffer_with_damage(S.shell_node, &S.shellbuf.base, &S.shell_damage);
	pixman_region32_clear(&S.shell_damage);
}

/* ---- home handle fading ---- */

static uint32_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int handle_timer_cb(void *data) {
	if (!S.handle_fading) { // the hold is over: start fading to the app's level
		S.handle_fading = true;
		S.handle_fade_start = now_ms();
	}
	float k = (now_ms() - S.handle_fade_start) / (float)SPRITE_HANDLE_FADE_MS;
	if (k >= 1) k = 1;
	wlr_scene_buffer_set_opacity(S.handle, S.handle_from + (S.handle_to - S.handle_from) * k);
	if (k < 1) wl_event_source_timer_update(S.handle_timer, 16);
	return 0;
}

// Show the handle at normal strength for a new foreground app, then fade to its own level.
static void handle_show(struct sprite_app *app) {
	if (app == S.handle_app) return;
	S.handle_app = app;
	if (!app) return;
	wlr_scene_buffer_set_opacity(S.handle, SPRITE_HANDLE_NORMAL);
	S.handle_from = SPRITE_HANDLE_NORMAL;
	S.handle_to = app->handle;
	S.handle_fading = false;
	wl_event_source_timer_update(S.handle_timer, S.handle_to == S.handle_from ? 0 : SPRITE_HANDLE_HOLD_MS);
}

/* ---- LVGL timer on the Wayland event loop ---- */

static int lv_timer_cb(void *data) {
	uint32_t next = lv_timer_handler();
	if (next == LV_NO_TIMER_READY || next > 500) next = 500;
	if (next < 1) next = 1;
	wl_event_source_timer_update(S.lv_timer, next);
	return 0;
}
void sprite_lvgl_kick(void) { wl_event_source_timer_update(S.lv_timer, 1); }

/* ---- views and foreground ---- */

static struct wlr_surface *view_surface(struct sprite_view *v) {
	if (v->xdg) return v->xdg->base->surface;
	if (v->xw) return v->xw->surface;
	return NULL;
}

// Deactivating matters as well as activating: a full-screen SDL app grabs the X pointer, and
// only lets go when it loses focus. Without this, a background SDL app keeps taking every
// touch meant for the X app in front.
static void set_view_active(struct sprite_view *v, bool active) {
	if (!v->mapped) return;
	if (v->xdg) wlr_xdg_toplevel_set_activated(v->xdg, active);
	if (v->xw && !v->xw->override_redirect) wlr_xwayland_surface_activate(v->xw, active);
}

static void focus_view(struct sprite_view *v) {
	struct wlr_surface *surface = v ? view_surface(v) : NULL;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
	if (!surface) {
		wlr_seat_keyboard_notify_clear_focus(S.seat);
		wlr_seat_pointer_notify_clear_focus(S.seat);
		return;
	}
	if (v->xdg) wlr_xdg_toplevel_set_activated(v->xdg, true);
	if (v->xw) {
		// All X apps share one Xwayland, which picks the window for input by its own
		// stacking order, so the foreground app must be on top there too.
		wlr_xwayland_surface_restack(v->xw, NULL, XCB_STACK_MODE_ABOVE);
		wlr_xwayland_surface_activate(v->xw, true);
	}
	if (kb) wlr_seat_keyboard_notify_enter(S.seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
}

static void apply_foreground(void) {
	struct sprite_view *v;
	// Covered (the screensaver), only a screensaver app's windows show, and those over the shell:
	// the apps' tree is moved above it while one's up
	bool saver = S.covered && S.saver && S.saver->view;
	if (saver) wlr_scene_node_place_above(&S.apps_tree->node, &S.shell_node->node);
	else wlr_scene_node_place_below(&S.apps_tree->node, &S.shell_node->node);
	wl_list_for_each(v, &S.views, link) {
		bool on = S.covered ? S.saver && v->app == S.saver : S.fg && v->app == S.fg;
		if (v->tree) wlr_scene_node_set_enabled(&v->tree->node, v->mapped && on);
		if (!S.fg || v != S.fg->view) set_view_active(v, false);
	}
	// An app that keeps the bar (bar=yes) has the shell's top strip above it: only that strip is
	// shown, so nothing is composited under the app.
	bool bar = S.fg && S.fg->bar && !S.covered;
	if (bar) {
		wlr_scene_buffer_set_source_box(S.shell_node, &(struct wlr_fbox){ 0, 0, SPRITE_W, SPRITE_BAR_H });
		wlr_scene_buffer_set_dest_size(S.shell_node, SPRITE_W, SPRITE_BAR_H);
	} else {
		wlr_scene_buffer_set_source_box(S.shell_node, NULL);
		wlr_scene_buffer_set_dest_size(S.shell_node, 0, 0);
	}
	bool shell = (S.fg == NULL && S.hold == NULL) || bar || S.covered;
	wlr_scene_node_set_enabled(&S.shell_node->node, shell);
	wlr_scene_node_set_enabled(&S.handle->node, S.fg != NULL && !S.covered);
	wlr_scene_node_raise_to_top(&S.handle->node);
	splash_raise();
	handle_show(S.fg);
	shell_set_visible(shell);
	shell_set_bar_app(bar ? S.fg : NULL);
	if (S.fg && S.fg->view) {
		wlr_scene_node_raise_to_top(&S.fg->view->tree->node);
		wl_list_for_each(v, &S.views, link) // X menus and tooltips stay above the main window
			if (v->app == S.fg && v->xw && v->xw->override_redirect && v->tree)
				wlr_scene_node_raise_to_top(&v->tree->node);
		focus_view(S.fg->view);
	} else {
		focus_view(NULL);
	}
	shell_apps_changed();
}

static int hold_timeout(void *data) {
	if (!S.hold) return 0;
	char s[128];
	snprintf(s, sizeof s, STR_APP_DIDNT_OPEN, S.hold->name);
	wlr_log(WLR_ERROR, "%s", s);
	S.hold = NULL;
	apply_foreground();
	shell_notice(s);
	return 0;
}

void sprite_hold_for(struct sprite_app *app) {
	if (S.hold == app) return;
	S.hold = app;
	wl_event_source_timer_update(S.hold_timer, app ? 20000 : 0);
	apply_foreground();
}

void sprite_go_home(void) {
	if (S.fg) wlr_log(WLR_INFO, "home (was %s)", S.fg->name);
	S.fg = NULL;
	apply_foreground();
}

struct sprite_app *sprite_foreground(void) { return S.fg; }

void sprite_open_app(struct sprite_app *app) {
	if (app->pid == 0) {
		apps_launch(app);
		shell_apps_changed();
		return; // shown when its first window maps
	}
	if (app->view) {
		S.fg = app;
		apply_foreground();
	}
}

void sprite_close_app(struct sprite_app *app) {
	struct sprite_view *v;
	wl_list_for_each(v, &S.views, link) {
		if (v->app != app || !v->mapped) continue;
		if (v->xdg) wlr_xdg_toplevel_send_close(v->xdg);
		else if (v->xw && !v->xw->override_redirect) wlr_xwayland_surface_close(v->xw);
	}
	apps_close(app);
	if (S.fg == app) sprite_go_home();
}

static pid_t view_pid(struct sprite_view *v) {
	pid_t pid = 0;
	if (v->xdg) wl_client_get_credentials(v->xdg->base->client->client, &pid, NULL, NULL);
	else if (v->xw) pid = v->xw->pid;
	return pid;
}

static void view_mapped(struct sprite_view *v) {
	v->mapped = true;
	if (!v->app) v->app = apps_for_pid(view_pid(v));
	if (v->primary && !v->app->view) {
		v->app->view = v;
		wlr_log(WLR_INFO, "%s window mapped", v->app->name);
		if (v->app->reopen_behind) v->app->reopen_behind = false; // reopened behind (apps_restart)
		else if (!v->app->saver) S.fg = v->app; // a new main window comes to the front (a
							   // screensaver's shows only as one)
		if (S.hold == v->app) {
			S.hold = NULL;
			wl_event_source_timer_update(S.hold_timer, 0);
		}
	}
	apply_foreground();
}

static void view_unmapped(struct sprite_view *v) {
	v->mapped = false;
	if (v->app && v->app->view == v) {
		v->app->view = NULL;
		// another mapped primary window of the same app takes over
		struct sprite_view *o;
		wl_list_for_each(o, &S.views, link)
			if (o != v && o->app == v->app && o->mapped && o->primary) { v->app->view = o; break; }
		if (!v->app->view && S.fg == v->app) {
			S.fg = NULL;
			// the startup app going away by itself will be reopened: keep the screen black
			if (v->app == apps_autostart() && !v->app->closing) {
				S.hold = v->app;
				wl_event_source_timer_update(S.hold_timer, 20000);
			}
		}
	}
	apply_foreground();
}

/* ---- xdg-shell ---- */

// Where an app's windows start: below the bar if it keeps it (bar=yes), else at the top.
static int app_top(struct sprite_app *a) { return a && a->bar ? SPRITE_BAR_H : 0; }

static void xdg_map(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, map);
	v->primary = v->xdg->parent == NULL; // a parent can be set after creation
	if (!v->app) v->app = apps_for_pid(view_pid(v));
	wlr_scene_node_set_position(&v->tree->node, 0, app_top(v->app));
	view_mapped(v);
}
static void xdg_unmap(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, unmap);
	view_unmapped(v);
}

static void xdg_commit(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, commit);
	if (v->xdg->base->initial_commit) {
		if (!v->app) v->app = apps_for_pid(view_pid(v));
		int top = app_top(v->app);
		wlr_xdg_toplevel_set_size(v->xdg, SPRITE_W, SPRITE_H - top);
		// below the bar it's the whole space it has, not the whole screen
		if (top) wlr_xdg_toplevel_set_maximized(v->xdg, true);
		else wlr_xdg_toplevel_set_fullscreen(v->xdg, true);
	}
}

static void xdg_request_fullscreen(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, request_fullscreen);
	if (v->xdg->base->initialized) wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void xdg_destroy(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, destroy);
	if (v->app && v->app->view == v) v->app->view = NULL;
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->link);
	free(v);
}

static void new_xdg_toplevel(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel *xdg = data;
	struct sprite_view *v = calloc(1, sizeof(*v));
	v->xdg = xdg;
	v->primary = xdg->parent == NULL;
	v->tree = wlr_scene_xdg_surface_create(S.apps_tree, xdg->base);
	v->tree->node.data = v;
	xdg->base->data = v->tree;
	wlr_scene_node_set_enabled(&v->tree->node, false);
	v->map.notify = xdg_map;
	wl_signal_add(&xdg->base->surface->events.map, &v->map);
	v->unmap.notify = xdg_unmap;
	wl_signal_add(&xdg->base->surface->events.unmap, &v->unmap);
	v->commit.notify = xdg_commit;
	wl_signal_add(&xdg->base->surface->events.commit, &v->commit);
	v->destroy.notify = xdg_destroy;
	wl_signal_add(&xdg->events.destroy, &v->destroy);
	v->request_fullscreen.notify = xdg_request_fullscreen;
	wl_signal_add(&xdg->events.request_fullscreen, &v->request_fullscreen);
	wl_list_insert(&S.views, &v->link);
}

struct popup { struct wlr_xdg_popup *p; struct wl_listener commit, destroy; };
static void popup_commit(struct wl_listener *l, void *data) {
	struct popup *p = wl_container_of(l, p, commit);
	if (p->p->base->initial_commit) wlr_xdg_surface_schedule_configure(p->p->base);
}
static void popup_destroy(struct wl_listener *l, void *data) {
	struct popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}
static void new_xdg_popup(struct wl_listener *l, void *data) {
	struct wlr_xdg_popup *xp = data;
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xp->parent);
	if (!parent || !parent->data) return;
	struct popup *p = calloc(1, sizeof(*p));
	p->p = xp;
	xp->base->data = wlr_scene_xdg_surface_create(parent->data, xp->base);
	p->commit.notify = popup_commit;
	wl_signal_add(&xp->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&xp->events.destroy, &p->destroy);
}

/* ---- Xwayland ---- */

static void xw_map(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, map);
	v->primary = !v->xw->override_redirect && v->xw->parent == NULL;
	v->tree = wlr_scene_subsurface_tree_create(S.apps_tree, v->xw->surface);
	v->tree->node.data = v;
	if (v->xw->override_redirect) {
		wlr_scene_node_set_position(&v->tree->node, v->xw->x, v->xw->y);
	} else {
		if (!v->app) v->app = apps_for_pid(view_pid(v));
		int top = app_top(v->app);
		wlr_xwayland_surface_configure(v->xw, 0, top, SPRITE_W, SPRITE_H - top);
		wlr_scene_node_set_position(&v->tree->node, 0, top);
		if (top) wlr_xwayland_surface_set_maximized(v->xw, true);
		else wlr_xwayland_surface_set_fullscreen(v->xw, true);
	}
	view_mapped(v);
}

static void xw_unmap(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, unmap);
	if (v->tree) wlr_scene_node_destroy(&v->tree->node);
	v->tree = NULL;
	view_unmapped(v);
}

static void xw_associate(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, associate);
	v->map.notify = xw_map;
	wl_signal_add(&v->xw->surface->events.map, &v->map);
	v->unmap.notify = xw_unmap;
	wl_signal_add(&v->xw->surface->events.unmap, &v->unmap);
}

static void xw_dissociate(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, dissociate);
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
}

static void xw_request_configure(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, request_configure);
	struct wlr_xwayland_surface_configure_event *e = data;
	if (v->xw->override_redirect) {
		wlr_xwayland_surface_configure(v->xw, e->x, e->y, e->width, e->height);
		if (v->tree) wlr_scene_node_set_position(&v->tree->node, e->x, e->y);
	} else {
		int top = app_top(v->app); // always the whole of its space
		wlr_xwayland_surface_configure(v->xw, 0, top, SPRITE_W, SPRITE_H - top);
	}
}

static void xw_destroy(struct wl_listener *l, void *data) {
	struct sprite_view *v = wl_container_of(l, v, destroy);
	if (v->app && v->app->view == v) v->app->view = NULL;
	wl_list_remove(&v->associate.link);
	wl_list_remove(&v->dissociate.link);
	wl_list_remove(&v->request_configure.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->link);
	free(v);
}

static void new_xw_surface(struct wl_listener *l, void *data) {
	struct wlr_xwayland_surface *xw = data;
	struct sprite_view *v = calloc(1, sizeof(*v));
	v->xw = xw;
	v->primary = !xw->override_redirect && xw->parent == NULL;
	v->associate.notify = xw_associate;
	wl_signal_add(&xw->events.associate, &v->associate);
	v->dissociate.notify = xw_dissociate;
	wl_signal_add(&xw->events.dissociate, &v->dissociate);
	v->request_configure.notify = xw_request_configure;
	wl_signal_add(&xw->events.request_configure, &v->request_configure);
	v->destroy.notify = xw_destroy;
	wl_signal_add(&xw->events.destroy, &v->destroy);
	wl_list_insert(&S.views, &v->link);
}

static void xw_ready(struct wl_listener *l, void *data) {
	wlr_xwayland_set_seat(S.xwayland, S.seat);
	wlr_log(WLR_INFO, "Xwayland ready on %s", S.xwayland->display_name);
}

/* ---- input: one pointer-or-touch stream, routed to the shell, an app, or the home gesture ---- */

static void forward_to_app(double x, double y, uint32_t t) {
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(&S.apps_tree->node, x, y, &sx, &sy);
	struct wlr_surface *surface = NULL;
	if (node && node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		if (ss) surface = ss->surface;
	}
	if (surface) {
		if (S.trace_input && !S.button_down)
			wlr_log(WLR_INFO, "trace: app pointer at %.1f,%.1f (screen %.1f,%.1f)", sx, sy, x, y);
		wlr_seat_pointer_notify_enter(S.seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(S.seat, t, sx, sy);
	} else if (!S.button_down) {
		wlr_seat_pointer_notify_clear_focus(S.seat);
	}
}

// How far a point is from the home edge, in the same terms for top and bottom.
static double edge_dist(double y) { return S.home_bottom ? SPRITE_BOTTOM_REACH - y : y; }

/* ---- touch calibration ---- */

void sprite_set_calibration(const double m[6]) { memcpy(S.calib, m, sizeof S.calib); }
void sprite_get_calibration(double m[6]) { memcpy(m, S.calib, sizeof S.calib); }
void sprite_calibrating(bool on) { S.calibrating = on; }

static void load_calibration(void) {
	const double identity[6] = { 1, 0, 0, 0, 1, 0 };
	memcpy(S.calib, identity, sizeof S.calib);
	const char *v = settings_get("calibration");
	double m[6];
	if (v && sscanf(v, "%lf,%lf,%lf,%lf,%lf,%lf", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
		memcpy(S.calib, m, sizeof S.calib);
		wlr_log(WLR_INFO, "touch calibration: %s", v);
	}
}

// Where the finger is, in screen terms: the cursor position with the correction applied.
static void finger(double *x, double *y) {
	double cx = S.cursor->x, cy = S.cursor->y;
	if (!S.calibrating) {
		*x = S.calib[0] * cx + S.calib[1] * cy + S.calib[2];
		*y = S.calib[3] * cx + S.calib[4] * cy + S.calib[5];
	} else {
		*x = cx;
		*y = cy;
	}
	if (*x < 0) *x = 0;
	if (*x > SPRITE_W - 1) *x = SPRITE_W - 1;
	if (*y < 0) *y = 0;
	if (*y > SPRITE_H - 1) *y = SPRITE_H - 1;
}

static void input_motion(double x, double y, uint32_t t) {
	if (S.button_down) input_seen();
	switch (S.gesture) {
	case G_DONE:
		return;
	case G_SHELL:
		shell_pointer((int)x, (int)y, true);
		return;
	case G_APP:
		if (edge_dist(S.app_press_y) < SPRITE_EDGE_BAND_PX && S.app_press_home_x &&
		    edge_dist(y) - edge_dist(S.app_press_y) >= SPRITE_EDGE_SWIPE_PX) {
			wlr_log(WLR_INFO, "home by swipe from y=%.0f", S.app_press_y);
			// let the app see its press end before it goes to the background
			wlr_seat_pointer_notify_button(S.seat, t, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
			S.gesture = G_DONE;
			sprite_go_home();
			return;
		}
		forward_to_app(x, y, t);
		return;
	default:
		if (S.fg && !S.covered && !S.asleep) forward_to_app(x, y, t); // no hover under the screensaver
		else shell_pointer((int)x, (int)y, false);
	}
}

static void finish_release(double x, double y, uint32_t t, uint32_t button);
static void input_press(double x, double y, uint32_t t, uint32_t button) {
	if (S.asleep && !S.release_pending) {
		wake_on_input();
		S.button_down = true;
		S.gesture = G_DONE; // the rest of the stroke too
		return;
	}
	input_seen();
	if (S.covered && !S.release_pending) {
		S.button_down = true;
		S.gesture = G_DONE;
		shell_screensaver_tap();
		return;
	}
	if (S.release_pending) {
		S.release_pending = false;
		wl_event_source_timer_update(S.release_timer, 0);
		// contact bounce: the same stroke carrying on; past the plain bounce (a slider's longer
		// hold), only near where it left off
		if (t - S.release_time <= SPRITE_DEBOUNCE_MS || fabs(y - S.release_y) <= SPRITE_BRIDGE_PX) {
			input_motion(x, y, t);
			return;
		}
		finish_release(S.release_x, S.release_y, S.release_time, S.release_button); // a new touch elsewhere
	}
	S.button_down = true;
	if (S.fg) {
		bool middle = fabs(x - SPRITE_W / 2.0) <= SPRITE_EDGE_HALF_W;
		if (edge_dist(y) < SPRITE_EDGE_PX && middle) {
			// Home now; the rest of this stroke is swallowed so it can't tap a tile.
			wlr_log(WLR_INFO, "home from the %s edge (y=%.0f)", S.home_bottom ? "bottom" : "top", y);
			S.gesture = G_DONE;
			sprite_go_home();
			return;
		}
		if (S.fg->bar && y < SPRITE_BAR_H) { // the bar above it is the shell's
			S.gesture = G_SHELL;
			shell_pointer((int)x, (int)y, true);
			return;
		}
		S.gesture = G_APP;
		if (S.trace_input) wlr_log(WLR_INFO, "trace: app press at screen %.1f,%.1f", x, y);
		S.app_press_y = y;
		S.app_press_home_x = middle;
		forward_to_app(x, y, t);
		wlr_seat_pointer_notify_button(S.seat, t, button, WL_POINTER_BUTTON_STATE_PRESSED);
		return;
	}
	S.gesture = G_SHELL;
	shell_pointer((int)x, (int)y, true);
}

static void finish_release(double x, double y, uint32_t t, uint32_t button) {
	S.button_down = false;
	enum gesture g = S.gesture;
	S.gesture = G_NONE;
	if (g == G_APP) wlr_seat_pointer_notify_button(S.seat, t, button, WL_POINTER_BUTTON_STATE_RELEASED);
	else if (g == G_SHELL) shell_pointer((int)x, (int)y, false);
}

static int release_timer_cb(void *data) {
	if (!S.release_pending) return 0;
	S.release_pending = false;
	double x, y;
	finger(&x, &y);
	finish_release(x, y, S.release_time, S.release_button);
	return 0;
}

static void input_release(double x, double y, uint32_t t, uint32_t button) {
	if (!S.button_down || S.release_pending) return;
	S.release_pending = true;
	S.release_button = button;
	S.release_time = t;
	S.release_x = x;
	S.release_y = y;
	wl_event_source_timer_update(S.release_timer, S.gesture == G_SHELL ? shell_release_hold_ms() : SPRITE_DEBOUNCE_MS);
}

static void cursor_motion(struct wl_listener *l, void *data) {
	struct wlr_pointer_motion_event *e = data;
	wlr_cursor_move(S.cursor, &e->pointer->base, e->delta_x, e->delta_y);
	double x, y;
	finger(&x, &y);
	input_motion(x, y, e->time_msec);
}
static void cursor_motion_abs(struct wl_listener *l, void *data) {
	struct wlr_pointer_motion_absolute_event *e = data;
	wlr_cursor_warp_absolute(S.cursor, &e->pointer->base, e->x, e->y);
	double x, y;
	finger(&x, &y);
	input_motion(x, y, e->time_msec);
}
static void cursor_button(struct wl_listener *l, void *data) {
	struct wlr_pointer_button_event *e = data;
	double x, y;
	finger(&x, &y);
	if (e->state == WL_POINTER_BUTTON_STATE_PRESSED) input_press(x, y, e->time_msec, e->button);
	else input_release(x, y, e->time_msec, e->button);
}
static void cursor_axis(struct wl_listener *l, void *data) {
	struct wlr_pointer_axis_event *e = data;
	input_seen();
	// a wheel wakes the screen or ends the screensaver, as a press does, and goes no further
	if (wake_on_input()) return;
	if (S.covered) { shell_screensaver_tap(); return; }
	if (S.fg) wlr_seat_pointer_notify_axis(S.seat, e->time_msec, e->orientation, e->delta,
					       e->delta_discrete, e->source, e->relative_direction);
}
static void cursor_frame(struct wl_listener *l, void *data) { if (S.fg) wlr_seat_pointer_notify_frame(S.seat); }

// Touch devices (if a unit has a real touch controller) are folded into the same single-touch
// stream, and reach apps as a pointer with the left button.
static int touch_id = -1;
static void touch_down(struct wl_listener *l, void *data) {
	struct wlr_touch_down_event *e = data;
	if (touch_id >= 0) return;
	touch_id = e->touch_id;
	double x, y;
	wlr_cursor_absolute_to_layout_coords(S.cursor, &e->touch->base, e->x, e->y, &x, &y);
	wlr_cursor_warp_closest(S.cursor, NULL, x, y);
	finger(&x, &y);
	input_motion(x, y, e->time_msec);
	input_press(x, y, e->time_msec, BTN_LEFT);
}
static void touch_motion(struct wl_listener *l, void *data) {
	struct wlr_touch_motion_event *e = data;
	if (e->touch_id != touch_id) return;
	double x, y;
	wlr_cursor_absolute_to_layout_coords(S.cursor, &e->touch->base, e->x, e->y, &x, &y);
	wlr_cursor_warp_closest(S.cursor, NULL, x, y);
	finger(&x, &y);
	input_motion(x, y, e->time_msec);
}
static void touch_up(struct wl_listener *l, void *data) {
	struct wlr_touch_up_event *e = data;
	if (e->touch_id != touch_id) return;
	touch_id = -1;
	double x, y;
	finger(&x, &y);
	input_release(x, y, e->time_msec, BTN_LEFT);
}
static void touch_frame(struct wl_listener *l, void *data) { if (S.fg) wlr_seat_pointer_notify_frame(S.seat); }

static void kb_key(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, key);
	struct wlr_keyboard_key_event *e = data;
	const xkb_keysym_t *syms;
	int n = xkb_state_key_get_syms(k->wlr->xkb_state, e->keycode + 8, &syms);
	uint32_t mods = wlr_keyboard_get_modifiers(k->wlr);
	// a key wakes the screen too, and goes no further (its release does, harmlessly)
	if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED && wake_on_input()) return;
	if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED && S.covered) { shell_screensaver_tap(); return; }
	if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < n; i++) {
			if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
				if (S.session) wlr_session_change_vt(S.session, syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
				return;
			}
			if ((mods & WLR_MODIFIER_ALT) && syms[i] == XKB_KEY_Escape) { sprite_go_home(); return; }
			if ((mods & (WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL)) == (WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL) &&
			    syms[i] == XKB_KEY_BackSpace) { wl_display_terminate(S.display); return; }
		}
	}
	if (!S.fg) return; // the shell has no text entry yet
	wlr_seat_set_keyboard(S.seat, k->wlr);
	wlr_seat_keyboard_notify_key(S.seat, e->time_msec, e->keycode, e->state);
}
static void kb_modifiers(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, modifiers);
	wlr_seat_set_keyboard(S.seat, k->wlr);
	wlr_seat_keyboard_notify_modifiers(S.seat, &k->wlr->modifiers);
}
static void kb_destroy(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, destroy);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	free(k);
}

static void new_input(struct wl_listener *l, void *data) {
	struct wlr_input_device *dev = data;
	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: {
		struct keyboard *k = calloc(1, sizeof(*k));
		k->wlr = wlr_keyboard_from_input_device(dev);
		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
		wlr_keyboard_set_keymap(k->wlr, km);
		xkb_keymap_unref(km);
		xkb_context_unref(ctx);
		wlr_keyboard_set_repeat_info(k->wlr, 25, 600);
		k->key.notify = kb_key;
		wl_signal_add(&k->wlr->events.key, &k->key);
		k->modifiers.notify = kb_modifiers;
		wl_signal_add(&k->wlr->events.modifiers, &k->modifiers);
		k->destroy.notify = kb_destroy;
		wl_signal_add(&dev->events.destroy, &k->destroy);
		wlr_seat_set_keyboard(S.seat, k->wlr);
		wl_list_insert(&S.keyboards, &k->link);
		break;
	}
	case WLR_INPUT_DEVICE_POINTER:
	case WLR_INPUT_DEVICE_TOUCH:
		wlr_cursor_attach_input_device(S.cursor, dev);
		break;
	default:
		break;
	}
	wlr_log(WLR_INFO, "input: %s (type %d)", dev->name, dev->type);
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&S.keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(S.seat, caps);
}

/* ---- output ---- */

struct output { struct wlr_output *wlr; struct wl_listener frame, request_state, destroy, present; };

static void perf_flush(double now);

// Each frame that reached the screen, timed by the display (for the performance trace).
static void output_present(struct wl_listener *l, void *data) {
	struct wlr_output_event_present *e = data;
	if (!S.trace_perf || !e->presented) return;
	double now = e->when->tv_sec * 1e3 + e->when->tv_nsec / 1e6;
	double gap = now - S.perf.last;
	if (S.perf.frames && gap > 200) perf_flush(now); // the end of a burst
	if (!S.perf.frames) S.perf.first = now;
	else {
		if (gap > 20) S.perf.missed += (int)(gap / 16.7 + 0.5) - 1; // refreshes with no new frame
		if (gap > 20 && S.trace_perf > 1)
			wlr_log(WLR_INFO, "perf: late frame %.0f ms into the burst, after a %.1f ms gap", now - S.perf.first, gap);
		if (gap > S.perf.gap_max) S.perf.gap_max = gap;
	}
	S.perf.last = now;
	S.perf.frames++;
}

static void perf_flush(double now) {
	if (S.perf.frames > 2) {
		double dur = S.perf.last - S.perf.first;
		wlr_log(WLR_INFO, "perf: %d frames in %.0f ms (%.1f/s), %d late (max gap %.1f ms); drawn %d, avg %.1f ms, max %.1f",
			S.perf.frames, dur, dur > 0 ? (S.perf.frames - 1) * 1000.0 / dur : 0, S.perf.missed, S.perf.gap_max,
			S.perf.renders, S.perf.renders ? S.perf.render_sum / S.perf.renders : 0, S.perf.render_max);
	}
	memset(&S.perf, 0, sizeof S.perf);
}

void perf_render(double ms) {
	if (!S.trace_perf) return;
	S.perf.renders++;
	S.perf.render_sum += ms;
	if (ms > S.perf.render_max) S.perf.render_max = ms;
}

void sprite_request_frame(void) {
	if (S.output) wlr_output_schedule_frame(S.output);
}

// The panel is ready for a frame: the shell draws what has changed (in step with the display,
// one frame per refresh), then the scene, with that, goes to the screen.
/* ---- the first frames: the boot splash carried on ----
   Plymouth's last picture (openframe-scripts' theme: the firmware's bird, 204x82 at 274,199,
   on black, with a 160x4 bar under it, here full) is Sprite's first frame, held a moment, then
   faded away to the shell. In the scene, over everything, so the shell is drawn once and the
   fade is only the compositor blending: an LVGL fade redrew the whole home screen each frame
   (18 ms on an OpenFrame 1, 30 frames a second, choppy). */

#define SPLASH_X 274
#define SPLASH_Y 199
#define SPLASH_W 206 // the bird (to x 478) and the bar (320 to 480)
#define SPLASH_H 135 // the bird (to y 281) and the bar (330 to 334)
#define SPLASH_HOLD_MS 350
#define SPLASH_FADE_MS 500

extern const lv_image_dsc_t sprite_bird;

static void splash_build(void) {
	uint32_t *px = calloc(SPLASH_W * SPLASH_H, 4);
	if (!px) return;
	const uint8_t *a = sprite_bird.data; // white, premultiplied: each channel the alpha
	for (uint32_t y = 0; y < sprite_bird.header.h; y++)
		for (uint32_t x = 0; x < sprite_bird.header.w; x++) {
			uint32_t v = a[y * sprite_bird.header.stride + x];
			px[y * SPLASH_W + x] = v << 24 | v << 16 | v << 8 | v;
		}
	for (int y = 330 - SPLASH_Y; y < 334 - SPLASH_Y; y++)
		for (int x = 320 - SPLASH_X; x < 480 - SPLASH_X; x++) px[y * SPLASH_W + x] = 0xffffffff;
	S.splashbuf.pixels = px;
	S.splashbuf.format = DRM_FORMAT_ARGB8888;
	wlr_buffer_init(&S.splashbuf.base, &shellbuf_impl, SPLASH_W, SPLASH_H);
	float black[4] = { 0, 0, 0, 1 };
	S.splash_black = wlr_scene_rect_create(&S.scene->tree, SPRITE_W, SPRITE_H, black);
	S.splash_bird = wlr_scene_buffer_create(&S.scene->tree, &S.splashbuf.base);
	wlr_scene_node_set_position(&S.splash_bird->node, SPLASH_X, SPLASH_Y);
}

static void splash_raise(void) { // above anything apply_foreground raised
	if (!S.splash_black) return;
	wlr_scene_node_raise_to_top(&S.splash_black->node);
	wlr_scene_node_raise_to_top(&S.splash_bird->node);
}

static int splash_step(void *data) {
	uint64_t t = mono_ms() - S.splash_t0;
	if (t < SPLASH_HOLD_MS) {
		wl_event_source_timer_update(S.splash_timer, (int)(SPLASH_HOLD_MS - t));
		return 0;
	}
	double f = (double)(t - SPLASH_HOLD_MS) / SPLASH_FADE_MS;
	if (f >= 1) { // gone
		wlr_scene_node_destroy(&S.splash_black->node);
		wlr_scene_node_destroy(&S.splash_bird->node);
		S.splash_black = NULL;
		S.splash_bird = NULL;
		wl_event_source_remove(S.splash_timer);
		S.splash_timer = NULL;
		return 0;
	}
	float o = (float)(1 - (f < 0.5 ? 2 * f * f : 1 - 2 * (1 - f) * (1 - f))); // eased in and out
	float black[4] = { 0, 0, 0, o };
	wlr_scene_rect_set_color(S.splash_black, black);
	wlr_scene_buffer_set_opacity(S.splash_bird, o);
	wl_event_source_timer_update(S.splash_timer, 16);
	return 0;
}

static void splash_shown(void) { // the first frame's on the screen: hold from now, then fade
	if (!S.splash_black || S.splash_timer) return;
	S.splash_t0 = mono_ms();
	S.splash_timer = wl_event_loop_add_timer(S.loop, splash_step, NULL);
	if (S.splash_timer) wl_event_source_timer_update(S.splash_timer, SPLASH_HOLD_MS);
}

static void splash_done(char *reply, void *ctx) {
	wlr_log(WLR_INFO, "boot splash: %s", helper_reply_status(reply));
}

static void output_frame(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(S.scene, o->wlr);
	shell_frame();
	wlr_scene_output_commit(so, NULL);
	// The boot splash (Plymouth, put aside as sprite.service started, its picture still up)
	// goes once Sprite's first frame is on the screen in its place: only root may end it
	static bool splash_ended;
	if (!splash_ended) {
		splash_ended = true;
		helper_request("splash-done\n", splash_done, NULL);
		splash_shown();
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *e = data;
	wlr_output_commit_state(o->wlr, e->state);
}
static void output_destroy(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, destroy);
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->present.link);
	if (S.output == o->wlr) S.output = NULL;
	free(o);
}

static void new_output(struct wl_listener *l, void *data) {
	struct wlr_output *wo = data;
	wlr_output_init_render(wo, S.allocator, S.renderer);
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
	if (mode) wlr_output_state_set_mode(&st, mode);
	wlr_output_commit_state(wo, &st);
	wlr_output_state_finish(&st);
	struct output *o = calloc(1, sizeof(*o));
	o->wlr = wo;
	o->frame.notify = output_frame;
	wl_signal_add(&wo->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wo->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wo->events.destroy, &o->destroy);
	o->present.notify = output_present;
	wl_signal_add(&wo->events.present, &o->present);
	struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(S.layout, wo);
	struct wlr_scene_output *so = wlr_scene_output_create(S.scene, wo);
	wlr_scene_output_layout_add_output(S.scene_layout, lo, so);
	S.output = wo;
	wlr_log(WLR_INFO, "output %s %dx%d@%.1f", wo->name, wo->width, wo->height, wo->refresh / 1000.0);
}

/* ---- children ---- */

// SIGUSR1: save the shell as it's drawn (not apps) to $XDG_RUNTIME_DIR/sprite-shot.ppm, for
// looking at the UI without a camera. 'pkill -USR1 -x sprite' as the of user. (SIGUSR2, below,
// saves the whole screen, apps and all.)
static int on_screenshot(int sig, void *data) {
	char path[256];
	snprintf(path, sizeof path, "%s/sprite-shot.ppm", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	fprintf(f, "P6\n%d %d\n255\n", SPRITE_W, SPRITE_H);
	for (int i = 0; i < SPRITE_W * SPRITE_H; i++) {
		uint32_t p = S.shellbuf.pixels[i];
		unsigned char rgb[3] = { (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	wlr_log(WLR_INFO, "screenshot: %s", path);
	return 0;
}

// SIGUSR2: the whole screen as it shows, apps and all, to $XDG_RUNTIME_DIR/sprite-screen.ppm:
// every buffer in the scene that's shown, bottom to top, blended by its opacity.
struct screen_shot { uint32_t *px; uint32_t *tmp; };
static void screen_blend(struct wlr_scene_buffer *sb, int sx, int sy, void *data) {
	struct screen_shot *ss = data;
	int w, h;
	const uint32_t *src;
	int stride;
	struct wlr_fbox box = sb->src_box;
	if (sb == S.shell_node || sb == S.handle) {
		struct shell_buffer *b = sb == S.shell_node ? &S.shellbuf : &S.handlebuf;
		w = b->base.width;
		h = b->base.height;
		src = b->pixels;
		stride = w;
		if (!wlr_fbox_empty(&box)) { // (the bar strip)
			src += (int)box.y * stride + (int)box.x;
			w = (int)box.width;
			h = (int)box.height;
		}
	} else {
		// an app's buffer has its texture in its client buffer; others, in the scene's
		struct wlr_client_buffer *cb = sb->buffer ? wlr_client_buffer_get(sb->buffer) : NULL;
		struct wlr_texture *tex = cb ? cb->texture : sb->texture;
		if (!tex) return; // not drawn yet
		w = tex->width;
		h = tex->height;
		if (w > SPRITE_W || h > SPRITE_H) return;
		if (!wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
			    .data = ss->tmp, .format = DRM_FORMAT_ARGB8888, .stride = w * 4 })) return;
		src = ss->tmp;
		stride = w;
	}
	bool opaque = sb == S.shell_node; // (XRGB: its alpha byte means nothing)
	float op = sb->opacity;
	for (int y = 0; y < h; y++) {
		int ty = sy + y;
		if (ty < 0 || ty >= SPRITE_H) continue;
		for (int x = 0; x < w; x++) {
			int tx = sx + x;
			if (tx < 0 || tx >= SPRITE_W) continue;
			uint32_t s = src[y * stride + x], *d = &ss->px[ty * SPRITE_W + tx];
			float a = (opaque ? 1.0f : ((s >> 24) & 0xff) / 255.0f) * op;
			uint32_t o = 0;
			for (int c = 0; c < 24; c += 8) { // premultiplied: src * opacity + dst * (1 - a)
				float v = ((s >> c) & 0xff) * op + ((*d >> c) & 0xff) * (1 - a);
				o |= (uint32_t)(v > 255 ? 255 : v + 0.5f) << c;
			}
			*d = o;
		}
	}
}

static int on_screen_shot(int sig, void *data) {
	struct screen_shot ss = { calloc(SPRITE_W * SPRITE_H, 4), calloc(SPRITE_W * SPRITE_H, 4) };
	if (!ss.px || !ss.tmp) { free(ss.px); free(ss.tmp); return 0; }
	wlr_scene_node_for_each_buffer(&S.scene->tree.node, screen_blend, &ss);
	char path[256];
	snprintf(path, sizeof path, "%s/sprite-screen.ppm", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
	FILE *f = fopen(path, "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", SPRITE_W, SPRITE_H);
		for (int i = 0; i < SPRITE_W * SPRITE_H; i++) {
			uint32_t p = ss.px[i];
			unsigned char rgb[3] = { (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff };
			fwrite(rgb, 1, 3, f);
		}
		fclose(f);
		wlr_log(WLR_INFO, "screenshot: %s", path);
	}
	free(ss.px);
	free(ss.tmp);
	return 0;
}

static int on_terminate(int sig, void *data) {
	wlr_log(WLR_INFO, "signal %d: stopping", sig);
	wl_display_terminate(S.display);
	return 0;
}

static int on_sigchld(int sig, void *data) {
	// Helpers and settings calls end in SIGCHLD too: only an app's change of state redraws
	if (apps_reap()) shell_apps_changed();
	return 0;
}

// Sandboxed apps reach the display through sockets of the sprite-apps group, the one group
// they share with sprite (see sprite-helper): a Wayland socket of their own, in
// /run/sprite-apps (made by the package's tmpfiles.d, setgid sprite-apps), and Xwayland's.
#define APP_SOCKET_DIR "/run/sprite-apps"
static void open_display_to_apps(struct wl_display *display, const char *x_display) {
	struct group *g = getgrnam("sprite-apps");
	if (!g) { wlr_log(WLR_ERROR, "no sprite-apps group: sandboxed apps can't reach the display"); return; }
	const char *path = APP_SOCKET_DIR "/wayland-0";
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	unlink(path);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || chown(path, -1, g->gr_gid) != 0 ||
	    chmod(path, 0660) != 0 || listen(fd, 128) != 0 || wl_display_add_socket_fd(display, fd) != 0) {
		wlr_log(WLR_ERROR, "no Wayland socket for apps at %s: %s", path, strerror(errno));
		if (fd >= 0) close(fd);
	} else {
		wlr_log(WLR_INFO, "Wayland socket for apps at %s", path);
	}
	// Xwayland's socket (wlroots made it, as us): the group may connect, as well as its owner.
	// X then decides who it lets in, and the X keeper lets in the group.
	if (x_display && x_display[0] == ':') {
		char x[64];
		snprintf(x, sizeof x, "/tmp/.X11-unix/X%s", x_display + 1);
		if (chown(x, -1, g->gr_gid) != 0 || chmod(x, 0770) != 0)
			wlr_log(WLR_ERROR, "%s can't be opened to apps: %s", x, strerror(errno));
	}
}

// The Wayland socket must be reachable by the unprivileged user apps run as.
static void prepare_runtime_dir(uid_t uid, gid_t gid) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir) { dir = "/run/sprite"; setenv("XDG_RUNTIME_DIR", dir, 1); }
	mkdir(dir, 0700);
	if (chown(dir, uid, gid) != 0) wlr_log(WLR_ERROR, "chown %s failed", dir);
	chmod(dir, 0700);
}

int main(int argc, char **argv) {
	// No hangups: Sprite has tty1, and the boot splash, which had it before, ending as Sprite
	// starts could send one (it ended a Sprite still starting at boot, which systemd took for
	// a clean exit and didn't restart)
	signal(SIGHUP, SIG_IGN);
	// Block SIGCHLD before LVGL starts its render threads, so the event loop's signalfd
	// receives it rather than whichever thread the kernel picks.
	sigset_t chld;
	sigemptyset(&chld);
	sigaddset(&chld, SIGCHLD);
	sigaddset(&chld, SIGTERM);
	sigaddset(&chld, SIGINT);
	sigaddset(&chld, SIGUSR1);
	sigaddset(&chld, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &chld, NULL);
	wlr_log_init(getenv("SPRITE_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);
	const char *apps_dir = getenv("SPRITE_APPS");
	// sprite's own, then those packages install, then local ones (demo apps, apps added by hand)
	if (!apps_dir) apps_dir = "/opt/sprite/share/sprite/apps:/usr/share/sprite/apps:/usr/local/share/sprite/apps";
	struct passwd *pw = getpwnam(getenv("SPRITE_USER") ? getenv("SPRITE_USER") : "sprite");
	if (pw) prepare_runtime_dir(pw->pw_uid, pw->pw_gid);

	wl_list_init(&S.views);
	wl_list_init(&S.keyboards);
	pixman_region32_init(&S.shell_damage);
	S.display = wl_display_create();
	S.loop = wl_display_get_event_loop(S.display);
	S.backend = wlr_backend_autocreate(S.loop, &S.session);
	if (!S.backend) { wlr_log(WLR_ERROR, "no backend"); return 1; }
	S.renderer = wlr_renderer_autocreate(S.backend);
	if (!S.renderer) { wlr_log(WLR_ERROR, "no renderer"); return 1; }
	wlr_renderer_init_wl_display(S.renderer, S.display);
	S.allocator = wlr_allocator_autocreate(S.backend, S.renderer);
	if (!S.allocator) { wlr_log(WLR_ERROR, "no allocator"); return 1; }

	S.compositor = wlr_compositor_create(S.display, 6, S.renderer);
	wlr_subcompositor_create(S.display);
	wlr_data_device_manager_create(S.display);
	wlr_viewporter_create(S.display);
	wlr_presentation_create(S.display, S.backend);

	S.layout = wlr_output_layout_create(S.display);
	S.new_output.notify = new_output;
	wl_signal_add(&S.backend->events.new_output, &S.new_output);
	S.scene = wlr_scene_create();
	S.scene_layout = wlr_scene_attach_output_layout(S.scene, S.layout);

	// Scene: black background, apps, then the shell on top (disabled while an app shows).
	float black[4] = { 0, 0, 0, 1 };
	wlr_scene_rect_create(&S.scene->tree, SPRITE_W, SPRITE_H, black);
	S.apps_tree = wlr_scene_tree_create(&S.scene->tree);
	S.shellbuf.pixels = calloc(SPRITE_W * SPRITE_H, 4);
	S.shellbuf.format = DRM_FORMAT_XRGB8888;
	wlr_buffer_init(&S.shellbuf.base, &shellbuf_impl, SPRITE_W, SPRITE_H);
	S.shell_node = wlr_scene_buffer_create(&S.scene->tree, &S.shellbuf.base);
	S.handlebuf.pixels = calloc(SPRITE_HANDLE_W * SPRITE_HANDLE_H, 4);
	S.handlebuf.format = DRM_FORMAT_ARGB8888;
	draw_handle(S.handlebuf.pixels, SPRITE_HANDLE_W, SPRITE_HANDLE_H);
	wlr_buffer_init(&S.handlebuf.base, &shellbuf_impl, SPRITE_HANDLE_W, SPRITE_HANDLE_H);
	S.handle = wlr_scene_buffer_create(&S.scene->tree, &S.handlebuf.base);
	S.trace_input = getenv("SPRITE_TRACE_INPUT") && atoi(getenv("SPRITE_TRACE_INPUT"));
	S.trace_perf = getenv("SPRITE_TRACE_PERF") ? atoi(getenv("SPRITE_TRACE_PERF")) : 0;
	S.home_bottom = !getenv("SPRITE_HOME_EDGE") || strcmp(getenv("SPRITE_HOME_EDGE"), "top");
	wlr_scene_node_set_position(&S.handle->node, (SPRITE_W - SPRITE_HANDLE_W) / 2,
		S.home_bottom ? SPRITE_H - SPRITE_HANDLE_Y - SPRITE_HANDLE_H : SPRITE_HANDLE_Y);
	wlr_scene_node_set_enabled(&S.handle->node, false);
	splash_build();

	S.idle_inhibit = wlr_idle_inhibit_v1_create(S.display);
	S.new_inhibitor.notify = new_inhibitor;
	wl_signal_add(&S.idle_inhibit->events.new_inhibitor, &S.new_inhibitor);
	S.last_input_ms = mono_ms(); // idle from the start, as if touched then

	S.xdg_shell = wlr_xdg_shell_create(S.display, 3);
	S.new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&S.xdg_shell->events.new_toplevel, &S.new_xdg_toplevel);
	S.new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&S.xdg_shell->events.new_popup, &S.new_xdg_popup);

	S.cursor = wlr_cursor_create(); // no image: touch needs no pointer on screen
	wlr_cursor_attach_output_layout(S.cursor, S.layout);
	S.cursor_motion.notify = cursor_motion;
	wl_signal_add(&S.cursor->events.motion, &S.cursor_motion);
	S.cursor_motion_abs.notify = cursor_motion_abs;
	wl_signal_add(&S.cursor->events.motion_absolute, &S.cursor_motion_abs);
	S.cursor_button.notify = cursor_button;
	wl_signal_add(&S.cursor->events.button, &S.cursor_button);
	S.cursor_axis.notify = cursor_axis;
	wl_signal_add(&S.cursor->events.axis, &S.cursor_axis);
	S.cursor_frame.notify = cursor_frame;
	wl_signal_add(&S.cursor->events.frame, &S.cursor_frame);
	S.touch_down.notify = touch_down;
	wl_signal_add(&S.cursor->events.touch_down, &S.touch_down);
	S.touch_up.notify = touch_up;
	wl_signal_add(&S.cursor->events.touch_up, &S.touch_up);
	S.touch_motion.notify = touch_motion;
	wl_signal_add(&S.cursor->events.touch_motion, &S.touch_motion);
	S.touch_frame.notify = touch_frame;
	wl_signal_add(&S.cursor->events.touch_frame, &S.touch_frame);

	S.new_input.notify = new_input;
	wl_signal_add(&S.backend->events.new_input, &S.new_input);
	S.seat = wlr_seat_create(S.display, "seat0");

	S.xwayland = wlr_xwayland_create(S.display, S.compositor, true); // started on first X client
	if (S.xwayland) {
		S.new_xw_surface.notify = new_xw_surface;
		wl_signal_add(&S.xwayland->events.new_surface, &S.new_xw_surface);
		S.xw_ready.notify = xw_ready;
		wl_signal_add(&S.xwayland->events.ready, &S.xw_ready);
	}

	const char *socket = wl_display_add_socket_auto(S.display);
	if (!socket) { wlr_log(WLR_ERROR, "no socket"); return 1; }
	if (pw) {
		char path[256];
		snprintf(path, sizeof path, "%s/%s", getenv("XDG_RUNTIME_DIR"), socket);
		if (chown(path, pw->pw_uid, pw->pw_gid) != 0) wlr_log(WLR_ERROR, "chown %s failed", path);
	}
	apps_set_env(socket, S.xwayland ? S.xwayland->display_name : NULL);
	if (apps_sandboxed()) open_display_to_apps(S.display, S.xwayland ? S.xwayland->display_name : NULL);

	// LVGL: the shell renders into the buffer above, on this thread, from the event loop.
	settings_load(); // the shell reads some (the clock's format) as it's built
	load_calibration();
	shell_init(S.shellbuf.pixels);
	S.lv_timer = wl_event_loop_add_timer(S.loop, lv_timer_cb, NULL);
	wl_event_source_timer_update(S.lv_timer, 1);
	apps_load(apps_dir);
	shell_apps_changed();
	S.hold_timer = wl_event_loop_add_timer(S.loop, hold_timeout, NULL);
	struct sprite_app *start = apps_autostart();
	if (start) {
		// open it "clean": no home screen first, just black until its window appears
		wlr_log(WLR_INFO, "opening %s at startup", start->name);
		sprite_hold_for(start);
		sprite_open_app(start);
	}
	S.sigchld = wl_event_loop_add_signal(S.loop, SIGCHLD, on_sigchld, NULL);
	wl_event_loop_add_signal(S.loop, SIGTERM, on_terminate, NULL);
	wl_event_loop_add_signal(S.loop, SIGINT, on_terminate, NULL);
	wl_event_loop_add_signal(S.loop, SIGUSR1, on_screenshot, NULL);
	wl_event_loop_add_signal(S.loop, SIGUSR2, on_screen_shot, NULL);
	S.release_timer = wl_event_loop_add_timer(S.loop, release_timer_cb, NULL);
	S.handle_timer = wl_event_loop_add_timer(S.loop, handle_timer_cb, NULL);

	if (!wlr_backend_start(S.backend)) { wlr_log(WLR_ERROR, "backend start failed"); return 1; }
	apply_foreground();
	wlr_log(WLR_INFO, "sprite %s running on %s, X on %s", SPRITE_VERSION, socket,
		S.xwayland ? S.xwayland->display_name : "(none)");
	wl_display_run(S.display);

	apps_end_all();
	wl_display_destroy_clients(S.display);
	if (S.xwayland) wlr_xwayland_destroy(S.xwayland);
	wlr_scene_node_destroy(&S.scene->tree.node);
	wlr_cursor_destroy(S.cursor);
	wlr_allocator_destroy(S.allocator);
	wlr_renderer_destroy(S.renderer);
	wlr_backend_destroy(S.backend);
	wl_display_destroy(S.display);
	return 0;
}
