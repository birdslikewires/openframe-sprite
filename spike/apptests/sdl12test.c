// sdl12test: SqueezePlay-shaped SDL 1.2 client. Full-screen software surface, redraw + flip N frames.
// usage: sdl12test <frames> [wait_tap_seconds]
#include <SDL/SDL.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
	int frames = argc > 1 ? atoi(argv[1]) : 300, tapwait = argc > 2 ? atoi(argv[2]) : 0;
	if (SDL_Init(SDL_INIT_VIDEO) < 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
	char drv[64] = "?"; SDL_VideoDriverName(drv, sizeof drv);
	SDL_Surface *s = SDL_SetVideoMode(800, 480, 32, SDL_SWSURFACE | SDL_FULLSCREEN);
	if (!s) { printf("SetVideoMode: %s\n", SDL_GetError()); return 1; }
	printf("SDL %d.%d.%d driver=%s surface %dx%d bpp %d\n", SDL_Linked_Version()->major, SDL_Linked_Version()->minor,
	       SDL_Linked_Version()->patch, drv, s->w, s->h, s->format->BitsPerPixel);
	Uint32 t0 = SDL_GetTicks();
	for (int f = 0; f < frames; f++) {
		SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 20, 30, 40));
		SDL_Rect r = { (Sint16)((f * 8) % 760), 200, 40, 80 };
		SDL_FillRect(s, &r, SDL_MapRGB(s->format, 255, 160, 0));
		SDL_Flip(s);
		SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_MOUSEBUTTONDOWN) printf("tap at %d,%d\n", e.button.x, e.button.y);
	}
	Uint32 el = SDL_GetTicks() - t0;
	printf("frames %d in %u ms = %.1f fps\n", frames, el, frames * 1000.0 / el);
	if (tapwait) { // show targets in the corners and centre, report where taps land
		SDL_FillRect(s, NULL, 0);
		int pts[5][2] = { {20, 20}, {780, 20}, {400, 240}, {20, 460}, {780, 460} };
		for (int i = 0; i < 5; i++) { SDL_Rect r = { (Sint16)(pts[i][0] - 10), (Sint16)(pts[i][1] - 10), 20, 20 }; SDL_FillRect(s, &r, SDL_MapRGB(s->format, 255, 255, 255)); }
		SDL_Flip(s); printf("tap the white squares (targets 20,20 780,20 400,240 20,460 780,460) for %d s\n", tapwait); fflush(stdout);
		Uint32 end = SDL_GetTicks() + tapwait * 1000;
		while (SDL_GetTicks() < end) { SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_MOUSEBUTTONDOWN) { printf("tap at %d,%d\n", e.button.x, e.button.y); fflush(stdout); } SDL_Delay(10); }
	}
	SDL_Quit();
	return 0;
}
