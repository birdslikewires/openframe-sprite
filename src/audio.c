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
#include <stdio.h>
#include <string.h>

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
