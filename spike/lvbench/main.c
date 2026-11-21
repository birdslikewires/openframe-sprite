// lvbench: run an LVGL demo on fbdev or Wayland. usage: lvbench fb|wl bench|widgets
#include <stdio.h>
#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
static void done(const lv_demo_benchmark_summary_t *s) { lv_demo_benchmark_summary_display(s); fflush(stdout); exit(0); }
int main(int argc, char **argv) {
	int wl = argc > 1 && !strcmp(argv[1], "wl");
	int bench = argc < 3 || !strcmp(argv[2], "bench");
	lv_init();
	lv_display_t *disp;
	if (wl) {
		disp = lv_wayland_window_create(800, 480, "lvbench", NULL);
		lv_wayland_window_set_fullscreen(disp, true);
	} else {
		disp = lv_linux_fbdev_create(); lv_linux_fbdev_set_file(disp, "/dev/fb0");
		lv_indev_t *t = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
		if (t) lv_evdev_set_calibration(t, 0, 0, 32639, 32639);
	}
	(void)disp;
	if (bench) { lv_demo_benchmark_set_end_cb(done); lv_demo_benchmark(); } else lv_demo_widgets();
	for (;;) { uint32_t ms = wl ? lv_wayland_timer_handler() : lv_timer_handler(); if (ms > 50) ms = 50; usleep(ms * 1000); }
}
