// SPDX-License-Identifier: GPL-3.0-or-later
// audio.c: the volume of the sound card in use, read and set here through ALSA's mixer API
// (openframe-scripts' of-audio says which card and control): at every step of a drag, so in
// the process rather than a command each time. Sprite's user can, as the seat's (logind gives
// it the sound devices).
//
// A slider's 0-100 follows alsamixer's "mapped" volume (alsa-utils' volume_mapping.c): along
// how loud it sounds for a control with a wide dB range, rather than its raw steps, so the
// middle of the slider is half as loud rather than nearly silent.
#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sprite.h"

#define MAX_LINEAR_DB_SCALE 24 // dB: up to this range, plain steps are near enough

static snd_mixer_t *mx;
static snd_mixer_elem_t *el;
static char mx_card[32], mx_ctl[64];

static void mixer_close(void) {
	if (mx) snd_mixer_close(mx);
	mx = NULL;
	el = NULL;
	mx_card[0] = mx_ctl[0] = 0;
}

// The control, opened once and kept while it's the one; false if it isn't there (yet: a
// software one appears once something has played).
static bool mixer_open(const char *card, const char *ctl) {
	if (!card || !card[0] || !ctl || !ctl[0]) { mixer_close(); return false; }
	if (mx && !strcmp(card, mx_card) && !strcmp(ctl, mx_ctl)) {
		snd_mixer_handle_events(mx); // (changed elsewhere since: alsactl, amixer)
		return true;
	}
	mixer_close();
	char dev[48];
	snprintf(dev, sizeof dev, "hw:CARD=%s", card);
	if (snd_mixer_open(&mx, 0) < 0) { mx = NULL; return false; }
	if (snd_mixer_attach(mx, dev) < 0 || snd_mixer_selem_register(mx, NULL, NULL) < 0 || snd_mixer_load(mx) < 0) {
		mixer_close();
		return false;
	}
	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_index(id, 0);
	snd_mixer_selem_id_set_name(id, ctl);
	el = snd_mixer_find_selem(mx, id);
	if (!el || !snd_mixer_selem_has_playback_volume(el)) { mixer_close(); return false; }
	snprintf(mx_card, sizeof mx_card, "%s", card);
	snprintf(mx_ctl, sizeof mx_ctl, "%s", ctl);
	return true;
}

int audio_volume(const char *card, const char *ctl) {
	if (!mixer_open(card, ctl)) return -1;
	long min, max, v;
	double norm;
	if (snd_mixer_selem_get_playback_dB_range(el, &min, &max) < 0 || min >= max) {
		snd_mixer_selem_get_playback_volume_range(el, &min, &max);
		if (min >= max || snd_mixer_selem_get_playback_volume(el, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0) return -1;
		norm = (double)(v - min) / (max - min);
	} else {
		if (snd_mixer_selem_get_playback_dB(el, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0) return -1;
		if (max - min <= MAX_LINEAR_DB_SCALE * 100) norm = (double)(v - min) / (max - min);
		else {
			norm = pow(10, (v - max) / 6000.0);
			if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
				double min_norm = pow(10, (min - max) / 6000.0);
				norm = (norm - min_norm) / (1 - min_norm);
			}
		}
	}
	int sw = 1;
	if (snd_mixer_selem_has_playback_switch(el)) snd_mixer_selem_get_playback_switch(el, SND_MIXER_SCHN_FRONT_LEFT, &sw);
	if (!sw) return 0; // muted: as good as nothing
	int p = (int)lrint(norm * 100);
	return p < 0 ? 0 : p > 100 ? 100 : p;
}

bool audio_set_volume(const char *card, const char *ctl, int pct) {
	if (!mixer_open(card, ctl)) return false;
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	int was = audio_volume(card, ctl);
	int dir = pct >= was ? 1 : -1; // (rounding towards where it's going, so every step moves)
	double vol = pct / 100.0;
	long min, max;
	int err;
	if (snd_mixer_selem_get_playback_dB_range(el, &min, &max) < 0 || min >= max) {
		snd_mixer_selem_get_playback_volume_range(el, &min, &max);
		err = snd_mixer_selem_set_playback_volume_all(el, lrint(vol * (max - min)) + min);
	} else if (max - min <= MAX_LINEAR_DB_SCALE * 100) {
		err = snd_mixer_selem_set_playback_dB_all(el, lrint(vol * (max - min)) + min, dir);
	} else {
		if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
			double min_norm = pow(10, (min - max) / 6000.0);
			vol = vol * (1 - min_norm) + min_norm;
		}
		long db = vol > 0 ? lrint(6000.0 * log10(vol)) + max : min;
		err = snd_mixer_selem_set_playback_dB_all(el, db, dir);
	}
	// 0 is off, anything else on (a level on its own leaves a mute as it was)
	if (snd_mixer_selem_has_playback_switch(el)) snd_mixer_selem_set_playback_switch_all(el, pct > 0);
	return err >= 0;
}

/* ---- the test sound: left, then right, then both, synthesized as it plays ---- */
//
// A rising three-note chime on the left, the same a few notes higher on the right, then the
// whole chord in both: which side is which, and that they're the right way round. Made here,
// as it goes, into ALSA's default (wherever of-audio sends the sound, at its volume, or
// straight to a DAC), on a thread of its own so the shell carries on drawing.
#define TS_RATE 44100
#define TS_SECS 4.1  // the chord dies away by itself in about two seconds
#define TS_FADE 0.05 // the last 50 ms (by then 43 dB down) to exactly nothing: no click at the end
static atomic_int ts_state;      // 0 idle, 1 playing, -1 couldn't (since the last ask)
static double ts_started;

struct ts_note { double at, freq, decay; int side; }; // side: -1 left, 1 right, 0 both
static const struct ts_note ts_notes[] = {
	{ 0.00, 523.25, 0.35, -1 }, { 0.13, 659.25, 0.35, -1 }, { 0.26, 783.99, 0.45, -1 },   // C5 E5 G5
	{ 0.95, 783.99, 0.35, 1 },  { 1.08, 1046.50, 0.35, 1 }, { 1.21, 1318.51, 0.45, 1 },  // G5 C6 E6
	{ 2.03, 523.25, 0.42, 0 },  { 2.03, 659.25, 0.42, 0 },  { 2.03, 783.99, 0.42, 0 },   // C major,
	{ 2.06, 1046.50, 0.37, 0 },                                                          // both
};

// A bell-ish tone: the note and two softer overtones (from it: sin 2w = 2 sin w cos w,
// sin 3w = 3 sin w - 4 sin^3 w), a 4 ms rise (no click) and a decay. Each note's oscillator
// turns by a fixed step and its level falls by a fixed factor per sample, so making the
// whole sound takes a few tens of milliseconds on the Atom rather than most of a second.
struct ts_osc { double s, c, ds, dc, env, fall; long start, rise; };
static void ts_osc_init(struct ts_osc *o, const struct ts_note *n) {
	double w = 2 * M_PI * n->freq / TS_RATE;
	o->s = 0; o->c = 1; o->ds = sin(w); o->dc = cos(w);
	o->env = 1; o->fall = exp(-1.0 / (n->decay * TS_RATE));
	o->start = lrint(n->at * TS_RATE); o->rise = lrint(0.004 * TS_RATE);
}
static double ts_osc_next(struct ts_osc *o, long f) {
	if (f < o->start) return 0;
	double s = o->s, c = o->c, v = (s + 0.25 * 2 * s * c + 0.08 * (3 * s - 4 * s * s * s)) * o->env;
	long a = f - o->start;
	if (a < o->rise) v *= (double)a / o->rise;
	o->s = s * o->dc + c * o->ds; // turn on by one step
	o->c = c * o->dc - s * o->ds;
	o->env *= o->fall;
	return v;
}

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// All of it made first, then played: made as it played, the thread could fall behind for a
// moment just as it started (the shell busy drawing the tap), and the gap clicked.
static void *ts_play(void *arg) {
	long total = (long)(TS_SECS * TS_RATE), fade = (long)(TS_FADE * TS_RATE);
	int16_t *buf = malloc(total * 2 * sizeof *buf);
	if (!buf) { ts_state = -1; return NULL; }
	enum { N = sizeof ts_notes / sizeof ts_notes[0] };
	struct ts_osc osc[N];
	for (int k = 0; k < N; k++) ts_osc_init(&osc[k], &ts_notes[k]);
	for (long f = 0; f < total; f++) {
		double l = 0, r = 0;
		for (int k = 0; k < N; k++) {
			double v = ts_osc_next(&osc[k], f);
			if (ts_notes[k].side <= 0) l += v;
			if (ts_notes[k].side >= 0) r += v;
		}
		// about 6 dB below full scale at the loudest (the chord), whatever the volume
		double g = 0.14 * 32767;
		if (f > total - fade) g *= 0.5 + 0.5 * cos(M_PI * (f - (total - fade)) / fade);
		buf[2 * f] = (int16_t)lrint(l * g);
		buf[2 * f + 1] = (int16_t)lrint(r * g);
	}
	snd_pcm_t *pcm;
	if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) { free(buf); ts_state = -1; return NULL; }
	if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, TS_RATE, 1, 400000) < 0) {
		snd_pcm_close(pcm);
		free(buf);
		ts_state = -1;
		return NULL;
	}
	ts_started = now_s();
	for (long done = 0; done < total;) {
		snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf + 2 * done, total - done);
		if (w < 0) w = snd_pcm_recover(pcm, (int)w, 1);
		if (w < 0) { snd_pcm_close(pcm); free(buf); ts_state = -1; return NULL; }
		done += w;
	}
	snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
	free(buf);
	ts_state = 0;
	return NULL;
}

bool audio_test_sound(void) {
	int idle = 0;
	if (!atomic_compare_exchange_strong(&ts_state, &idle, 1)) {
		if (idle != -1) return false;  // still playing
		ts_state = 1;                   // (after one that couldn't)
	}
	ts_started = now_s();
	// ALSA reads its settings once in a process and keeps them (it looks again only if its
	// own alsa.conf changes, not /etc/asound.conf, which of-audio writes): dropped here, on
	// the shell's thread, so the sound goes where the output is now, not where it was when
	// sprite first made a sound.
	snd_config_update_free_global();
	pthread_t th;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	bool ok = pthread_create(&th, &a, ts_play, NULL) == 0;
	pthread_attr_destroy(&a);
	if (!ok) ts_state = 0;
	return ok;
}

// What it's doing: 0 nothing, 1 the left, 2 the right, 3 both, -1 it couldn't play.
int audio_test_state(void) {
	int s = ts_state;
	if (s <= 0) return s;
	double t = now_s() - ts_started;
	return t < 0.9 ? 1 : t < 2.0 ? 2 : 3; // (the label for "both" stays on to the end)
}
void audio_test_seen(void) { int f = -1; atomic_compare_exchange_strong(&ts_state, &f, 0); }

/* ---- a USB DAC's connection, from what the kernel says of it in /proc ---- */
//
// Its name and how it's connected (/proc/asound/cards: "Audient iD4 at usb-..., high speed"),
// what it takes (its stream0's playback formats: bits, rates, and whether it's asynchronous,
// keeping the time itself), and, as it plays, at what (stream0 again: the format in use, by
// its altset, and the momentary rate). Readable by anyone, so no helper's needed.
bool audio_dac_info(const char *card, struct audio_dac *d) {
	memset(d, 0, sizeof *d);
	char line[256], want[40], path[96];
	snprintf(want, sizeof want, "[%s", card);
	FILE *f = fopen("/proc/asound/cards", "r");
	if (!f) return false;
	int n = -1;
	while (fgets(line, sizeof line, f)) {
		char *b = strchr(line, '[');
		if (!b || strncmp(b, want, strlen(want)) || (b[strlen(want)] != ' ' && b[strlen(want)] != ']')) continue;
		n = atoi(line);
		if (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\n")] = 0;
			char *s = line + strspn(line, " ");
			char *at = strstr(s, " at usb-"), *sp = strrchr(s, ',');
			if (sp) snprintf(d->speed, sizeof d->speed, "%s", sp + 2);
			if (at) *at = 0;
			snprintf(d->name, sizeof d->name, "%s", s);
		}
		break;
	}
	fclose(f);
	if (n < 0) return false;
	snprintf(path, sizeof path, "/proc/asound/card%d/stream0", n);
	if (!(f = fopen(path, "r"))) return false;
	// the playback altsets (their format's bits) and the running one
	int altset = -1, alt_bits[16] = { 0 }, run_alt = -1;
	bool playback = false;
	d->rate_min = 1 << 30;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "Playback:", 9)) { playback = true; continue; }
		if (!strncmp(line, "Capture:", 8)) playback = false;
		if (!playback) continue;
		char *v;
		if (strstr(line, "Status: Running")) d->running = true;
		else if ((v = strstr(line, "Altset = "))) run_alt = atoi(v + 9);
		else if ((v = strstr(line, "Momentary freq = "))) d->run_rate = atoi(v + 17);
		else if ((v = strstr(line, "Altset "))) altset = atoi(v + 7);
		else if ((v = strstr(line, "Channels: ")) && atoi(v + 10) > d->channels) d->channels = atoi(v + 10);
		else if (strstr(line, "Endpoint:") && strstr(line, "(ASYNC)")) d->async = true;
		else if ((v = strstr(line, "Bits: "))) {
			int b = atoi(v + 6);
			if (altset >= 0 && altset < 16) alt_bits[altset] = b;
			if (b > d->bits_max) d->bits_max = b;
			if (!d->bits_min || b < d->bits_min) d->bits_min = b;
		} else if ((v = strstr(line, "Rates: "))) {
			for (char *p = v + 7; *p;) {
				if (*p >= '0' && *p <= '9') {
					int r = (int)strtol(p, &p, 10);
					if (r >= 8000) {
						if (r < d->rate_min) d->rate_min = r;
						if (r > d->rate_max) d->rate_max = r;
					}
				} else p++;
			}
		}
	}
	fclose(f);
	if (d->rate_min == 1 << 30) d->rate_min = 0;
	if (d->running && run_alt >= 0 && run_alt < 16) d->run_bits = alt_bits[run_alt];
	return true;
}
