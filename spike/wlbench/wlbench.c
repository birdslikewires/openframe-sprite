// wlbench: fullscreen shm client that animates and records wp_presentation feedback.
// usage: wlbench <frames> <mode: full|small|idle>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wp_presentation *pres;
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static int W = 800, H = 480, configured, frames_wanted, frames_done, presented, discarded;
static uint32_t *pix[2];
static struct wl_buffer *buf[2];
static int cur, mode; // 0 full, 1 small, 2 idle
static double ptimes[4096];
static uint32_t pflags_or, pflags_and = ~0u;
static double draw_ms_total;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
	if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(iface, wp_presentation_interface.name)) pres = wl_registry_bind(r, name, &wp_presentation_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t s) { xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
static void xs_conf(void *d, struct xdg_surface *s, uint32_t serial) { xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_conf };
static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { if (w > 0 && h > 0) { W = w; H = h; } }
static void top_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener top_l = { top_conf, top_close };

static void fb_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) {}
static void fb_presented(void *d, struct wp_presentation_feedback *f, uint32_t hi, uint32_t lo, uint32_t nsec,
			 uint32_t refresh, uint32_t shi, uint32_t slo, uint32_t flags) {
	double t = (((uint64_t)hi << 32) | lo) * 1e3 + nsec / 1e6;
	if (presented < 4096) ptimes[presented] = t;
	presented++; pflags_or |= flags; pflags_and &= flags;
	wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *d, struct wp_presentation_feedback *f) { discarded++; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener fb_l = { fb_sync, fb_presented, fb_discarded };

static struct wl_buffer *mkbuf(uint32_t **out) {
	int stride = W * 4, size = stride * H;
	int fd = memfd_create("wlbench", 0); ftruncate(fd, size);
	*out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *p = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(p, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(p); close(fd); return b;
}

static void draw_and_commit(void);
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) { wl_callback_destroy(cb); if (frames_done < frames_wanted) draw_and_commit(); }
static const struct wl_callback_listener frame_l = { frame_done };

static void draw_and_commit(void) {
	uint32_t *p = pix[cur];
	double t0 = now_ms();
	int x = (frames_done * 8) % W;
	if (mode == 0) { // full redraw: gradient + moving bar
		static uint32_t *bgf; if (!bgf) { bgf = malloc(W * H * 4); for (int y = 0; y < H; y++) for (int i = 0; i < W; i++) bgf[y * W + i] = ((y * 255 / H) << 8) | (i * 255 / W); }
		memcpy(p, bgf, W * H * 4); // full repaint, like a page transition
		for (int y = 0; y < H; y++) { uint32_t *row = p + y * W; for (int i = x; i < x + 40 && i < W; i++) row[i] = 0xffffffff; }
		wl_surface_damage_buffer(surf, 0, 0, W, H);
	} else { // small: 64x64 square moving over a static background
		static int bg[2]; if (!bg[cur]) { for (int i = 0; i < W * H; i++) p[i] = 0xff203040; bg[cur] = 1; }
		int px = (frames_done * 8) % (W - 64), ox = ((frames_done - 2) * 8) % (W - 64); if (frames_done < 2) ox = px;
		for (int y = 100; y < 164; y++) { for (int i = ox; i < ox + 72 && i < W; i++) p[y * W + i] = 0xff203040; for (int i = px; i < px + 64; i++) p[y * W + i] = 0xffffa000; }
		int lo = ox < px ? ox : px; wl_surface_damage_buffer(surf, lo, 100, 80 + abs(px - ox), 64);
	}
	draw_ms_total += now_ms() - t0;
	wl_surface_attach(surf, buf[cur], 0, 0);
	struct wl_callback *cb = wl_surface_frame(surf); wl_callback_add_listener(cb, &frame_l, NULL);
	if (pres) { struct wp_presentation_feedback *f = wp_presentation_feedback(pres, surf); wp_presentation_feedback_add_listener(f, &fb_l, NULL); }
	wl_surface_commit(surf);
	cur ^= 1; frames_done++;
}

static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }
static double cpu_s(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
	frames_wanted = argc > 1 ? atoi(argv[1]) : 300;
	mode = argc > 2 ? (!strcmp(argv[2], "small") ? 1 : !strcmp(argv[2], "idle") ? 2 : 0) : 0;
	struct wl_display *dpy = wl_display_connect(NULL); if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *r = wl_display_get_registry(dpy); wl_registry_add_listener(r, &reg_l, NULL); wl_display_roundtrip(dpy);
	xdg_wm_base_add_listener(wm, &wm_l, NULL);
	surf = wl_compositor_create_surface(comp); xsurf = xdg_wm_base_get_xdg_surface(wm, surf); xdg_surface_add_listener(xsurf, &xs_l, NULL);
	top = xdg_surface_get_toplevel(xsurf); xdg_toplevel_add_listener(top, &top_l, NULL); xdg_toplevel_set_fullscreen(top, NULL);
	wl_surface_commit(surf); while (!configured) wl_display_dispatch(dpy);
	printf("surface %dx%d presentation=%s\n", W, H, pres ? "yes" : "no");
	buf[0] = mkbuf(&pix[0]); buf[1] = mkbuf(&pix[1]);
	if (mode == 2) { // draw once, then sit idle so compositor idle CPU can be measured outside
		mode = 0; frames_wanted = 1; draw_and_commit(); wl_display_flush(dpy);
		double end = now_ms() + (argc > 3 ? atof(argv[3]) : 10) * 1e3; while (now_ms() < end) { wl_display_dispatch_pending(dpy); wl_display_flush(dpy); usleep(100000); }
		puts("idle done"); return 0;
	}
	double t0 = now_ms(), c0 = cpu_s();
	draw_and_commit();
	while (presented + discarded < frames_wanted && wl_display_dispatch(dpy) != -1);
	double el = now_ms() - t0, cpu = cpu_s() - c0;
	int n = presented < 4096 ? presented : 4096; double iv[4096]; int ni = 0;
	for (int i = 1; i < n; i++) iv[ni++] = ptimes[i] - ptimes[i - 1];
	qsort(iv, ni, sizeof(double), cmp);
	int over = 0; for (int i = 0; i < ni; i++) if (iv[i] > 20) over++;
	printf("frames %d presented %d discarded %d in %.0f ms = %.1f fps\n", frames_done, presented, discarded, el, presented * 1e3 / el);
	if (ni) printf("present interval ms: min %.2f p50 %.2f p90 %.2f p99 %.2f max %.2f; >20ms: %d\n", iv[0], iv[ni / 2], iv[ni * 9 / 10], iv[ni * 99 / 100], iv[ni - 1], over);
	printf("flags all-frames=0x%x any-frame=0x%x (1=VSYNC 2=HW_CLOCK 4=HW_COMPLETION 8=ZERO_COPY)\n", pflags_and, pflags_or);
	printf("client draw avg %.2f ms, client cpu %.2f s (%.0f%%)\n", draw_ms_total / frames_done, cpu, cpu * 1e5 / el);
	return 0;
}
