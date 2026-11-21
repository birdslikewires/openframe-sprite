// System state for the status bar and settings: network, backlight, a summary.
// Reads /proc and /sys only; changes go through the existing openframe tools (of-backlight).
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sprite.h"

static int read_int(const char *path, int fallback) {
	FILE *f = fopen(path, "r");
	if (!f) return fallback;
	int v = fallback;
	if (fscanf(f, "%d", &v) != 1) v = fallback;
	fclose(f);
	return v;
}

static bool if_up(const char *ifname) {
	char path[128], state[32] = "";
	snprintf(path, sizeof path, "/sys/class/net/%s/operstate", ifname);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	if (!fgets(state, sizeof state, f)) state[0] = 0;
	fclose(f);
	return !strncmp(state, "up", 2);
}

void sysinfo_net(struct sprite_net *n) {
	memset(n, 0, sizeof *n);
	n->wifi_quality = -1;
	n->eth_up = if_up("eth0");
	n->wifi_up = if_up("wlan0");
	struct ifaddrs *ifa, *i;
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
			char *dst = !strcmp(i->ifa_name, "eth0") ? n->eth_ip : !strcmp(i->ifa_name, "wlan0") ? n->wifi_ip : NULL;
			if (dst) inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, dst, sizeof n->eth_ip);
		}
		freeifaddrs(ifa);
	}
	// /proc/net/wireless: "wlan0: 0000   54.  -56.  -256 ..." link quality out of 70
	FILE *f = fopen("/proc/net/wireless", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof line, f)) {
			char *p = strstr(line, "wlan0:");
			if (!p) continue;
			unsigned status;
			float q;
			if (sscanf(p + 6, "%x %f", &status, &q) == 2) {
				int pct = (int)(q * 100 / 70);
				n->wifi_quality = pct < 0 ? 0 : pct > 100 ? 100 : pct;
			}
		}
		fclose(f);
	}
}

static int backlight_dir(char *out, size_t len) {
	FILE *p = popen("ls -1d /sys/class/backlight/* 2>/dev/null | head -1", "r");
	if (!p) return -1;
	int ok = fgets(out, len, p) != NULL;
	pclose(p);
	if (!ok) return -1;
	out[strcspn(out, "\n")] = 0;
	return 0;
}

int sysinfo_backlight(void) {
	static char dir[128];
	if (!dir[0] && backlight_dir(dir, sizeof dir) != 0) return -1;
	char path[192];
	snprintf(path, sizeof path, "%s/brightness", dir);
	int cur = read_int(path, -1);
	snprintf(path, sizeof path, "%s/max_brightness", dir);
	int max = read_int(path, -1);
	if (cur < 0 || max <= 0) return -1;
	return (cur * 100 + max / 2) / max;
}

bool sysinfo_backlight_auto(void) {
	char mode[16] = "";
	FILE *f = fopen("/run/of-backlight/status", "r");
	if (!f) return false;
	if (!fgets(mode, sizeof mode, f)) mode[0] = 0;
	fclose(f);
	return !strncmp(mode, "auto", 4);
}

void sysinfo_run(const char *cmd) {
	// Double fork, so the command is never our child and leaves no zombie.
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		if (fork() == 0) {
			setsid();
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0) waitpid(pid, NULL, 0);
}

// sprite's root helper listens here (systemd socket activation, one connection per request).
#define HELPER_SOCKET "/run/sprite-helper.sock"

void sysinfo_helper(const char *line) {
	pid_t pid = fork();
	if (pid == 0) {
		if (fork() != 0) _exit(0); // double fork: never our child, no zombie
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un sa = { .sun_family = AF_UNIX };
		snprintf(sa.sun_path, sizeof sa.sun_path, "%s", HELPER_SOCKET);
		if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
			fprintf(stderr, "sprite: helper not reachable at %s for '%s'\n", HELPER_SOCKET, line);
			_exit(1);
		}
		dprintf(fd, "%s\n", line);
		shutdown(fd, SHUT_WR);
		char reply[256];
		ssize_t n = read(fd, reply, sizeof reply - 1);
		if (n > 0) {
			reply[n] = 0;
			if (strncmp(reply, "ok", 2)) fprintf(stderr, "sprite: helper: %s", reply);
		}
		_exit(0);
	}
	if (pid > 0) waitpid(pid, NULL, 0);
}

void sysinfo_set_backlight(const char *arg) {
	char cmd[64];
	snprintf(cmd, sizeof cmd, "of-backlight %s >/dev/null 2>&1", arg);
	sysinfo_run(cmd);
}

void sysinfo_summary(char *buf, size_t len) {
	char host[64] = "?";
	gethostname(host, sizeof host);
	struct utsname u;
	uname(&u);
	int up = 0;
	FILE *f = fopen("/proc/uptime", "r");
	if (f) { double d; if (fscanf(f, "%lf", &d) == 1) up = (int)d; fclose(f); }
	long mem_total = 0, mem_avail = 0;
	f = fopen("/proc/meminfo", "r");
	if (f) {
		char line[128];
		while (fgets(line, sizeof line, f)) {
			sscanf(line, "MemTotal: %ld kB", &mem_total);
			sscanf(line, "MemAvailable: %ld kB", &mem_avail);
		}
		fclose(f);
	}
	struct statvfs sv;
	long root_free = statvfs("/", &sv) == 0 ? (long)(sv.f_bavail * (sv.f_frsize / 1024)) / 1024 : -1;
	struct sprite_net n;
	sysinfo_net(&n);
	char wifi[96];
	const char *wstate = n.wifi_ip[0] ? n.wifi_ip : n.wifi_up ? "connected, no address" : "not connected";
	if (n.wifi_quality >= 0) snprintf(wifi, sizeof wifi, "%s, signal %d%%", wstate, n.wifi_quality);
	else snprintf(wifi, sizeof wifi, "%s", wstate);
	snprintf(buf, len,
		 "Name: %s\n"
		 "Ethernet: %s\n"
		 "Wi-Fi: %s\n"
		 "Memory: %ld MB free of %ld MB\n"
		 "Storage: %ld MB free\n"
		 "Up: %dh %02dm\n"
		 "Kernel: %s\n"
		 "sprite: %s",
		 host,
		 n.eth_ip[0] ? n.eth_ip : n.eth_up ? "connected, no address" : "not connected",
		 wifi, mem_avail / 1024, mem_total / 1024, root_free, up / 3600, (up / 60) % 60, u.release,
		 SPRITE_VERSION);
}
