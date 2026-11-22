// SPDX-License-Identifier: GPL-3.0-or-later
// Wi-Fi, through sprite-helper (which runs as root, and owns settings/wifi.txt): looking
// for networks, the connection's state, joining and forgetting. Each request is a
// connection to the helper's socket, sent without waiting and answered through the event
// loop, so a scan (which takes a few seconds) never holds up a frame.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "sprite.h"

#define REPLY_MAX 16384

struct request {
	int fd;
	struct wl_event_source *src;
	size_t len;
	void (*done)(char *reply, void *ctx); // reply is NULL if the helper couldn't be reached
	void *ctx;
	char buf[REPLY_MAX];
};

static void request_end(struct request *r, bool ok) {
	if (r->src) wl_event_source_remove(r->src);
	if (r->fd >= 0) close(r->fd);
	r->buf[r->len] = 0;
	r->done(ok ? r->buf : NULL, r->ctx);
	explicit_bzero(r, sizeof *r);
	free(r);
}

static int request_readable(int fd, uint32_t mask, void *data) {
	struct request *r = data;
	for (;;) {
		ssize_t n = read(fd, r->buf + r->len, REPLY_MAX - 1 - r->len);
		if (n > 0) {
			r->len += n;
			if (r->len < REPLY_MAX - 1) continue;
		} else if (n < 0 && errno == EAGAIN) {
			return 0;
		} else if (n < 0 && errno == EINTR) {
			continue;
		}
		break; // the end of the reply, an error, or full
	}
	request_end(r, true);
	return 0;
}

// Send text to the helper; done() gets its whole reply when the helper hangs up.
void helper_request(const char *text, void (*done)(char *reply, void *ctx), void *ctx) {
	struct request *r = calloc(1, sizeof *r);
	if (!r) { done(NULL, ctx); return; }
	r->done = done;
	r->ctx = ctx;
	r->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", HELPER_SOCKET);
	size_t len = strlen(text);
	if (r->fd < 0 || connect(r->fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    write(r->fd, text, len) != (ssize_t)len) { // a few bytes into a fresh socket: all go at once
		fprintf(stderr, "sprite: helper not reachable at %s\n", HELPER_SOCKET);
		request_end(r, false);
		return;
	}
	shutdown(r->fd, SHUT_WR);
	r->src = wl_event_loop_add_fd(sprite_event_loop(), r->fd, WL_EVENT_READABLE, request_readable, r);
	if (!r->src) request_end(r, false);
}

// The reply's last line: "ok", or "error: <reason>". The lines before it are the answer.
const char *helper_reply_status(char *reply) {
	if (!reply) return "error: Sprite's helper isn't running";
	size_t n = strlen(reply);
	while (n && reply[n - 1] == '\n') reply[--n] = 0;
	char *last = strrchr(reply, '\n');
	if (last) { *last = 0; return last + 1; }
	static char only[256];
	snprintf(only, sizeof only, "%s", reply);
	reply[0] = 0;
	return only;
}

// wpa_supplicant writes SSIDs with \\, \", \e, \n, \r, \t and \xNN escapes: undo them.
// False if what's left is empty, too long, or has control characters, or isn't UTF-8
// (sprite can't show those, and netplan wants UTF-8).
static bool ssid_decode(const char *in, char out[WIFI_SSID_MAX + 1]) {
	size_t n = 0;
	for (const char *p = in; *p; p++) {
		unsigned char c = *p;
		if (c == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'e': c = 0x1b; break;
			case 'x': {
				char hex[3] = { p[1], p[1] ? p[2] : 0, 0 };
				char *end;
				c = (unsigned char)strtoul(hex, &end, 16);
				if (end != hex + 2) return false;
				p += 2;
				break;
			}
			default: c = *p; break; // \\ and \"
			}
		}
		if (c < 0x20 || c == 0x7f || n == WIFI_SSID_MAX) return false;
		out[n++] = c;
	}
	out[n] = 0;
	if (!n) return false;
	// UTF-8: lead bytes followed by the right number of continuation bytes
	for (size_t i = 0; i < n;) {
		unsigned char c = out[i];
		int more = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
		if (more < 0 || i + more >= n + (more ? 0 : 1)) return false;
		for (int k = 1; k <= more; k++)
			if ((out[i + k] & 0xc0) != 0x80) return false;
		i += more + 1;
	}
	return true;
}

/* ---- scan ---- */

struct scan_ctx { wifi_scan_cb cb; void *data; };

static int by_signal(const void *a, const void *b) {
	return ((const struct wifi_net *)b)->dbm - ((const struct wifi_net *)a)->dbm;
}

static void scan_done(char *reply, void *p) {
	struct scan_ctx c = *(struct scan_ctx *)p;
	free(p);
	const char *st = helper_reply_status(reply);
	static struct wifi_net nets[WIFI_NETS_MAX];
	int n = 0;
	if (!strcmp(st, "ok")) {
		char *save = NULL;
		for (char *line = strtok_r(reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			// bssid, frequency, signal, flags, ssid
			char *f[5] = { 0 };
			char *q = line;
			for (int i = 0; i < 5 && q; i++) {
				f[i] = q;
				q = i < 4 ? strchr(q, '\t') : NULL;
				if (q) *q++ = 0;
			}
			if (!f[4]) continue;
			struct wifi_net w = { .dbm = atoi(f[2]) };
			if (!ssid_decode(f[4], w.ssid)) continue; // hidden (no name), or one we can't show
			const char *fl = f[3];
			w.secured = strstr(fl, "WPA") || strstr(fl, "RSN") || strstr(fl, "WEP") || strstr(fl, "SAE");
			// netplan's settings from wifi.txt are a password or none: no WEP, no enterprise
			// (EAP) networks, unless they also take a password (PSK, SAE)
			w.supported = !strstr(fl, "WEP") && (!strstr(fl, "EAP") || strstr(fl, "PSK") || strstr(fl, "SAE"));
			int i;
			for (i = 0; i < n; i++)
				if (!strcmp(nets[i].ssid, w.ssid)) break;
			if (i < n) { // the same network from another access point: keep the strongest
				if (w.dbm > nets[i].dbm) nets[i].dbm = w.dbm;
				continue;
			}
			if (n < WIFI_NETS_MAX) nets[n++] = w;
		}
		qsort(nets, n, sizeof nets[0], by_signal);
	}
	c.cb(nets, n, strcmp(st, "ok") ? st : NULL, c.data);
}

void wifi_scan(wifi_scan_cb cb, void *data) {
	struct scan_ctx *c = malloc(sizeof *c);
	if (!c) return;
	*c = (struct scan_ctx){ cb, data };
	helper_request("wifi-scan\n", scan_done, c);
}

// Bars from the signal, for the list and the status bar alike: the usual cut-offs, -67 dBm
// being the common figure for good enough to stream reliably, below -75 unreliable.
int wifi_level(int dbm) {
	return dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -75 ? 2 : 1;
}

/* ---- status ---- */

struct status_ctx { wifi_status_cb cb; void *data; };

static void status_done(char *reply, void *p) {
	struct status_ctx c = *(struct status_ctx *)p;
	free(p);
	const char *st = helper_reply_status(reply);
	struct wifi_status s = { 0 };
	if (!strcmp(st, "ok")) {
		s.present = true;
		char *save = NULL;
		for (char *line = strtok_r(reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *v = strchr(line, '=');
			if (!v) continue;
			*v++ = 0;
			if (!strcmp(line, "saved")) snprintf(s.saved, sizeof s.saved, "%s", v);
			else if (!strcmp(line, "ssid")) { if (!ssid_decode(v, s.ssid)) s.ssid[0] = 0; }
			else if (!strcmp(line, "wpa_state")) snprintf(s.state, sizeof s.state, "%s", v);
			else if (!strcmp(line, "address")) snprintf(s.address, sizeof s.address, "%s", v);
			else if (!strcmp(line, "trouble")) s.trouble = true;
		}
		s.connected = !strcmp(s.state, "COMPLETED") && s.address[0];
	}
	c.cb(&s, strcmp(st, "ok") ? st : NULL, c.data);
}

void wifi_status(wifi_status_cb cb, void *data) {
	struct status_ctx *c = malloc(sizeof *c);
	if (!c) return;
	*c = (struct status_ctx){ cb, data };
	helper_request("wifi-status\n", status_done, c);
}

/* ---- join, forget ---- */

struct done_ctx { wifi_done_cb cb; void *data; };

static void change_done(char *reply, void *p) {
	struct done_ctx c = *(struct done_ctx *)p;
	free(p);
	const char *st = helper_reply_status(reply);
	c.cb(strcmp(st, "ok") ? st : NULL, c.data);
}

void wifi_join(const char *ssid, const char *password, wifi_done_cb cb, void *data) {
	struct done_ctx *c = malloc(sizeof *c);
	if (!c) return;
	*c = (struct done_ctx){ cb, data };
	char req[160];
	snprintf(req, sizeof req, "wifi-join\n%s\n%s\n", ssid, password);
	helper_request(req, change_done, c);
	explicit_bzero(req, sizeof req); // the password
}

void wifi_forget(wifi_done_cb cb, void *data) {
	struct done_ctx *c = malloc(sizeof *c);
	if (!c) return;
	*c = (struct done_ctx){ cb, data };
	helper_request("wifi-forget\n", change_done, c);
}
