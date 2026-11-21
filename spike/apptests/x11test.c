// x11test: HamClock-shaped Xlib client. 800x480 window, full XPutImage per frame, N frames.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
int main(int argc, char **argv) {
	int frames = argc > 1 ? atoi(argv[1]) : 300, W = 800, H = 480;
	double t_start = now_ms();
	Display *d = XOpenDisplay(NULL); if (!d) { puts("no X display"); return 1; }
	printf("X connected after %.0f ms (vendor %s)\n", now_ms() - t_start, ServerVendor(d));
	int sc = DefaultScreen(d);
	Window w = XCreateSimpleWindow(d, RootWindow(d, sc), 0, 0, W, H, 0, 0, 0);
	XSelectInput(d, w, ExposureMask | ButtonPressMask);
	XMapWindow(d, w);
	XEvent e; do XNextEvent(d, &e); while (e.type != Expose);
	GC gc = DefaultGC(d, sc);
	char *px = malloc(W * H * 4);
	XImage *img = XCreateImage(d, DefaultVisual(d, sc), DefaultDepth(d, sc), ZPixmap, 0, px, W, H, 32, 0);
	double t0 = now_ms();
	for (int f = 0; f < frames; f++) {
		uint32_t *p = (uint32_t *)px; for (int i = 0; i < W * H; i++) p[i] = 0x203040;
		int x = (f * 8) % (W - 40); for (int y = 200; y < 280; y++) for (int i = x; i < x + 40; i++) p[y * W + i] = 0xffa000;
		XPutImage(d, w, gc, img, 0, 0, 0, 0, W, H);
		XSync(d, False);
		while (XPending(d)) { XNextEvent(d, &e); if (e.type == ButtonPress) printf("tap at %d,%d\n", e.xbutton.x, e.xbutton.y); }
	}
	double el = now_ms() - t0;
	printf("frames %d in %.0f ms = %.1f fps (full-frame XPutImage + XSync)\n", frames, el, frames * 1e3 / el);
	XCloseDisplay(d);
	return 0;
}
