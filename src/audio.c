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
#define TS_SECS 3.2
static atomic_int ts_state;      // 0 idle, 1 playing, -1 couldn't (since the last ask)
static double ts_started;

struct ts_note { double at, freq, decay; int side; }; // side: -1 left, 1 right, 0 both
static const struct ts_note ts_notes[] = {
	{ 0.00, 523.25, 0.35, -1 }, { 0.13, 659.25, 0.35, -1 }, { 0.26, 783.99, 0.45, -1 },   // C5 E5 G5
	{ 0.95, 783.99, 0.35, 1 },  { 1.08, 1046.50, 0.35, 1 }, { 1.21, 1318.51, 0.45, 1 },  // G5 C6 E6
	{ 1.95, 523.25, 0.70, 0 },  { 1.95, 659.25, 0.70, 0 },  { 1.95, 783.99, 0.70, 0 },   // C major,
	{ 1.98, 1046.50, 0.60, 0 },                                                          // both
};

// A bell-ish tone: the note and two softer overtones, a 4 ms rise (no click) and a decay.
static double ts_tone(const struct ts_note *n, double t) {
	if (t < 0) return 0;
	double env = (t < 0.004 ? t / 0.004 : 1) * exp(-t / n->decay);
	double w = 2 * M_PI * n->freq * t;
	return env * (sin(w) + 0.25 * sin(2 * w) + 0.08 * sin(3 * w));
}

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *ts_play(void *arg) {
	snd_pcm_t *pcm;
	if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) { ts_state = -1; return NULL; }
	if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, TS_RATE, 1, 150000) < 0) {
		snd_pcm_close(pcm);
		ts_state = -1;
		return NULL;
	}
	ts_started = now_s();
	enum { CHUNK = 1024 };
	int16_t buf[CHUNK * 2];
	long total = (long)(TS_SECS * TS_RATE);
	for (long f0 = 0; f0 < total; f0 += CHUNK) {
		int n = total - f0 < CHUNK ? (int)(total - f0) : CHUNK;
		for (int i = 0; i < n; i++) {
			double t = (double)(f0 + i) / TS_RATE, l = 0, r = 0;
			for (size_t k = 0; k < sizeof ts_notes / sizeof ts_notes[0]; k++) {
				double v = ts_tone(&ts_notes[k], t - ts_notes[k].at);
				if (ts_notes[k].side <= 0) l += v;
				if (ts_notes[k].side >= 0) r += v;
			}
			// about 6 dB below full scale at the loudest (the chord), whatever the volume
			buf[2 * i] = (int16_t)lrint(l * 0.14 * 32767);
			buf[2 * i + 1] = (int16_t)lrint(r * 0.14 * 32767);
		}
		for (int done = 0; done < n;) {
			snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf + 2 * done, n - done);
			if (w < 0) w = snd_pcm_recover(pcm, (int)w, 1);
			if (w < 0) { snd_pcm_close(pcm); ts_state = -1; return NULL; }
			done += (int)w;
		}
	}
	snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
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
	return t < 0.9 ? 1 : t < 1.9 ? 2 : 3;
}
void audio_test_seen(void) { int f = -1; atomic_compare_exchange_strong(&ts_state, &f, 0); }
