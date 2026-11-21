// sprite's settings: settings/sprite.txt on the OF-BOOT volume, key=value lines like the
// other OpenFrame settings files, so they can be edited from a PC. sprite reads them at
// start. Changes from the touchscreen go through the root helper (/boot is read-only).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sprite.h"

#define SETTINGS_FILE "/boot/settings/sprite.txt"
#define MAX_SETTINGS 16

static struct { char key[32], value[160]; } settings[MAX_SETTINGS];
static int n_settings;

void settings_load(void) {
	const char *path = getenv("SPRITE_SETTINGS") ? getenv("SPRITE_SETTINGS") : SETTINGS_FILE;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof line, f) && n_settings < MAX_SETTINGS) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		snprintf(settings[n_settings].key, sizeof settings[0].key, "%.31s", line);
		snprintf(settings[n_settings].value, sizeof settings[0].value, "%.159s", eq + 1);
		n_settings++;
	}
	fclose(f);
}

const char *settings_get(const char *key) {
	for (int i = 0; i < n_settings; i++)
		if (!strcmp(settings[i].key, key)) return settings[i].value[0] ? settings[i].value : NULL;
	return NULL;
}

void settings_set(const char *key, const char *value) {
	int i;
	for (i = 0; i < n_settings; i++)
		if (!strcmp(settings[i].key, key)) break;
	if (i == n_settings) {
		if (n_settings == MAX_SETTINGS) return;
		snprintf(settings[n_settings++].key, sizeof settings[0].key, "%s", key);
	}
	snprintf(settings[i].value, sizeof settings[0].value, "%s", value ? value : "");
	char line[224];
	snprintf(line, sizeof line, "set %s %s", key, value ? value : "");
	sysinfo_helper(line);
}
