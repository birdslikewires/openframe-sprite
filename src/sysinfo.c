// SPDX-License-Identifier: GPL-3.0-or-later
// System state for the status bar and settings: network, backlight, a summary.
// Reads /proc, /sys and nl80211 only; changes go through the existing openframe tools
// (of-backlight).
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/genetlink.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
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

/* ---- the Wi-Fi signal, from nl80211 ---- */

// Asked over generic netlink, as `iw dev wlan0 link` does: no root needed, and no child
// process. /proc/net/wireless can't be used, as it lists cfg80211 drivers (every OpenFrame
// Wi-Fi card's) only with CFG80211_WEXT, which the kernel doesn't have.

struct nl_req {
	struct nlmsghdr n;
	struct genlmsghdr g;
	char attrs[32];
};

static void nl_req_init(struct nl_req *r, int type, int flags, int cmd) {
	memset(r, 0, sizeof *r);
	r->n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	r->n.nlmsg_type = type;
	r->n.nlmsg_flags = NLM_F_REQUEST | flags;
	r->g.cmd = cmd;
	r->g.version = 1;
}

static void nl_put(struct nl_req *r, int type, const void *data, int len) {
	struct nlattr *a = (struct nlattr *)((char *)r + NLMSG_ALIGN(r->n.nlmsg_len));
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	memcpy((char *)a + NLA_HDRLEN, data, len);
	r->n.nlmsg_len = NLMSG_ALIGN(r->n.nlmsg_len) + NLA_ALIGN(a->nla_len);
}

// The attribute `type` among those in [p, p + len), or NULL.
static struct nlattr *nl_find(void *p, int len, int type) {
	struct nlattr *a = p;
	while (len >= NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= len) {
		if ((a->nla_type & NLA_TYPE_MASK) == type) return a;
		len -= NLA_ALIGN(a->nla_len);
		a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
	}
	return NULL;
}

#define NLA_DATA(a) ((void *)((char *)(a) + NLA_HDRLEN))

// A generic netlink message's attributes, and their length.
static void *genl_attrs(struct nlmsghdr *h, int *len) {
	*len = (int)h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
	return (char *)NLMSG_DATA(h) + GENL_HDRLEN;
}

static char nl_buf[32768] __attribute__((aligned(4))); // the family reply lists every command

// nl80211's generic netlink family, or -1 if cfg80211 isn't loaded (yet).
static int nl80211_family(int fd) {
	struct nl_req r;
	nl_req_init(&r, GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY);
	nl_put(&r, CTRL_ATTR_FAMILY_NAME, "nl80211", sizeof "nl80211");
	if (send(fd, &r, r.n.nlmsg_len, 0) != (ssize_t)r.n.nlmsg_len) return -1;
	ssize_t got = recv(fd, nl_buf, sizeof nl_buf, 0);
	struct nlmsghdr *h = (struct nlmsghdr *)nl_buf;
	if (got < (ssize_t)NLMSG_HDRLEN || !NLMSG_OK(h, (size_t)got) || h->nlmsg_type != GENL_ID_CTRL) return -1;
	int len;
	void *attrs = genl_attrs(h, &len);
	struct nlattr *a = nl_find(attrs, len, CTRL_ATTR_FAMILY_ID);
	return a ? *(uint16_t *)NLA_DATA(a) : -1;
}

int wifi_signal(const char *ifname) {
	static int family = -1; // found once cfg80211 is there, and kept
	unsigned ifindex = if_nametoindex(ifname);
	if (!ifindex) return 0;
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (fd < 0) return 0;
	struct timeval tv = { 0, 200000 }; // the kernel answers at once; never hold up a frame long
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	int dbm = 0;
	if (connect(fd, (struct sockaddr *)&kernel, sizeof kernel) < 0) goto out;
	if (family < 0) family = nl80211_family(fd);
	if (family < 0) goto out;

	// Joined to a network, the one station is its access point.
	struct nl_req r;
	nl_req_init(&r, family, NLM_F_DUMP, NL80211_CMD_GET_STATION);
	uint32_t idx = ifindex;
	nl_put(&r, NL80211_ATTR_IFINDEX, &idx, sizeof idx);
	if (send(fd, &r, r.n.nlmsg_len, 0) != (ssize_t)r.n.nlmsg_len) goto out;
	for (int reads = 0; reads < 8; reads++) {
		ssize_t got = recv(fd, nl_buf, sizeof nl_buf, 0);
		if (got <= 0) break;
		size_t left = got;
		for (struct nlmsghdr *h = (struct nlmsghdr *)nl_buf; NLMSG_OK(h, left); h = NLMSG_NEXT(h, left)) {
			if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) goto out;
			if (h->nlmsg_type != family || dbm) continue;
			int len;
			void *attrs = genl_attrs(h, &len);
			struct nlattr *sta = nl_find(attrs, len, NL80211_ATTR_STA_INFO);
			if (!sta) continue;
			// The average if the driver keeps one, so the bars don't flicker with each frame.
			int slen = sta->nla_len - NLA_HDRLEN;
			struct nlattr *s = nl_find(NLA_DATA(sta), slen, NL80211_STA_INFO_SIGNAL_AVG);
			if (!s) s = nl_find(NLA_DATA(sta), slen, NL80211_STA_INFO_SIGNAL);
			if (s) dbm = *(int8_t *)NLA_DATA(s);
		}
	}
out:
	close(fd);
	return dbm < 0 ? dbm : 0;
}

void sysinfo_net(struct sprite_net *n) {
	memset(n, 0, sizeof *n);
	n->eth_up = if_up("eth0");
	n->eth_present = access("/sys/class/net/eth0", F_OK) == 0;
	if (n->eth_up) { // the kernel says -1 while it doesn't know
		int mbps = read_int("/sys/class/net/eth0/speed", 0);
		n->eth_mbps = mbps > 0 ? mbps : 0;
	}
	n->wifi_up = if_up("wlan0");
	n->wifi_present = access("/sys/class/net/wlan0", F_OK) == 0;
	if (n->wifi_present) {
		int flags = 0; // IFF_UP is 0x1: off when it's down administratively (not configured)
		FILE *f = fopen("/sys/class/net/wlan0/flags", "r");
		if (f) { if (fscanf(f, "%x", &flags) != 1) flags = 0; fclose(f); }
		n->wifi_enabled = flags & 1;
		for (int i = 0; i < 8; i++) { // a blocked Wi-Fi radio counts as off
			char path[64], type[16] = "";
			snprintf(path, sizeof path, "/sys/class/rfkill/rfkill%d/type", i);
			FILE *t = fopen(path, "r");
			if (!t) continue;
			if (!fgets(type, sizeof type, t)) type[0] = 0;
			fclose(t);
			if (strncmp(type, "wlan", 4)) continue;
			snprintf(path, sizeof path, "/sys/class/rfkill/rfkill%d/soft", i);
			if (read_int(path, 0)) n->wifi_enabled = false;
			snprintf(path, sizeof path, "/sys/class/rfkill/rfkill%d/hard", i);
			if (read_int(path, 0)) n->wifi_enabled = false;
		}
	}
	struct ifaddrs *ifa, *i;
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
			char *dst = !strcmp(i->ifa_name, "eth0") ? n->eth_ip : !strcmp(i->ifa_name, "wlan0") ? n->wifi_ip : NULL;
			if (dst) inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, dst, sizeof n->eth_ip);
		}
		freeifaddrs(ifa);
	}
	if (n->wifi_up) n->wifi_dbm = wifi_signal("wlan0");
	// of-netplan's notes for the login prompt (sysinfo_netinfo reads them): any at all?
	FILE *f = fopen("/var/lib/of-netplan/issue.note", "r");
	if (f) {
		char b[64];
		size_t len = fread(b, 1, sizeof b, f);
		for (size_t i = 0; i < len && !n->notes; i++) n->notes = b[i] > ' ';
		fclose(f);
	}
}

// Each interface's IPv4 address (the first with global scope), and whether it's permanent:
// networkd gives a fixed address (ethernet.txt, wifi.txt) no lifetime, so the kernel marks it
// permanent, and one from DHCP the lease's.
static void addresses(struct sprite_netinfo *o) {
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) return;
	struct timeval tv = { 0, 300000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	struct { struct nlmsghdr h; struct ifaddrmsg a; } req = {
		.h = { .nlmsg_len = sizeof req, .nlmsg_type = RTM_GETADDR, .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 1 },
		.a = { .ifa_family = AF_INET },
	};
	unsigned eth = if_nametoindex("eth0"), wifi = if_nametoindex("wlan0");
	if (send(fd, &req, sizeof req, 0) < 0) { close(fd); return; }
	char buf[8192] __attribute__((aligned(4)));
	for (bool done = false; !done;) {
		ssize_t len = recv(fd, buf, sizeof buf, 0);
		if (len <= 0) break;
		for (struct nlmsghdr *h = (void *)buf; NLMSG_OK(h, (size_t)len); h = NLMSG_NEXT(h, len)) {
			if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) { done = true; break; }
			if (h->nlmsg_type != RTM_NEWADDR) continue;
			struct ifaddrmsg *a = NLMSG_DATA(h);
			struct sprite_netif *nif = a->ifa_index == eth ? &o->eth : a->ifa_index == wifi ? &o->wifi : NULL;
			if (!nif || nif->ip[0] || a->ifa_scope != RT_SCOPE_UNIVERSE) continue;
			uint32_t flags = a->ifa_flags;
			struct in_addr addr = { 0 };
			bool have = false;
			int rlen = IFA_PAYLOAD(h);
			for (struct rtattr *r = IFA_RTA(a); RTA_OK(r, rlen); r = RTA_NEXT(r, rlen)) {
				if (r->rta_type == IFA_FLAGS) memcpy(&flags, RTA_DATA(r), sizeof flags);
				else if (r->rta_type == IFA_LOCAL) { memcpy(&addr, RTA_DATA(r), sizeof addr); have = true; }
			}
			if (!have) continue;
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &addr, ip, sizeof ip);
			snprintf(nif->ip, sizeof nif->ip, "%s/%u", ip, a->ifa_prefixlen);
			nif->fixed = flags & IFA_F_PERMANENT;
		}
	}
	close(fd);
}

// The default gateway through each interface (the lowest metric's), from the routing table.
static void gateways(struct sprite_netinfo *o) {
	FILE *f = fopen("/proc/net/route", "r");
	if (!f) return;
	char line[256], name[IFNAMSIZ];
	unsigned dest, gw, flags, metric;
	unsigned best_eth = ~0u, best_wifi = ~0u;
	if (!fgets(line, sizeof line, f)) { fclose(f); return; } // the heading
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "%15s %x %x %x %*d %*d %u", name, &dest, &gw, &flags, &metric) != 5) continue;
		if (dest || !(flags & 0x2)) continue; // RTF_GATEWAY on the default route
		struct sprite_netif *nif = !strcmp(name, "eth0") ? &o->eth : !strcmp(name, "wlan0") ? &o->wifi : NULL;
		unsigned *best = nif == &o->eth ? &best_eth : &best_wifi;
		if (!nif || metric >= *best) continue;
		*best = metric;
		struct in_addr a = { gw }; // as the kernel holds it, network order
		inet_ntop(AF_INET, &a, nif->gateway, sizeof nif->gateway);
	}
	fclose(f);
}

// The DNS servers in use: systemd-resolved's list of them (its own resolv.conf), IPv4 first.
static void dns_servers(struct sprite_netinfo *o) {
	FILE *f = fopen("/run/systemd/resolve/resolv.conf", "r");
	if (!f) f = fopen("/etc/resolv.conf", "r");
	if (!f) return;
	char line[160], all[8][46];
	int n = 0;
	while (n < 8 && fgets(line, sizeof line, f))
		if (sscanf(line, "nameserver %45s", all[n]) == 1 && strcmp(all[n], "127.0.0.53")) n++;
	fclose(f);
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < n && o->n_dns < NET_DNS_MAX; i++)
			if (!strchr(all[i], ':') == !pass)
				snprintf(o->dns[o->n_dns++], sizeof o->dns[0], "%.45s", all[i]);
}

void sysinfo_netinfo(struct sprite_netinfo *o) {
	memset(o, 0, sizeof *o);
	gethostname(o->host, sizeof o->host - 1);
	addresses(o);
	gateways(o);
	dns_servers(o);
	const char *names[] = { "eth0", "wlan0" };
	struct sprite_netif *ifs[] = { &o->eth, &o->wifi };
	for (int i = 0; i < 2; i++) {
		char path[64];
		snprintf(path, sizeof path, "/sys/class/net/%s/address", names[i]);
		FILE *f = fopen(path, "r");
		if (f) { if (fscanf(f, "%17s", ifs[i]->mac) != 1) ifs[i]->mac[0] = 0; fclose(f); }
	}
	// What of-netplan couldn't use in the settings files (it used to be on the login prompt).
	FILE *f = fopen("/var/lib/of-netplan/issue.note", "r");
	if (f) {
		size_t n = fread(o->notes, 1, sizeof o->notes - 1, f);
		o->notes[n] = 0;
		fclose(f);
		while (n && (o->notes[n - 1] == '\n' || o->notes[n - 1] == ' ')) o->notes[--n] = 0;
	}
}

// The first backlight's sysfs directory. (Read directly: a child process here would end in a
// SIGCHLD, and each one makes the shell rebuild its home grid.)
static int backlight_dir(char *out, size_t len) {
	DIR *d = opendir("/sys/class/backlight");
	if (!d) return -1;
	struct dirent *e;
	int r = -1;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		snprintf(out, len, "/sys/class/backlight/%.64s", e->d_name);
		r = 0;
		break;
	}
	closedir(d);
	return r;
}

int sysinfo_backlight_max(void) {
	static int max = -1;
	if (max > 0) return max;
	char dir[128], path[192];
	if (backlight_dir(dir, sizeof dir) != 0) return -1;
	snprintf(path, sizeof path, "%s/max_brightness", dir);
	max = read_int(path, -1);
	return max;
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
	snprintf(path, sizeof path, "%s/bl_power", dir);
	if (read_int(path, 0) != 0) return 0; // its power off: the screen's off, whatever the level
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

// The unit's name as its owner wrote it (the pretty hostname in /etc/machine-info, which
// openframe-scripts' name-settings keeps from name.txt), or the hostname without one; and
// the hostname.
void sysinfo_name(char *name, size_t n, char *host, size_t hn) {
	if (gethostname(host, hn) != 0) snprintf(host, hn, "openframe");
	host[hn - 1] = 0;
	snprintf(name, n, "%s", host);
	FILE *f = fopen("/etc/machine-info", "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "PRETTY_HOSTNAME=", 16)) continue;
		// "..." with \ escapes (as name-settings writes it), or bare
		const char *v = line + 16;
		bool q = *v == '"';
		if (q) v++;
		size_t o = 0;
		for (; *v && *v != '\n' && o + 1 < n; v++) {
			if (q && *v == '"') break;
			if (q && *v == '\\' && v[1]) v++;
			name[o++] = *v;
		}
		if (o) name[o] = 0;
	}
	fclose(f);
}

// A request straight into of-backlight's service (its control pipe, which anyone may write
// and it holds open): many a second while a slider's dragged, with no process started for
// each. Without the service there's no pipe, and nothing happens.
bool sysinfo_backlight_request(const char *req) {
	int fd = open("/run/of-backlight/control", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return false;
	char line[32];
	int n = snprintf(line, sizeof line, "%s\n", req);
	bool ok = n > 0 && n < (int)sizeof line && write(fd, line, n) == n;
	close(fd);
	return ok;
}

// The same pipe if the service is there, so it's at once and in order with what the slider
// sent; of-backlight itself if not (a fixed level, set without the service).
void sysinfo_set_backlight(const char *arg) {
	if (sysinfo_backlight_request(arg)) return;
	char cmd[64];
	snprintf(cmd, sizeof cmd, "of-backlight %s >/dev/null 2>&1", arg);
	sysinfo_run(cmd);
}

void sysinfo_summary(char *buf, size_t len) {
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
	const char *wstate = n.wifi_ip[0] ? n.wifi_ip : n.wifi_up ? STR_ABOUT_NO_ADDRESS : STR_ABOUT_NOT_CONNECTED;
	static const char *strength[] = { "", "weak", "fair", "good", "excellent" };
	if (n.wifi_ip[0] && n.wifi_dbm < 0)
		snprintf(wifi, sizeof wifi, STR_ABOUT_WIFI_SIGNAL, wstate, strength[wifi_level(n.wifi_dbm)]);
	else snprintf(wifi, sizeof wifi, "%s", wstate);
	snprintf(buf, len,
		 STR_ABOUT_SUMMARY,
		 n.eth_ip[0] ? n.eth_ip : n.eth_up ? STR_ABOUT_NO_ADDRESS : STR_ABOUT_NOT_CONNECTED,
		 wifi, mem_avail / 1024, mem_total / 1024, root_free, up / 3600, (up / 60) % 60, u.release,
		 SPRITE_VERSION,
		 // a test unit (OF-BOOT/settings/test.txt): openframe-linux's test-settings writes this list
		 access("/etc/apt/sources.list.d/openframe-testing.list", F_OK) == 0
			 ? STR_ABOUT_TEST_UNIT : "");
}
