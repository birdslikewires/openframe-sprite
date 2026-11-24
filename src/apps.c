// SPDX-License-Identifier: GPL-3.0-or-later
// Apps: manifests on disk, launching them, and tracking their processes.
//
// Apps run in sandboxes: sprite-helper starts each as a systemd unit, sprite-app-<id>, under
// a user of its own (see the helper for what each may do). sprite knows an app's processes
// by that unit, and its exit by a pidfd on its main process. With SPRITE_SANDBOX=0 (for
// trying things out) sprite starts apps itself instead, as its own user, as it used to.
//
// A manifest is a small key=value file, one per app, in the apps directory:
//   name=Clock
//   exec=xclock -digital -update 1
//   icon=clock           (keyword or up to two letters)
//   color=2a6f97         (tile colour)
//   builtin=settings     (handled by sprite itself, no exec)
//   display=x11          (an X11 or SDL app, run through Xwayland; default wayland)
//   handle=faint         (home handle: normal, faint or hidden while the app shows)
//   bar=yes              (the status bar stays above it, its window below; default full screen)
//   allow=network sound  (what a sandboxed app may have beyond the display: network, sound
//                         to play, microphone to record too; read by the helper)
// Comments go on lines of their own.
#define _GNU_SOURCE
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <xcb/xcb.h>

#include "sprite.h"

#define MAX_APPS 64

static struct sprite_app apps[MAX_APPS];
static int n_apps;
static char env_wayland[64], env_x[32];

// The X keeper. wlroots runs Xwayland as root and starts it only when a client connects;
// Xwayland admits only its own user, and quits when its last client leaves, forgetting any
// access granted. So a root helper connects (which starts Xwayland), lets the apps' user in,
// reports back through a pipe, and stays connected so the grant lasts. X11 apps launch
// once it has reported.
// The app opened at startup: "startup=" in settings/sprite.txt, by the app's name or id.
static char autostart_id[64];

static pid_t keeper_pid;
static bool x_open;
static struct wl_event_source *keeper_src;

void apps_set_env(const char *wayland_display, const char *x_display) {
	snprintf(env_wayland, sizeof env_wayland, "%s", wayland_display ? wayland_display : "");
	snprintf(env_x, sizeof env_x, "%s", x_display ? x_display : "");
}

bool apps_sandboxed(void) {
	const char *v = getenv("SPRITE_SANDBOX");
	return !(v && !strcmp(v, "0"));
}

int apps_count(void) { return n_apps; }
struct sprite_app *apps_get(int i) { return i >= 0 && i < n_apps ? &apps[i] : NULL; }

static void trim(char *s) {
	char *e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
}

static int cmp_apps(const void *a, const void *b) {
	const struct sprite_app *x = a, *y = b;
	if (x->builtin != y->builtin) return x->builtin ? 1 : -1; // settings last
	return strcasecmp(x->name, y->name);
}

static void load_dir(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) && n_apps < MAX_APPS) {
		size_t len = strlen(de->d_name);
		if (len < 5 || strcmp(de->d_name + len - 4, ".app")) continue;
		char path[512];
		snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		struct sprite_app *a = &apps[n_apps];
		memset(a, 0, sizeof *a);
		snprintf(a->id, sizeof a->id, "%.*s", (int)(len - 4), de->d_name);
		a->color = 0x3a4a5a;
		a->handle = SPRITE_HANDLE_NORMAL;
		a->pidfd = -1;
		char line[768];
		while (fgets(line, sizeof line, f)) {
			trim(line);
			char *eq = strchr(line, '=');
			if (line[0] == '#' || !eq) continue;
			*eq = 0;
			const char *k = line, *v = eq + 1;
			if (!strcmp(k, "name")) snprintf(a->name, sizeof a->name, "%s", v);
			else if (!strcmp(k, "exec")) snprintf(a->exec, sizeof a->exec, "%s", v);
			else if (!strcmp(k, "icon")) snprintf(a->icon, sizeof a->icon, "%s", v);
			else if (!strcmp(k, "color")) a->color = strtoul(v, NULL, 16);
			else if (!strcmp(k, "builtin")) { a->builtin = true; snprintf(a->exec, sizeof a->exec, "%s", v); }
			else if (!strcmp(k, "display")) a->x11 = !strcmp(v, "x11");
			else if (!strcmp(k, "bar")) a->bar = !strcmp(v, "yes");
			else if (!strcmp(k, "handle"))
				a->handle = !strcmp(v, "hidden") ? 0.0f : !strcmp(v, "faint") ? SPRITE_HANDLE_FAINT : SPRITE_HANDLE_NORMAL;
		}
		fclose(f);
		if (!a->name[0]) memcpy(a->name, a->id, sizeof a->name < sizeof a->id ? sizeof a->name : sizeof a->id);
		if (!a->exec[0]) { wlr_log(WLR_ERROR, "%s: no exec", path); continue; }
		bool dup = false; // the first directory to have an app id wins
		for (int i = 0; i < n_apps; i++) dup |= !strcmp(apps[i].id, a->id);
		if (dup) { wlr_log(WLR_INFO, "%s: %s is already loaded", path, a->id); continue; }
		n_apps++;
	}
	closedir(d);
}

// dirs: colon-separated, e.g. sprite's own apps then the ones packages install.
void apps_load(const char *dirs) {
	char list[512];
	snprintf(list, sizeof list, "%s", dirs);
	for (char *save = NULL, *dir = strtok_r(list, ":", &save); dir; dir = strtok_r(NULL, ":", &save))
		load_dir(dir);
	qsort(apps, n_apps, sizeof apps[0], cmp_apps);
	const char *v = settings_get("startup");
	if (v) {
		for (int i = 0; i < n_apps; i++) // by id or by name, ignoring case
			if (!apps[i].builtin && (!strcasecmp(apps[i].id, v) || !strcasecmp(apps[i].name, v)))
				snprintf(autostart_id, sizeof autostart_id, "%.63s", apps[i].id);
		if (!autostart_id[0]) wlr_log(WLR_ERROR, "startup=%s: no such app", v);
	}
	wlr_log(WLR_INFO, "%d apps from %s", n_apps, dirs);
}

static pid_t parent_of(pid_t pid) {
	char path[64], buf[512];
	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	char *p = strrchr(buf, ')'); // the command name can contain spaces
	int ppid = 0;
	if (p) sscanf(p + 2, "%*c %d", &ppid);
	return ppid;
}

// A sandboxed app's processes are all in its unit's cgroup: "0::/system.slice/sprite-app-<id>.service".
static struct sprite_app *app_for_unit(pid_t pid) {
	char path[64], line[512];
	snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
	FILE *f = fopen(path, "r");
	if (!f) return NULL;
	struct sprite_app *found = NULL;
	if (fgets(line, sizeof line, f)) {
		char *u = strstr(line, "/sprite-app-"), *e;
		if (u && (e = strstr(u += 12, ".service\n"))) {
			*e = 0;
			for (int i = 0; i < n_apps && !found; i++)
				if (!apps[i].hidden && !strcmp(apps[i].id, u)) found = &apps[i];
		}
	}
	fclose(f);
	return found;
}

struct sprite_app *apps_for_pid(pid_t pid) {
	struct sprite_app *in_unit = pid > 1 ? app_for_unit(pid) : NULL;
	if (in_unit) return in_unit;
	for (pid_t p = pid; p > 1; p = parent_of(p))
		for (int i = 0; i < n_apps; i++)
			if (apps[i].pid == p) return &apps[i];
	// A window from a process we didn't launch (started by hand over SSH, say): give it a
	// hidden entry of its own so it can still be shown and closed like any other app.
	for (int i = 0; i < n_apps; i++)
		if (apps[i].hidden && apps[i].pid == pid) return &apps[i];
	if (n_apps >= MAX_APPS) return &apps[n_apps - 1];
	struct sprite_app *a = &apps[n_apps++];
	memset(a, 0, sizeof *a);
	snprintf(a->id, sizeof a->id, "pid-%d", pid);
	snprintf(a->name, sizeof a->name, TX_APP_WINDOW, pid);
	a->hidden = true;
	a->pid = pid;
	a->pidfd = -1;
	a->handle = SPRITE_HANDLE_NORMAL;
	return a;
}

static int keeper_readable(int fd, uint32_t mask, void *data) {
	char c = 0;
	ssize_t n = read(fd, &c, 1);
	wl_event_source_remove(keeper_src);
	keeper_src = NULL;
	close(fd);
	apps_x11_ready(n == 1 && c == 'y');
	shell_apps_changed();
	return 0;
}

// kind is "localuser" or "localgroup": sandboxed apps are let in by their group, sprite-apps.
static void start_keeper(const char *kind, const char *name) {
	int p[2];
	if (keeper_pid || !env_x[0] || pipe(p) != 0) return;
	pid_t pid = fork();
	if (pid == 0) {
		close(p[0]);
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		xcb_connection_t *c = xcb_connect(env_x, NULL);
		char ok = 'n';
		if (!xcb_connection_has_error(c)) {
			char entry[80]; // server-interpreted address: "localuser" or "localgroup", NUL, name
			int len = snprintf(entry, sizeof entry, "%s%c%s", kind, 0, name);
			xcb_void_cookie_t ck = xcb_change_hosts_checked(c, XCB_HOST_MODE_INSERT,
				XCB_FAMILY_SERVER_INTERPRETED, len, (const uint8_t *)entry);
			xcb_generic_error_t *err = xcb_request_check(c, ck);
			ok = err ? 'n' : 'y';
			free(err);
		}
		if (write(p[1], &ok, 1) != 1) _exit(1);
		close(p[1]);
		if (ok != 'y') _exit(1);
		// stay connected; leave when Xwayland goes away
		xcb_generic_event_t *ev;
		while ((ev = xcb_wait_for_event(c))) free(ev);
		_exit(0);
	}
	close(p[1]);
	if (pid < 0) { close(p[0]); return; }
	keeper_pid = pid;
	keeper_src = wl_event_loop_add_fd(sprite_event_loop(), p[0], WL_EVENT_READABLE, keeper_readable, NULL);
	wlr_log(WLR_INFO, "X keeper started (pid %d)", pid);
}

void apps_x11_ready(bool ready) {
	x_open = ready;
	wlr_log(WLR_INFO, "X %s to apps", ready ? "open" : "closed");
	for (int i = 0; i < n_apps; i++) {
		if (!apps[i].pending) continue;
		apps[i].pending = false;
		if (ready) apps_launch(&apps[i]);
		else wlr_log(WLR_ERROR, "%s not started: X is not available", apps[i].name);
	}
}

static void exited(struct sprite_app *a);

static int app_gone(int fd, uint32_t mask, void *data) {
	struct sprite_app *a = data;
	wl_event_source_remove(a->exit_src);
	a->exit_src = NULL;
	close(a->pidfd);
	a->pidfd = -1;
	wlr_log(WLR_INFO, "%s exited", a->name);
	a->pid = 0;
	exited(a);
	shell_apps_changed();
	return 0;
}

static void launched(char *reply, void *data) {
	struct sprite_app *a = data;
	a->pending = false;
	const char *st = helper_reply_status(reply);
	int pid = 0;
	if (strcmp(st, "ok") || sscanf(reply, "pid %d", &pid) != 1 || pid <= 1) {
		wlr_log(WLR_ERROR, "%s didn't start: %s", a->name, st);
		char s[160];
		snprintf(s, sizeof s, TX_APP_COULDNT_OPEN, a->name);
		shell_notice(s);
		exited(a);
		shell_apps_changed();
		return;
	}
	a->pid = pid;
	a->pidfd = pidfd_open(pid, 0);
	if (a->pidfd >= 0)
		a->exit_src = wl_event_loop_add_fd(sprite_event_loop(), a->pidfd, WL_EVENT_READABLE, app_gone, a);
	wlr_log(WLR_INFO, "launched %s in its sandbox (pid %d)", a->name, pid);
	if (!a->exit_src) app_gone(-1, 0, a); // gone already
	else shell_apps_changed();
}

void apps_launch(struct sprite_app *app) {
	if (app->builtin || app->pid || app->pending) return;
	// Xwayland admits only its own user until it's opened to the apps' (or their group)
	if (app->x11 && !x_open && (apps_sandboxed() || getuid() == 0)) {
		app->pending = true;
		if (apps_sandboxed()) start_keeper("localgroup", "sprite-apps");
		else start_keeper("localuser", getenv("SPRITE_USER") ? getenv("SPRITE_USER") : "sprite");
		return;
	}
	if (apps_sandboxed()) {
		char req[128];
		snprintf(req, sizeof req, "app-start %s %s\n", app->id, app->x11 ? env_x : "");
		app->pending = true;
		wlr_log(WLR_INFO, "starting %s: %s", app->name, app->exec);
		helper_request(req, launched, app);
		return;
	}
	const char *user = getenv("SPRITE_USER") ? getenv("SPRITE_USER") : "sprite";
	struct passwd *pw = getpwnam(user);
	pid_t pid = fork();
	if (pid < 0) { wlr_log(WLR_ERROR, "fork: %s", strerror(errno)); return; }
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL); // the compositor blocks SIGCHLD for its loop
		if (getuid() == 0 && pw) {
			if (initgroups(pw->pw_name, pw->pw_gid) || setgid(pw->pw_gid) || setuid(pw->pw_uid)) _exit(126);
			setenv("HOME", pw->pw_dir, 1);
			setenv("USER", pw->pw_name, 1);
			setenv("LOGNAME", pw->pw_name, 1);
			if (chdir(pw->pw_dir) != 0) { /* stay where we are */ }
		}
		setenv("WAYLAND_DISPLAY", env_wayland, 1);
		if (env_x[0]) setenv("DISPLAY", env_x, 1);
		setenv("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", "0", 1); // sprite keeps it, in the background
		setenv("SDL_VIDEODRIVER", "x11", 1); // SDL's Wayland backend needs EGL, which we don't have
		// sdl12-compat loads our Mesa-free SDL2 at run time
		setenv("LD_LIBRARY_PATH", "/opt/sprite/lib:/opt/sprite/lib/i386-linux-gnu", 1);
		unsetenv("LIBSEAT_BACKEND");
		unsetenv("WLR_RENDERER");
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) { dup2(fd, 0); close(fd); }
		execl("/bin/sh", "/bin/sh", "-c", app->exec, (char *)NULL);
		_exit(127);
	}
	app->pid = pid;
	wlr_log(WLR_INFO, "launched %s (pid %d): %s", app->name, pid, app->exec);
}

struct sprite_app *apps_autostart(void) {
	if (!autostart_id[0]) return NULL;
	for (int i = 0; i < n_apps; i++)
		if (!apps[i].hidden && !apps[i].builtin && !strcmp(apps[i].id, autostart_id)) return &apps[i];
	return NULL;
}

void apps_set_autostart(struct sprite_app *app, bool on) {
	if (on) snprintf(autostart_id, sizeof autostart_id, "%s", app->id);
	else if (!strcmp(autostart_id, app->id)) autostart_id[0] = 0;
	else return;
	settings_set("startup", on ? app->name : "");
	wlr_log(WLR_INFO, "open at startup: %s", on ? app->name : "(none)");
}

static uint32_t mono_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int restart_cb(void *data) {
	struct sprite_app *a = data;
	wlr_log(WLR_INFO, "reopening %s", a->name);
	apps_launch(a);
	shell_apps_changed();
	return 0;
}

static int close_cb(void *data) {
	struct sprite_app *a = data;
	if (a->pid <= 0) return 0;
	if (a->exit_src) { // sandboxed: its unit stops it, SIGKILL 2 s after SIGTERM
		char req[96];
		snprintf(req, sizeof req, "app-stop %.63s", a->id);
		wlr_log(WLR_INFO, "%s still running: stopping its unit", a->name);
		sysinfo_helper(req);
		return 0;
	}
	pid_t target = a->hidden ? a->pid : -a->pid; // launched apps lead their own process group
	if (++a->close_stage == 1) {
		wlr_log(WLR_INFO, "%s still running: SIGTERM", a->name);
		kill(target, SIGTERM);
		wl_event_source_timer_update(a->close_timer, 2000);
	} else {
		wlr_log(WLR_INFO, "%s still running: SIGKILL", a->name);
		kill(target, SIGKILL);
	}
	return 0;
}

void apps_close(struct sprite_app *a) {
	if (a->builtin || a->pid <= 0) return;
	a->closing = true;
	a->close_stage = 0;
	if (a->restart_timer) wl_event_source_timer_update(a->restart_timer, 0);
	if (!a->close_timer) a->close_timer = wl_event_loop_add_timer(sprite_event_loop(), close_cb, a);
	// a window has been asked to close; give it 2 s to do so itself
	wl_event_source_timer_update(a->close_timer, a->view ? 2000 : 1);
}

// The startup app is reopened when it exits, unless it was closed from the shell or keeps
// failing (three exits within a minute).
static void exited(struct sprite_app *a) {
	if (a->close_timer) wl_event_source_timer_update(a->close_timer, 0);
	if (a->reopening) { // apps_restart: opened again straight away
		a->reopening = a->closing = false;
		if (!a->restart_timer) a->restart_timer = wl_event_loop_add_timer(sprite_event_loop(), restart_cb, a);
		wl_event_source_timer_update(a->restart_timer, 300);
		return;
	}
	if (a->closing) { a->closing = false; return; }
	if (a != apps_autostart()) return;
	uint32_t now = mono_ms();
	if (!a->quick_exits || now - a->first_exit_ms > 60000) { a->quick_exits = 0; a->first_exit_ms = now; }
	if (++a->quick_exits >= 3) {
		char s[128];
		snprintf(s, sizeof s, TX_APP_KEEPS_CLOSING, a->name);
		sprite_hold_for(NULL);
		shell_notice(s);
		wlr_log(WLR_ERROR, "%s exited 3 times in a minute; not reopening", a->name);
		a->quick_exits = 0;
		return;
	}
	sprite_hold_for(a);
	if (!a->restart_timer) a->restart_timer = wl_event_loop_add_timer(sprite_event_loop(), restart_cb, a);
	wl_event_source_timer_update(a->restart_timer, 2000);
}

// Closes app a and opens it again as soon as it's gone: for one playing when the sound output
// changes, which goes on the old way until it opens its sound again. Behind: its window comes
// back behind what's showing (Settings, where the output was changed), not in front.
void apps_restart(struct sprite_app *a, bool behind) {
	if (a->builtin || a->pid <= 0 || a->reopening) return;
	wlr_log(WLR_INFO, "restarting %s (the sound output changed)", a->name);
	a->reopening = true;
	a->reopen_behind = behind;
	apps_close(a);
	wl_event_source_timer_update(a->close_timer, 1); // stopped now: nothing to wait for
}

// Apps lead their own process groups and, under a PAM session, live outside sprite's
// service, so nothing else ends them when sprite stops. Left running they hold on to
// things (HamClock refuses to start while an old copy runs). SIGTERM all, wait up to 2 s,
// then SIGKILL what's left.
void apps_end_all(void) {
	int left = 0;
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].pid <= 0 || apps[i].hidden) continue;
		if (apps[i].exit_src) { // bound to sprite.service, but sprite may be running by hand
			char req[96];
			snprintf(req, sizeof req, "app-stop %.63s", apps[i].id);
			sysinfo_helper(req);
			continue;
		}
		kill(-apps[i].pid, SIGTERM);
		left++;
	}
	if (keeper_pid) kill(keeper_pid, SIGTERM);
	for (int t = 0; t < 20 && left; t++) {
		usleep(100000);
		left = 0;
		for (int i = 0; i < n_apps; i++) {
			if (apps[i].pid <= 0 || apps[i].hidden || apps[i].exit_src) continue;
			if (waitpid(apps[i].pid, NULL, WNOHANG) == apps[i].pid) apps[i].pid = 0;
			else left++;
		}
	}
	for (int i = 0; i < n_apps; i++)
		if (apps[i].pid > 0 && !apps[i].hidden && !apps[i].exit_src) {
			wlr_log(WLR_INFO, "%s still running at exit: SIGKILL", apps[i].name);
			kill(-apps[i].pid, SIGKILL);
		}
}

bool apps_reap(void) {
	bool changed = false;
	int st;
	if (keeper_pid && waitpid(keeper_pid, &st, WNOHANG) == keeper_pid) {
		wlr_log(WLR_INFO, "X keeper exited");
		keeper_pid = 0;
		if (keeper_src) { // died before reporting
			wl_event_source_remove(keeper_src);
			keeper_src = NULL;
		}
		apps_x11_ready(false);
		changed = true;
	}
	for (int i = 0; i < n_apps; i++) {
		struct sprite_app *a = &apps[i];
		if (a->pid <= 0 || a->hidden || a->exit_src) continue; // a sandboxed app isn't our child
		int status;
		if (waitpid(a->pid, &status, WNOHANG) == a->pid) {
			wlr_log(WLR_INFO, "%s exited (%s %d)", a->name, WIFEXITED(status) ? "status" : "signal",
				WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
			a->pid = 0;
			exited(a);
			changed = true;
		}
	}
	// hidden entries: drop the pid once the process has gone
	for (int i = 0; i < n_apps; i++)
		if (apps[i].hidden && apps[i].pid > 0 && kill(apps[i].pid, 0) != 0 && errno == ESRCH && !apps[i].view)
			apps[i].pid = 0;
	return changed;
}
