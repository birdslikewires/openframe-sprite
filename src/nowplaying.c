// SPDX-License-Identifier: GPL-3.0-or-later
// nowplaying.c: Now Playing, what the OpenFrame is playing, full screen: the artwork, the
// title, artist and album, where it's coming from, and how far through it is. It shows as the
// screensaver (when it's the one chosen, while something plays: the flip clock otherwise), or
// when the music note in the status bar is tapped; a touch ends it, as the screensaver's does.
//
// What's playing comes from the services playing it. Shairport Sync (ofapp-shairport-sync)
// sends its metadata to 127.0.0.1:5555 over UDP, in its own form: each item a 4-byte type and
// code then the data, items too big for a datagram in chunks ("ssnc" "chnk": the chunk's index,
// how many, then the item's type and code). AirPlay 1 senders give the title, artist and album
// as "core" items (minm, asar, asal) and the artwork as "ssnc" PICT; AirPlay 2 senders give
// them in Apple's now-playing messages, binary property lists that shairport-sync passes on as
// "ssnc" copl (updateMRNowPlayingInfo, with the artwork in it too). The session's start and end
// (abeg, aend), pauses (paus, pres, prsm) and the sender's name (snam) are "ssnc" items as well.
// Anything on this OpenFrame could send to that port; the worst it could do is show the wrong
// title, and everything read is checked against the datagram's length.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <jpeglib.h>
#include <png.h>
#include "lvgl.h"
#include "sprite.h"

extern const lv_font_t sprite_text_32, sprite_text_24, sprite_text_20, sprite_text_14, sprite_ui_16;
#include "ui_icons.h"

#define NP_PORT 5555
#define NP_ITEM_MAX (2 * 1024 * 1024) // the biggest item put together from chunks (artwork, in a plist)
#define NP_ART 360                    // the artwork's side on screen
#define NP_MARGIN 60                  // the artwork from the screen's top (and foot)
#define NP_LEFT 44                    // ... and from its left, as the text's room is from its right (text seldom fills its width, so with even margins everything looked shifted right)
#define NP_GAP 40                     // between it and the text
#define NP_TEXT_W (SPRITE_W - NP_LEFT - NP_ART - NP_GAP - 44)
#define NP_INSET 16                   // the text's top and the times' foot, in from the artwork's

#define C_NP_BG 0x131920
#define C_NP_TEXT 0xe8edf2
#define C_NP_DIM 0x9aa6b2
#define C_NP_TRACK 0x2a3440 // the progress bar's empty part
#define C_NP_CARD 0x1c242c  // where artwork would be, without any

/* ---- what's playing ---- */

static struct {
	bool active;            // a session's going (shairport-sync's active state: from playing till a while after it stops)
	bool playing;           // ... and not paused
	char title[256], artist[256], album[256], sender[128];
	char playlist[128];     // the playlist it's playing from (an AirPlay 2 sender's collection, when it isn't the album)
	bool live;              // a live stream (radio): no times, which mean nothing for one
	char item[96];          // the sender's id for what's playing (AirPlay 2's ContentItemIdentifier), to know a new one
	double duration;        // seconds, 0 if not known
	double elapsed;         // seconds into it, at 'at'
	uint32_t at;            // lv_tick when elapsed was so
	lv_draw_buf_t *art;     // NP_ART square, or NULL
	uint32_t art_hash;      // of the encoded artwork shown, so the same picture twice is decoded once
	uint32_t tint;          // the background's colour, from the artwork
	uint32_t stopped;       // lv_tick when it stopped playing (Paused shows only once it's lasted: a track change stops it for a moment)
} np;

#define NP_PAUSED_MS 1500
static bool np_log; // SPRITE_NP_LOG=1: each item that arrives, in the journal

static bool np_saver, np_opened, np_asleep; // shown as the screensaver; opened from the bar; the screen off
static int np_fd = -1;
static uint8_t *chunk_buf;
static size_t chunk_len;
static uint32_t chunk_next, chunk_count, chunk_type, chunk_code;

static void np_refresh(void);

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t code4(const char *s) { return be32((const uint8_t *)s); }

static uint32_t fnv(const uint8_t *p, size_t n) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
	return h;
}

// Text from a sender into one of ours: what fits, cut at a character's start, with control characters as spaces
static void set_str(char *dst, size_t n, const uint8_t *s, size_t len) {
	if (len >= n) {
		len = n - 1;
		while (len && (s[len] & 0xc0) == 0x80) len--;
	}
	for (size_t i = 0; i < len; i++) dst[i] = s[i] < 0x20 ? ' ' : (char)s[i];
	dst[len] = 0;
}

static void progress_at(double elapsed, double duration) {
	np.elapsed = elapsed < 0 ? 0 : elapsed;
	if (duration > 0) np.duration = duration;
	np.at = lv_tick_get();
}

static double elapsed_now(void) {
	double e = np.elapsed + (np.playing ? lv_tick_elaps(np.at) / 1000.0 : 0);
	return np.duration > 0 && e > np.duration ? np.duration : e;
}

static void set_playing(bool on) {
	if (np.playing && !on) np.stopped = lv_tick_get();
	np.playing = on;
}

static void clear_track(void) {
	np.title[0] = np.artist[0] = np.album[0] = np.playlist[0] = 0;
	np.live = false;
	np.duration = 0;
	progress_at(0, 0);
}

static void drop_art(void) {
	if (!np.art) return;
	lv_image_cache_drop(np.art);
	lv_draw_buf_destroy(np.art);
	np.art = NULL;
	np.art_hash = 0;
}

static void clear_all(void) {
	clear_track();
	np.item[0] = 0;
	np.active = np.playing = false;
	drop_art();
}

/* ---- artwork: JPEG or PNG, decoded (JPEG scaled down as it's decoded), then cropped to a
   square from its middle and fitted to NP_ART, once, so drawing it is a plain copy ---- */

struct jerr { struct jpeg_error_mgr mgr; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

// RGBA (as LVGL's ARGB8888 lies in memory: B, G, R, A), w x h, malloc'd
static uint8_t *decode_jpeg(const uint8_t *d, size_t n, int *w, int *h) {
	struct jpeg_decompress_struct c;
	struct jerr e;
	uint8_t *volatile px = NULL;
	c.err = jpeg_std_error(&e.mgr);
	e.mgr.error_exit = jerr_exit;
	if (setjmp(e.jb)) {
		jpeg_destroy_decompress(&c);
		free(px);
		return NULL;
	}
	jpeg_create_decompress(&c);
	jpeg_mem_src(&c, d, n);
	jpeg_read_header(&c, TRUE);
	// scaled by M/8 as it's decoded, to the smallest that still covers NP_ART
	unsigned side = c.image_width < c.image_height ? c.image_width : c.image_height;
	unsigned m = side ? (NP_ART * 8 + side - 1) / side : 8;
	c.scale_num = m < 1 ? 1 : m > 8 ? 8 : m;
	c.scale_denom = 8;
	c.out_color_space = JCS_EXT_BGRA;
	jpeg_start_decompress(&c);
	if (c.output_width > 8192 || c.output_height > 8192) longjmp(e.jb, 1);
	*w = c.output_width;
	*h = c.output_height;
	px = malloc((size_t)*w * *h * 4);
	if (!px) longjmp(e.jb, 1);
	while (c.output_scanline < c.output_height) {
		JSAMPROW row = px + (size_t)c.output_scanline * *w * 4;
		jpeg_read_scanlines(&c, &row, 1);
	}
	jpeg_finish_decompress(&c);
	jpeg_destroy_decompress(&c);
	return px;
}

static uint8_t *decode_png(const uint8_t *d, size_t n, int *w, int *h) {
	png_image im = { .version = PNG_IMAGE_VERSION };
	if (!png_image_begin_read_from_memory(&im, d, n)) return NULL;
	if (im.width > 4096 || im.height > 4096) { png_image_free(&im); return NULL; }
	im.format = PNG_FORMAT_BGRA;
	uint8_t *px = malloc(PNG_IMAGE_SIZE(im));
	if (!px || !png_image_finish_read(&im, NULL, px, 0, NULL)) { free(px); png_image_free(&im); return NULL; }
	*w = im.width;
	*h = im.height;
	return px;
}

static void set_art(const uint8_t *d, size_t n) {
	if (n < 8) return;
	uint32_t hash = fnv(d, n);
	if (np.art && hash == np.art_hash) return;
	int w = 0, h = 0;
	uint8_t *px = d[0] == 0xff && d[1] == 0xd8 ? decode_jpeg(d, n, &w, &h) :
		!memcmp(d, "\x89PNG", 4) ? decode_png(d, n, &w, &h) : NULL;
	if (!px || w < 1 || h < 1) { free(px); return; }
	lv_draw_buf_t *b = lv_draw_buf_create(NP_ART, NP_ART, LV_COLOR_FORMAT_ARGB8888, 0);
	if (!b) { free(px); return; }
	// the middle square, each of ours the average of the pixels it covers (or the one it's in)
	int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2;
	uint64_t sum[3] = { 0 };
	for (int y = 0; y < NP_ART; y++) {
		int sy0 = y0 + y * side / NP_ART, sy1 = y0 + (y + 1) * side / NP_ART;
		if (sy1 <= sy0) sy1 = sy0 + 1;
		uint8_t *out = b->data + (size_t)y * b->header.stride;
		for (int x = 0; x < NP_ART; x++) {
			int sx0 = x0 + x * side / NP_ART, sx1 = x0 + (x + 1) * side / NP_ART;
			if (sx1 <= sx0) sx1 = sx0 + 1;
			uint32_t acc[4] = { 0 }, cnt = 0;
			for (int sy = sy0; sy < sy1; sy++)
				for (int sx = sx0; sx < sx1; sx++) {
					const uint8_t *p = px + ((size_t)sy * w + sx) * 4;
					acc[0] += p[0], acc[1] += p[1], acc[2] += p[2], acc[3] += p[3], cnt++;
				}
			for (int k = 0; k < 4; k++) out[x * 4 + k] = acc[k] / cnt;
			for (int k = 0; k < 3; k++) sum[k] += out[x * 4 + k];
		}
	}
	free(px);
	if (np.art) {
		lv_image_cache_drop(np.art);
		lv_draw_buf_destroy(np.art);
	}
	np.art = b;
	np.art_hash = hash;
	// the background: the picture's average colour, darkened well under the text
	uint32_t n2 = NP_ART * NP_ART, bl = sum[0] / n2, g = sum[1] / n2, r = sum[2] / n2;
	np.tint = (r * 35 / 100 + 0x13 * 65 / 100) << 16 | (g * 35 / 100 + 0x19 * 65 / 100) << 8 | (bl * 35 / 100 + 0x20 * 65 / 100);
}

/* ---- binary property lists (Apple's bplist00), enough to read a now-playing message: a
   dictionary's values by key, and strings, numbers, dates and data. Every offset is checked
   against the list's length. ---- */

struct bp {
	const uint8_t *d;
	size_t n;
	unsigned osize, rsize;  // the offset table's integers, and object references, in bytes
	uint64_t count, table;  // objects, and where the offset table is
};

static uint64_t be_n(const uint8_t *p, unsigned k) {
	uint64_t v = 0;
	for (unsigned i = 0; i < k; i++) v = v << 8 | p[i];
	return v;
}

static bool bp_open(struct bp *b, const uint8_t *d, size_t n, uint64_t *top) {
	if (n < 40 || memcmp(d, "bplist00", 8)) return false;
	const uint8_t *t = d + n - 32;
	b->d = d, b->n = n, b->osize = t[6], b->rsize = t[7];
	b->count = be_n(t + 8, 8), *top = be_n(t + 16, 8), b->table = be_n(t + 24, 8);
	if (!b->osize || b->osize > 8 || !b->rsize || b->rsize > 8 || *top >= b->count) return false;
	if (b->table >= n || b->count > (n - b->table) / b->osize) return false;
	return true;
}

// Where object i starts, or 0 if it's out of bounds (an object never starts at 0: the header's there)
static size_t bp_at(const struct bp *b, uint64_t i) {
	if (i >= b->count) return 0;
	uint64_t o = be_n(b->d + b->table + i * b->osize, b->osize);
	return o >= 8 && o < b->n ? (size_t)o : 0;
}

// An object's marker type (its high nibble), and its count and where its contents start
static int bp_head(const struct bp *b, uint64_t i, uint64_t *count, size_t *body) {
	size_t o = bp_at(b, i);
	if (!o) return -1;
	int type = b->d[o] >> 4;
	uint64_t c = b->d[o] & 15;
	size_t p = o + 1;
	if (c == 15 && type != 0 && type != 1 && type != 2 && type != 3) { // the count follows, as an int
		if (p >= b->n || b->d[p] >> 4 != 1) return -1;
		unsigned k = 1u << (b->d[p] & 15);
		if (k > 8 || p + 1 + k > b->n) return -1;
		c = be_n(b->d + p + 1, k);
		p += 1 + k;
	}
	*count = c;
	*body = p;
	return type;
}

static bool bp_str(const struct bp *b, uint64_t i, char *out, size_t n) {
	uint64_t c;
	size_t p;
	int t = bp_head(b, i, &c, &p);
	if (t == 5) { // ASCII
		if (c > b->n - p) return false;
		set_str(out, n, b->d + p, c);
		return true;
	}
	if (t == 6) { // UTF-16, big-endian: into UTF-8
		if (c > (b->n - p) / 2) return false;
		size_t o = 0;
		for (uint64_t k = 0; k < c; k++) {
			uint32_t u = b->d[p + 2 * k] << 8 | b->d[p + 2 * k + 1];
			if (u >= 0xd800 && u < 0xdc00 && k + 1 < c) { // a surrogate pair
				uint32_t lo = b->d[p + 2 * k + 2] << 8 | b->d[p + 2 * k + 3];
				if (lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); k++; }
			}
			char e[4];
			int m = u < 0x80 ? (e[0] = u < 0x20 ? ' ' : u, 1) :
				u < 0x800 ? (e[0] = 0xc0 | u >> 6, e[1] = 0x80 | (u & 63), 2) :
				u < 0x10000 ? (e[0] = 0xe0 | u >> 12, e[1] = 0x80 | (u >> 6 & 63), e[2] = 0x80 | (u & 63), 3) :
				(e[0] = 0xf0 | u >> 18, e[1] = 0x80 | (u >> 12 & 63), e[2] = 0x80 | (u >> 6 & 63), e[3] = 0x80 | (u & 63), 4);
			if (o + m >= n) break;
			memcpy(out + o, e, m);
			o += m;
		}
		out[o] = 0;
		return true;
	}
	return false;
}

// A number: an integer, a real, or a date (seconds since 2001), as a double
static bool bp_num(const struct bp *b, uint64_t i, double *v) {
	size_t o = bp_at(b, i);
	if (!o) return false;
	int t = b->d[o] >> 4;
	unsigned k = 1u << (b->d[o] & 15);
	if (k > 8 || o + 1 + k > b->n) return false;
	uint64_t raw = be_n(b->d + o + 1, k);
	if (t == 1) { *v = k == 8 ? (double)(int64_t)raw : (double)raw; return true; }
	if ((t == 2 || t == 3) && k == 8) { double x; memcpy(&x, &raw, 8); *v = x; return true; }
	if (t == 2 && k == 4) { uint32_t r32 = raw; float f; memcpy(&f, &r32, 4); *v = f; return true; }
	return false;
}

static bool bp_data(const struct bp *b, uint64_t i, const uint8_t **d, size_t *n) {
	uint64_t c;
	size_t p;
	if (bp_head(b, i, &c, &p) != 4 || c > b->n - p) return false;
	*d = b->d + p;
	*n = c;
	return true;
}

// A dictionary's value for a key, as its object
static bool bp_get(const struct bp *b, uint64_t dict, const char *key, uint64_t *val) {
	uint64_t c;
	size_t p;
	if (bp_head(b, dict, &c, &p) != 13 || c > (b->n - p) / (2 * b->rsize)) return false;
	char k[96];
	for (uint64_t j = 0; j < c; j++) {
		if (!bp_str(b, be_n(b->d + p + j * b->rsize, b->rsize), k, sizeof k) || strcmp(k, key)) continue;
		*val = be_n(b->d + p + (c + j) * b->rsize, b->rsize);
		return true;
	}
	return false;
}

#define MR "kMRMediaRemoteNowPlayingInfo"

// One of Apple's now-playing messages (shairport-sync's ssnc copl)
// True if it says it's playing (which starts a session: an iPhone that's connected but idle sends messages too, and they mustn't)
static bool now_playing_plist(const uint8_t *d, size_t n) {
	struct bp b;
	uint64_t top, o, params, info;
	char s[64];
	if (!bp_open(&b, d, n, &top) || !bp_get(&b, top, "type", &o) || !bp_str(&b, o, s, sizeof s)) return false;
	if (strcmp(s, "updateMRNowPlayingInfo") || !bp_get(&b, top, "params", &params) || !bp_get(&b, params, "params", &info)) return false;
	// "replace" starts a new track's details; "update" changes some of them
	if (bp_get(&b, params, "mergePolicy", &o) && bp_str(&b, o, s, sizeof s) && !strcmp(s, "replace")) clear_track();
	// a different item, without a picture of its own: not the last one's
	char item[sizeof np.item];
	if (bp_get(&b, info, MR "ContentItemIdentifier", &o) && bp_str(&b, o, item, sizeof item) && strcmp(item, np.item)) {
		if (np.item[0] && !bp_get(&b, info, MR "ArtworkData", &o)) drop_art();
		snprintf(np.item, sizeof np.item, "%s", item);
	}
	if (bp_get(&b, info, MR "Title", &o)) bp_str(&b, o, np.title, sizeof np.title);
	if (bp_get(&b, info, MR "Artist", &o)) bp_str(&b, o, np.artist, sizeof np.artist);
	if (bp_get(&b, info, MR "Album", &o)) bp_str(&b, o, np.album, sizeof np.album);
	uint64_t coll;
	if (bp_get(&b, info, MR "CollectionInfo", &coll) && bp_get(&b, coll, "kMRMediaRemoteNowPlayingCollectionInfoKeyTitle", &o)) {
		bp_str(&b, o, np.playlist, sizeof np.playlist);
		if (!strcmp(np.playlist, np.album)) np.playlist[0] = 0; // (an album's own: nothing to add)
	}
	if (bp_get(&b, info, MR "IsAlwaysLive", &o)) {
		size_t at = bp_at(&b, o);
		if (at) np.live = b.d[at] == 0x09; // (bplist's true)
	}
	double v, rate = -1, el = -1, ts = 0;
	if (bp_get(&b, info, MR "Duration", &o) && bp_num(&b, o, &v) && v > 0 && v < 86400) np.duration = v;
	if (bp_get(&b, info, MR "PlaybackRate", &o) && bp_num(&b, o, &v)) rate = v;
	if (bp_get(&b, info, MR "ElapsedTime", &o) && bp_num(&b, o, &v) && v >= 0 && v < 86400) el = v;
	if (bp_get(&b, info, MR "Timestamp", &o)) bp_num(&b, o, &ts);
	if (rate >= 0) set_playing(rate > 0);
	if (el >= 0) {
		// as of the sender's timestamp (seconds since 2001), if both clocks agree to within a minute
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		double since = now.tv_sec + now.tv_nsec / 1e9 - 978307200.0 - ts;
		if (np_log) fprintf(stderr, "sprite: now playing: elapsed %.3f at %.3f s ago, rate %.3f\n", el, ts > 0 ? since : -1, rate);
		if (ts > 0 && since > 0 && since < 60 && np.playing) el += since;
		progress_at(el, 0);
	} else if (rate > 0 && ts > 0 && np.playing && lv_tick_elaps(np.at) < 3000) {
		// playing again, from where it stopped (an iPad says when, but not where): from when it says, rather than when the word came
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		double since = now.tv_sec + now.tv_nsec / 1e9 - 978307200.0 - ts;
		if (since > 0 && since < 5) {
			np.at = lv_tick_get() - (uint32_t)(since * 1000);
			if (np_log) fprintf(stderr, "sprite: now playing: started %.3f s ago\n", since);
		}
	}
	const uint8_t *art;
	size_t an;
	if (bp_get(&b, info, MR "ArtworkData", &o) && bp_data(&b, o, &art, &an)) set_art(art, an);
	return rate > 0;
}

/* ---- shairport-sync's items ---- */

// The sender's name comes once, as it connects, so it's kept where a restart of Sprite finds it (its runtime folder, gone at the next boot)
static void sender_path(char *p, size_t n) {
	const char *d = getenv("XDG_RUNTIME_DIR");
	snprintf(p, n, "%s/sprite-nowplaying-sender", d ? d : "/tmp");
}

static void sender_keep(void) {
	char p[256];
	sender_path(p, sizeof p);
	if (!np.sender[0]) { unlink(p); return; }
	FILE *f = fopen(p, "w");
	if (f) { fputs(np.sender, f); fclose(f); }
}

static void item(uint32_t type, uint32_t code, const uint8_t *d, size_t n) {
	bool starts = false; // it says something's playing: a session starts, if one hasn't
	if (np_log) fprintf(stderr, "sprite: now playing: %.4s %.4s, %zu bytes\n", (const char *)&(uint32_t){ __builtin_bswap32(type) }, (const char *)&(uint32_t){ __builtin_bswap32(code) }, n);
	if (type == code4("core")) {
		if (code == code4("minm")) set_str(np.title, sizeof np.title, d, n);
		else if (code == code4("asar")) set_str(np.artist, sizeof np.artist, d, n);
		else if (code == code4("asal")) set_str(np.album, sizeof np.album, d, n);
		else return;
	} else if (type == code4("ssnc")) {
		if (code == code4("abeg") || code == code4("pbeg")) {
			set_playing(true);
			starts = true;
		} else if (code == code4("aend")) {
			clear_all(); // (the session's over: nothing's shown till the next)
			np_refresh();
			shell_nowplaying_changed();
			return;
		} else if (code == code4("pend") || code == code4("paus") || code == code4("pfls")) {
			if (np.playing) progress_at(elapsed_now(), 0);
			set_playing(false);
		} else if (code == code4("pres") || code == code4("prsm")) {
			if (!np.playing) np.at = lv_tick_get();
			set_playing(true);
		} else if (code == code4("disc")) {
			np.sender[0] = 0; // (its name came as it connected, not with each session: kept till it goes)
			sender_keep();
			return;
		} else if (code == code4("snam")) {
			set_str(np.sender, sizeof np.sender, d, n);
			sender_keep();
		} else if (code == code4("PICT")) {
			set_art(d, n);
		} else if (code == code4("prgr") && n < 64) {
			// RTP frames, at 44.1 kHz: start/now/end (AirPlay 1's progress; AirPlay 2's comes in its messages)
			char s[64];
			memcpy(s, d, n);
			s[n] = 0;
			unsigned long a, c, e;
			if (sscanf(s, "%lu/%lu/%lu", &a, &c, &e) == 3 && e > a) progress_at((double)(uint32_t)(c - a) / 44100, (double)(uint32_t)(e - a) / 44100);
		} else if (code == code4("copl")) {
			if (np_log && getenv("XDG_RUNTIME_DIR")) { // (each kept, to read)
				static int k;
				char path[256];
				snprintf(path, sizeof path, "%s/np-copl-%03d.bin", getenv("XDG_RUNTIME_DIR"), k++ % 200);
				FILE *f = fopen(path, "wb");
				if (f) { fwrite(d, 1, n, f); fclose(f); }
			}
			starts = now_playing_plist(d, n);
		} else {
			return;
		}
	} else {
		return;
	}
	// a stream starting, or an iPhone saying it's playing (Sprite may have started part way through a session), starts one; anything else only changes the one that's going
	if (starts) np.active = true;
	if (!np.active) return;
	np_refresh();
	shell_nowplaying_changed();
}

static int np_readable(int fd, uint32_t mask, void *data) {
	static uint8_t pkt[65536];
	for (;;) {
		ssize_t r = recv(fd, pkt, sizeof pkt, MSG_DONTWAIT);
		if (r < 0) break;
		if (r < 8) continue;
		uint32_t type = be32(pkt), code = be32(pkt + 4);
		if (type == code4("ssnc") && code == code4("chnk")) {
			if (r < 24) continue;
			uint32_t ix = be32(pkt + 8), count = be32(pkt + 12), ct = be32(pkt + 16), cc = be32(pkt + 20);
			size_t len = r - 24;
			if (ix == 0) { chunk_len = 0; chunk_count = count; chunk_type = ct; chunk_code = cc; chunk_next = 0; }
			// a chunk out of turn (one lost): the item's dropped
			if (ix != chunk_next || count != chunk_count || ct != chunk_type || cc != chunk_code || chunk_len + len > NP_ITEM_MAX) {
				chunk_next = UINT32_MAX;
				continue;
			}
			if (!chunk_buf && !(chunk_buf = malloc(NP_ITEM_MAX))) continue;
			memcpy(chunk_buf + chunk_len, pkt + 24, len);
			chunk_len += len;
			if (++chunk_next == chunk_count) {
				chunk_next = UINT32_MAX;
				item(chunk_type, chunk_code, chunk_buf, chunk_len);
			}
			continue;
		}
		item(type, code, pkt + 8, r - 8);
	}
	return 0;
}

/* ---- the screen ---- */

static lv_obj_t *scr, *art_box, *art_img, *art_none, *src_row, *l_from, *l_title, *l_artist, *l_album, *bar, *l_el, *l_left, *l_paused;
static lv_timer_t *tick;

static void fmt_time(char *o, size_t n, double s) {
	int t = (int)s;
	if (t >= 3600) snprintf(o, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
	else snprintf(o, n, "%d:%02d", t / 60, t % 60);
}

static void set_label(lv_obj_t *l, const char *s) {
	if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
	if (s[0]) lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
}

static void np_progress(void) {
	bool known = np.duration > 0 && !np.live;
	double e = elapsed_now();
	char a[16] = "", b[20] = "";
	if (known) {
		fmt_time(a, sizeof a, e);
		b[0] = '-';
		fmt_time(b + 1, sizeof b - 1, np.duration - e);
		lv_bar_set_value(bar, (int)(e * 1000 / np.duration), LV_ANIM_OFF);
	}
	if (known) lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
	if (np.live) snprintf(a, sizeof a, "%s", STR_NOWPLAYING_LIVE);
	set_label(l_el, a);
	set_label(l_left, b);
	if (np.playing || lv_tick_elaps(np.stopped) < NP_PAUSED_MS) lv_obj_add_flag(l_paused, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag(l_paused, LV_OBJ_FLAG_HIDDEN);
}

// Each second as the time shown turns over (a timer of a fixed second, started whenever, showed
// it up to a second late), so the next one is set from where it's got to; stopped, as Paused is due
static void np_tick(lv_timer_t *t) {
	np_progress();
	double e = elapsed_now();
	uint32_t ms = np.playing ? (uint32_t)((1.0 - (e - floor(e))) * 1000) + 15 :
		lv_tick_elaps(np.stopped) < NP_PAUSED_MS ? NP_PAUSED_MS - lv_tick_elaps(np.stopped) + 15 : 1000; // (stopped: Paused, as soon as it's due)
	lv_timer_set_period(t, ms < 50 ? 50 : ms);
}

static void np_fill(void) {
	// where it's from, by its glyph: AirPlay's, with the sender's name (or AirPlay's, unknown), and the playlist, if there's room (else it's cut short)
	char from[300];
	const char *who = np.sender[0] ? np.sender : STR_NOWPLAYING_AIRPLAY;
	if (np.playlist[0]) snprintf(from, sizeof from, "%s  \xc2\xb7  %s", who, np.playlist);
	else snprintf(from, sizeof from, "%s", who);
	set_label(l_from, from);
	set_label(l_title, np.title[0] ? np.title : STR_NOWPLAYING_AIRPLAY);
	set_label(l_artist, np.artist);
	set_label(l_album, np.album);
	if (np.art) {
		if (lv_image_get_src(art_img) != np.art) lv_image_set_src(art_img, np.art);
		lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(art_none, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_image_set_src(art_img, NULL);
		lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(art_none, LV_OBJ_FLAG_HIDDEN);
	}
	uint32_t tint = np.art ? np.tint : C_NP_BG;
	lv_obj_set_style_bg_color(scr, lv_color_hex(tint), 0);
	np_progress();
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *f, uint32_t color, int lines) {
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_set_width(l, NP_TEXT_W);
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_height(l, lv_font_get_line_height(f) * lines + 2 * (lines - 1));
	lv_obj_set_style_text_line_space(l, 2, 0);
	lv_label_set_text(l, "");
	return l;
}

static void np_build(void) {
	scr = lv_obj_create(lv_layer_sys());
	lv_obj_remove_style_all(scr);
	lv_obj_set_size(scr, SPRITE_W, SPRITE_H);
	lv_obj_set_style_bg_color(scr, lv_color_hex(C_NP_BG), 0);
	lv_obj_set_style_bg_grad_color(scr, lv_color_hex(C_NP_BG), 0);
	lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

	// the artwork, its corners rounded, or a card with a note on it
	art_box = lv_obj_create(scr);
	lv_obj_remove_style_all(art_box);
	lv_obj_set_size(art_box, NP_ART, NP_ART);
	lv_obj_set_pos(art_box, NP_LEFT, NP_MARGIN);
	lv_obj_set_style_radius(art_box, 14, 0);
	lv_obj_set_style_clip_corner(art_box, true, 0);
	lv_obj_set_style_bg_color(art_box, lv_color_hex(C_NP_CARD), 0);
	lv_obj_set_style_bg_opa(art_box, LV_OPA_COVER, 0);
	lv_obj_remove_flag(art_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	art_img = lv_image_create(art_box);
	lv_obj_set_pos(art_img, 0, 0);
	art_none = lv_label_create(art_box);
	lv_label_set_text(art_none, LV_SYMBOL_AUDIO);
	lv_obj_set_style_text_font(art_none, &lv_font_montserrat_40, 0);
	lv_obj_set_style_text_color(art_none, lv_color_hex(C_NP_DIM), 0);
	lv_obj_center(art_none);

	// Beside it, a little in from its top and foot: the title, artist and album, from the top;
	// and from the foot up, the times (gone, and left), the bar, and where it's coming from
	int x = NP_LEFT + NP_ART + NP_GAP, foot = NP_MARGIN + NP_ART - NP_INSET;
	int times_y = foot - lv_font_get_line_height(&sprite_text_20), bar_y = times_y - 14, src_y = bar_y - 14 - 20;
	lv_obj_t *col = lv_obj_create(scr);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, NP_TEXT_W, src_y - 16 - (NP_MARGIN + NP_INSET));
	lv_obj_set_pos(col, x, NP_MARGIN + NP_INSET);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 10, 0);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	l_title = text(col, &sprite_text_32, C_NP_TEXT, 3);
	lv_obj_set_height(l_title, LV_SIZE_CONTENT); // up to three lines, and only as tall as it is
	lv_obj_set_style_max_height(l_title, lv_font_get_line_height(&sprite_text_32) * 3 + 4, 0);
	l_artist = text(col, &sprite_text_24, C_NP_TEXT, 2);
	lv_obj_set_height(l_artist, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(l_artist, lv_font_get_line_height(&sprite_text_24) * 2 + 2, 0);
	l_album = text(col, &sprite_text_20, C_NP_DIM, 2);
	lv_obj_set_height(l_album, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(l_album, lv_font_get_line_height(&sprite_text_20) * 2 + 2, 0);

	src_row = lv_obj_create(scr);
	lv_obj_remove_style_all(src_row);
	lv_obj_set_size(src_row, NP_TEXT_W, 20);
	lv_obj_set_pos(src_row, x, src_y);
	lv_obj_set_flex_flow(src_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(src_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(src_row, 7, 0);
	lv_obj_remove_flag(src_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *glyph = lv_label_create(src_row);
	lv_label_set_text(glyph, UI_ICON_AIRPLAY);
	lv_obj_set_style_text_font(glyph, &sprite_ui_16, 0);
	lv_obj_set_style_text_color(glyph, lv_color_hex(C_NP_DIM), 0);
	l_from = lv_label_create(src_row);
	lv_obj_set_style_text_font(l_from, &sprite_text_14, 0);
	lv_obj_set_style_text_color(l_from, lv_color_hex(C_NP_DIM), 0);
	lv_label_set_long_mode(l_from, LV_LABEL_LONG_DOT);
	lv_obj_set_width(l_from, NP_TEXT_W - 24);
	lv_label_set_text(l_from, "");

	bar = lv_bar_create(scr);
	lv_obj_set_size(bar, NP_TEXT_W, 6);
	lv_obj_set_pos(bar, x, bar_y);
	lv_bar_set_range(bar, 0, 1000);
	lv_obj_set_style_bg_color(bar, lv_color_hex(C_NP_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, lv_color_hex(C_NP_TEXT), LV_PART_INDICATOR);
	lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
	lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
	l_el = lv_label_create(scr);
	lv_obj_set_style_text_font(l_el, &sprite_text_20, 0);
	lv_obj_set_style_text_color(l_el, lv_color_hex(C_NP_DIM), 0);
	lv_obj_set_pos(l_el, x, times_y);
	l_left = lv_label_create(scr);
	lv_obj_set_style_text_font(l_left, &sprite_text_20, 0);
	lv_obj_set_style_text_color(l_left, lv_color_hex(C_NP_DIM), 0);
	lv_obj_set_width(l_left, NP_TEXT_W);
	lv_obj_set_style_text_align(l_left, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(l_left, x, times_y);
	// while it's paused, a pause sign between the times: two solid bars, their corners rounded (Lucide's is an outline)
	l_paused = lv_obj_create(scr);
	lv_obj_remove_style_all(l_paused);
	lv_obj_set_size(l_paused, 18, 20);
	lv_obj_set_pos(l_paused, x + (NP_TEXT_W - 18) / 2, times_y + (lv_font_get_line_height(&sprite_text_20) - 20) / 2);
	lv_obj_remove_flag(l_paused, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	for (int i = 0; i < 2; i++) {
		lv_obj_t *b = lv_obj_create(l_paused);
		lv_obj_remove_style_all(b);
		lv_obj_set_size(b, 6, 20);
		lv_obj_set_pos(b, i * 12, 0);
		lv_obj_set_style_radius(b, 2, 0);
		lv_obj_set_style_bg_color(b, lv_color_hex(C_NP_TEXT), 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	}
	lv_obj_add_flag(l_paused, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
}

// It fades in over what's under it (the screensaver's flip clock, the home screen, Settings,
// an app), and out again, rather than cutting; at once only with the screen off, where nobody
// would see it. Over an app, the shell keeps covering it till it's faded out.
#define NP_FADE_MS 300
static bool np_vis;        // showing, or fading in (what the rest goes by)
static bool cover_release; // the shell's cover to be let go once it's faded out

static bool shown(void) { return np_vis; }

static void fade_cb(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }

static void faded_out(void) {
	lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
	shell_nowplaying_shown(false);
	if (cover_release) {
		cover_release = false;
		if (!np_vis && !screensaver_active()) sprite_set_covered(false);
	}
}

static void fade_done(lv_anim_t *a) { if (!np_vis) faded_out(); }

static void fade(bool in) {
	lv_anim_delete(scr, fade_cb);
	lv_opa_t from = lv_obj_get_style_opa(scr, 0), to = in ? LV_OPA_COVER : LV_OPA_TRANSP;
	if (from == to) { if (!in) faded_out(); return; }
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, scr);
	lv_anim_set_exec_cb(&a, fade_cb);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, NP_FADE_MS * abs(to - from) / 255);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_set_completed_cb(&a, fade_done);
	lv_anim_start(&a);
}

// Shown while there's something to show and it's asked for (the screensaver, or the bar's
// note), and the screen's on; the screensaver's flip clock under it when that's what it is
static void np_refresh(void) {
	if (!scr) return;
	bool want = np.active && (np_saver || np_opened) && !np_asleep;
	if (!want) {
		if (np_vis) {
			np_vis = false;
			if (tick) lv_timer_delete(tick);
			tick = NULL;
			if (np_asleep) { // (the screen's off: at once)
				lv_anim_delete(scr, fade_cb);
				lv_obj_set_style_opa(scr, LV_OPA_TRANSP, 0);
				faded_out();
			} else {
				fade(false);
			}
		}
		// opened from the bar, and the session's over: back to what was showing (but still the
		// screensaver, if it's that too, for when something plays again), once it's faded
		if (np_opened && !np.active) {
			np_opened = false;
			if (!screensaver_active()) {
				if (lv_obj_has_flag(scr, LV_OBJ_FLAG_HIDDEN)) sprite_set_covered(false);
				else cover_release = true;
			}
		}
		return;
	}
	np_fill();
	if (!np_vis) {
		np_vis = true;
		cover_release = false;
		if (lv_obj_has_flag(scr, LV_OBJ_FLAG_HIDDEN)) lv_obj_set_style_opa(scr, LV_OPA_TRANSP, 0);
		lv_obj_move_foreground(scr);
		lv_obj_remove_flag(scr, LV_OBJ_FLAG_HIDDEN);
		tick = lv_timer_create(np_tick, 1000, NULL);
		shell_nowplaying_shown(true);
		fade(true);
	}
	// (the time's turning over from where it is now)
	np_tick(tick);
	lv_timer_reset(tick);
	sprite_request_frame();
}

/* ---- for the shell ---- */

void nowplaying_init(void) {
	np_log = getenv("SPRITE_NP_LOG") && !strcmp(getenv("SPRITE_NP_LOG"), "1");
	char p[256];
	sender_path(p, sizeof p);
	FILE *f = fopen(p, "r");
	if (f) {
		uint8_t b[sizeof np.sender];
		size_t n = fread(b, 1, sizeof b - 1, f);
		fclose(f);
		set_str(np.sender, sizeof np.sender, b, n);
	}
	np_build();
	np_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (np_fd < 0) return;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(NP_PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int big = 1 << 20;
	setsockopt(np_fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big); // artwork comes as a burst of chunks
	if (bind(np_fd, (struct sockaddr *)&a, sizeof a) < 0) {
		fprintf(stderr, "sprite: now playing: can't listen on 127.0.0.1:%d\n", NP_PORT);
		close(np_fd);
		np_fd = -1;
		return;
	}
	wl_event_loop_add_fd(sprite_event_loop(), np_fd, WL_EVENT_READABLE, np_readable, NULL);
}

bool nowplaying_active(void) { return np.active; }
bool nowplaying_shown(void) { return shown(); }

void nowplaying_saver(bool on) {
	np_saver = on;
	np_refresh();
}

void nowplaying_asleep(bool asleep) {
	np_asleep = asleep;
	np_refresh();
}

// Opened from the bar's note: over everything, as the screensaver is, till a touch
void nowplaying_open(void) {
	if (!np.active) return;
	np_opened = true;
	sprite_set_covered(true);
	np_refresh();
}

void nowplaying_close(void) {
	bool was = np_opened, vis = np_vis;
	np_opened = np_saver = false;
	np_refresh();
	// fading out: the shell covers what's under it (an app) till it's gone, though the screensaver's just let go
	if (vis && !np_asleep) {
		sprite_set_covered(true);
		cover_release = true;
	} else if (was && !screensaver_active()) {
		sprite_set_covered(false);
	}
}
