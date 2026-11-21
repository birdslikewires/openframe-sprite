#!/bin/sh
# Install a locally built sprite package on an OpenFrame, with the demo apps.
#   scripts/build.sh package && OFH=of@192.168.11.131 scripts/deploy.sh
#   OFH=... scripts/deploy.sh noboot     back to the tty1 login at boot (and now)
#   OFH=... scripts/deploy.sh boot       sprite at boot again (and now)
# The package takes tty1 itself on its first install. Extra ssh options can be given in
# SSH_OPTS.
set -e
cd "$(dirname "$0")/.."
: "${OFH:?set OFH to user@address of the OpenFrame}"
deb=$(ls -t out/openframe-sprite_*_i386.deb 2>/dev/null | head -1)
T=/tmp/sprite-deploy
ssh $SSH_OPTS "$OFH" "rm -rf $T && mkdir -p $T/demo" </dev/null
[ -z "$1" ] && { [ -n "$deb" ] || { echo "no package in out/: scripts/build.sh package"; exit 1; }; }
[ -z "$1" ] && scp $SSH_OPTS -q "$deb" "$OFH":$T/ </dev/null
scp $SSH_OPTS -q apps/demo/*.app "$OFH":$T/demo/ </dev/null
ssh $SSH_OPTS "$OFH" "MODE='$1' T=$T sh -s" </dev/null <<'REMOTE'
set -e
# units installed by hand before sprite was packaged would hide the package's
if [ -e /etc/systemd/system/sprite.service ]; then
	sudo rm -f /etc/systemd/system/sprite.service /etc/systemd/system/sprite-helper.socket \
		/etc/systemd/system/sprite-helper@.service /etc/systemd/system/graphical.target.wants/sprite.service \
		/etc/systemd/system/sockets.target.wants/sprite-helper.socket
	sudo systemctl daemon-reload
fi
sudo install -d /usr/local/share/sprite/apps
sudo install -m 644 $T/demo/*.app /usr/local/share/sprite/apps/
case "$MODE" in
	boot)
		sudo systemctl disable getty@tty1.service >/dev/null 2>&1
		sudo systemctl enable sprite.service >/dev/null 2>&1
		sudo systemctl restart sprite.service
		echo "sprite starts at boot, and is running"
		;;
	noboot)
		sudo systemctl disable --now sprite.service >/dev/null 2>&1 || true
		sudo systemctl enable --now getty@tty1.service >/dev/null 2>&1
		echo "tty1 login at boot again"
		;;
	*)
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --reinstall --allow-downgrades $T/*.deb 2>&1 | grep -E "^Setting up|^E:" || true
		sudo apt-get clean
		echo "installed $(dpkg-query -W -f='${Version}' openframe-sprite); sprite is $(systemctl is-active sprite.service)"
		;;
esac
rm -rf $T
REMOTE
