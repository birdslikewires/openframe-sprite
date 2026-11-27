// SPDX-License-Identifier: GPL-3.0-or-later
// The Shop's facts: what apps.openbeak.net offers, from apt's lists of it (fetched by the helper's shop-refresh, and overnight), what's installed, from dpkg's status, and how the helper's install or removal is going (/run/sprite-shop/status). Each package says what the Shop shows in its control fields (X-Sprite-*, written by openframe-apps' ci/build-app.sh), so there's no other catalogue. The Shop's screens are in shell.c.
#define _GNU_SOURCE
#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wlr/util/log.h>

#include "sprite.h"

#define LISTS "/var/lib/apt/lists/apps.openbeak.net_dists_*_main_binary-i386_Packages*"
#define APT_HELPER "/usr/lib/apt/apt-helper"
#define DPKG_STATUS "/var/lib/dpkg/status"
#define JOB_STATUS "/run/sprite-shop/status"
#define JOB_STALE_S 600 // a job that's said nothing for this long has died (apt waits up to 5 minutes for dpkg's lock)

static struct shop_app shop[SHOP_MAX];
static int n_shop;

int shop_count(void) { return n_shop; }
struct shop_app *shop_get(int i) { return i >= 0 && i < n_shop ? &shop[i] : NULL; }

struct shop_app *shop_find(const char *id) {
	for (int i = 0; i < n_shop; i++)
		if (!strcmp(shop[i].id, id)) return &shop[i];
	return NULL;
}

/* ---- Debian's version order (dpkg's verrevcmp), to take the newest of a package the lists offer and to tell an update ---- */

static int order(int c) {
	if (isdigit(c)) return 0;
	if (isalpha(c)) return c;
	if (c == '~') return -1;
	if (c) return c + 256;
	return 0;
}

static int verrevcmp(const char *a, const char *b) {
	while (*a || *b) {
		int first_diff = 0;
		while ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b))) {
			int ac = order((unsigned char)*a), bc = order((unsigned char)*b);
			if (ac != bc) return ac - bc;
			a++;
			b++;
		}
		while (*a == '0') a++;
		while (*b == '0') b++;
		while (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			if (!first_diff) first_diff = *a - *b;
			a++;
			b++;
		}
		if (isdigit((unsigned char)*a)) return 1;
		if (isdigit((unsigned char)*b)) return -1;
		if (first_diff) return first_diff;
	}
	return 0;
}

// [epoch:]upstream[-revision], as dpkg compares them: <0, 0 or >0
int shop_vercmp(const char *a, const char *b) {
	long ea = 0, eb = 0;
	const char *c;
	if ((c = strchr(a, ':'))) { ea = strtol(a, NULL, 10); a = c + 1; }
	if ((c = strchr(b, ':'))) { eb = strtol(b, NULL, 10); b = c + 1; }
	if (ea != eb) return ea < eb ? -1 : 1;
	char ua[96], ub[96];
	snprintf(ua, sizeof ua, "%s", a);
	snprintf(ub, sizeof ub, "%s", b);
	char *ra = strrchr(ua, '-'), *rb = strrchr(ub, '-');
	if (ra) *ra++ = 0;
	if (rb) *rb++ = 0;
	int r = verrevcmp(ua, ub);
	return r ? r : verrevcmp(ra ? ra : "", rb ? rb : "");
}

/* ---- control stanzas: apt's lists and dpkg's status are both made of them ---- */

// One stanza's fields, as the Shop wants them
struct stanza {
	char package[64], version[64], status[64];
	struct shop_app app;
};

static void field(struct stanza *s, const char *k, const char *v) {
	struct shop_app *a = &s->app;
	if (!strcmp(k, "Package")) snprintf(s->package, sizeof s->package, "%s", v);
	else if (!strcmp(k, "Version")) snprintf(s->version, sizeof s->version, "%s", v);
	else if (!strcmp(k, "Status")) snprintf(s->status, sizeof s->status, "%s", v);
	else if (!strcmp(k, "Description")) snprintf(a->summary, sizeof a->summary, "%s", v);
	else if (!strcmp(k, "Installed-Size")) a->size_kb = strtol(v, NULL, 10);
	else if (!strcmp(k, "Homepage")) snprintf(a->source, sizeof a->source, "%s", v);
	else if (!strcmp(k, "X-Sprite-Name")) snprintf(a->name, sizeof a->name, "%s", v);
	else if (!strcmp(k, "X-Sprite-Kind")) { a->saver = !strcmp(v, "screensaver"); a->service = !strcmp(v, "service"); }
	else if (!strcmp(k, "X-Sprite-Icon")) snprintf(a->icon, sizeof a->icon, "%s", v);
	else if (!strcmp(k, "X-Sprite-Color")) a->color = strtoul(v, NULL, 16);
	else if (!strcmp(k, "X-Sprite-Allow")) snprintf(a->allow, sizeof a->allow, "%s", v);
	else if (!strcmp(k, "X-Sprite-Author")) snprintf(a->author, sizeof a->author, "%s", v);
	else if (!strcmp(k, "X-Sprite-Licence")) snprintf(a->licence, sizeof a->licence, "%s", v);
	else if (!strcmp(k, "X-Sprite-Community")) snprintf(a->community, sizeof a->community, "%s", v);
}

// A line of the description after its first: " text", or " ." between paragraphs, which go into description a line each.
static void description_line(struct shop_app *a, const char *l) {
	size_t n = strlen(a->description);
	if (!strcmp(l, ".")) {
		if (n && n + 1 < sizeof a->description) strcat(a->description, "\n");
		return;
	}
	snprintf(a->description + n, sizeof a->description - n, "%s%s", n && a->description[n - 1] != '\n' ? " " : "", l);
}

// Reads stanzas from f, calling done() for each with an ofapp-* package.
static void read_stanzas(FILE *f, void (*done)(struct stanza *s)) {
	static struct stanza s;
	static char line[4096];
	char key[64] = "";
	memset(&s, 0, sizeof s);
	for (;;) {
		bool eof = !fgets(line, sizeof line, f);
		if (!eof) line[strcspn(line, "\n")] = 0;
		if (eof || !line[0]) { // a stanza ends
			if (!strncmp(s.package, "ofapp-", 6) && s.version[0]) done(&s);
			memset(&s, 0, sizeof s);
			key[0] = 0;
			if (eof) return;
			continue;
		}
		if (line[0] == ' ' || line[0] == '\t') { // a continuation: only the description's matter
			if (!strcmp(key, "Description")) description_line(&s.app, line + 1);
			continue;
		}
		char *colon = strchr(line, ':');
		if (!colon) continue;
		*colon = 0;
		snprintf(key, sizeof key, "%.63s", line);
		const char *v = colon + 1;
		while (*v == ' ') v++;
		field(&s, key, v);
	}
}

// The id an app's package is for: ofapp-<id>, as its manifest is <id>.app
static void set_ids(struct shop_app *a, const char *package) {
	snprintf(a->package, sizeof a->package, "%s", package);
	snprintf(a->id, sizeof a->id, "%s", package + 6);
	if (!a->name[0]) snprintf(a->name, sizeof a->name, "%s", a->id);
}

// A package the lists offer: kept if it's the newest of it so far (a test unit has the testing suite's list as well as the released one).
static void offered(struct stanza *s) {
	struct shop_app *a = shop_find(s->package + 6);
	if (a && shop_vercmp(s->version, a->version) <= 0) return;
	if (!a) {
		if (n_shop == SHOP_MAX) return;
		a = &shop[n_shop++];
	}
	*a = s->app;
	set_ids(a, s->package);
	snprintf(a->version, sizeof a->version, "%s", s->version);
	a->listed = true;
}

// An installed package: its version, and if the lists don't offer it (installed by hand, or no longer in the Shop), what its own control fields say.
static void installed(struct stanza *s) {
	const char *st = s->status;
	size_t n = strlen(st);
	if (n < 10 || strcmp(st + n - 10, " installed")) return; // ("install ok installed"; half-installed or removed isn't)
	struct shop_app *a = shop_find(s->package + 6);
	if (!a) {
		if (n_shop == SHOP_MAX) return;
		a = &shop[n_shop++];
		*a = s->app;
		set_ids(a, s->package);
		snprintf(a->version, sizeof a->version, "%s", s->version);
		a->listed = false;
	}
	snprintf(a->installed, sizeof a->installed, "%s", s->version);
}

static int by_name(const void *x, const void *y) {
	return strcasecmp(((const struct shop_app *)x)->name, ((const struct shop_app *)y)->name);
}

void shop_load(void) {
	n_shop = 0;
	glob_t g;
	if (glob(LISTS, 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			const char *p = g.gl_pathv[i];
			if (strchr(p, '\'')) continue;
			// apt-helper reads whatever apt compressed it with (lz4, on an OpenFrame)
			char cmd[512];
			snprintf(cmd, sizeof cmd, APT_HELPER " cat-file '%s' 2>/dev/null", p);
			FILE *f = popen(cmd, "r");
			if (!f) continue;
			read_stanzas(f, offered);
			pclose(f);
		}
		globfree(&g);
	}
	FILE *f = fopen(DPKG_STATUS, "r");
	if (f) {
		read_stanzas(f, installed);
		fclose(f);
	}
	qsort(shop, n_shop, sizeof shop[0], by_name);
	wlr_log(WLR_INFO, "the Shop has %d apps", n_shop);
}

// How long ago the newest of the lists was fetched, in seconds, or -1 if there's none yet
long shop_lists_age(void) {
	glob_t g;
	time_t newest = 0;
	if (glob(LISTS, 0, NULL, &g) == 0) {
		struct stat st;
		for (size_t i = 0; i < g.gl_pathc; i++)
			if (stat(g.gl_pathv[i], &st) == 0 && st.st_mtime > newest) newest = st.st_mtime;
		globfree(&g);
	}
	return newest ? (long)(time(NULL) - newest) : -1;
}

/* ---- the helper's job ---- */

void shop_job_read(struct shop_job *j) {
	memset(j, 0, sizeof *j);
	FILE *f = fopen(JOB_STATUS, "r");
	if (!f) return;
	char line[256], state[16] = "";
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		const char *k = line, *v = eq + 1;
		if (!strcmp(k, "action")) j->remove = !strcmp(v, "remove");
		else if (!strcmp(k, "id")) snprintf(j->id, sizeof j->id, "%s", v);
		else if (!strcmp(k, "state")) snprintf(state, sizeof state, "%s", v);
		else if (!strcmp(k, "percent")) j->percent = atoi(v);
		else if (!strcmp(k, "reason")) snprintf(j->reason, sizeof j->reason, "%s", v);
	}
	struct stat st;
	bool stale = fstat(fileno(f), &st) == 0 && time(NULL) - st.st_mtime > JOB_STALE_S;
	fclose(f);
	j->state = !strcmp(state, "running") ? (stale ? SHOP_JOB_NONE : SHOP_JOB_RUNNING) :
		!strcmp(state, "done") ? SHOP_JOB_DONE : !strcmp(state, "failed") ? SHOP_JOB_FAILED : SHOP_JOB_NONE;
	if (j->percent < 0) j->percent = 0;
	if (j->percent > 100) j->percent = 100;
}
