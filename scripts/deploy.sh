#!/bin/sh
# Install sprite on an OpenFrame, from out/ and this repository.
#   OFH=of@192.168.11.131 scripts/deploy.sh          install; restart sprite if it's running
#   OFH=... scripts/deploy.sh boot                   install, and start sprite at every boot (now too)
#   OFH=... scripts/deploy.sh noboot                 back to the tty1 login at boot (and now)
# Extra ssh options can be given in SSH_OPTS. The unit needs /opt/sprite from 'build.sh stack'.
set -e
cd "$(dirname "$0")/.."
: "${OFH:?set OFH to user@address of the OpenFrame}"
T=/tmp/sprite-deploy
ssh $SSH_OPTS "$OFH" "rm -rf $T && mkdir -p $T/apps" </dev/null
scp $SSH_OPTS -q out/sprite.stripped system/* settings/sprite.txt "$OFH":$T/ </dev/null
scp $SSH_OPTS -q apps/*.app "$OFH":$T/apps/ </dev/null
ssh $SSH_OPTS "$OFH" "MODE='$1' T=$T sh -s" </dev/null <<'REMOTE'
set -e
sudo install -D -m 755 $T/sprite.stripped /opt/sprite/bin/sprite
sudo install -D -m 755 $T/sprite-helper /opt/sprite/libexec/sprite-helper
sudo install -D -m 644 $T/sprite.txt /opt/sprite/share/sprite/sprite.txt
sudo mkdir -p /opt/sprite/share/sprite/apps
sudo rm -f /opt/sprite/share/sprite/apps/*.app
sudo install -m 644 $T/apps/*.app /opt/sprite/share/sprite/apps/
sudo install -m 644 $T/sprite.service $T/sprite-helper.socket $T/sprite-helper@.service /etc/systemd/system/
# the settings file, so it's there to edit from a PC
if [ ! -f /boot/settings/sprite.txt ]; then
	sudo /usr/lib/openframe/bootrw open && sudo cp $T/sprite.txt /boot/settings/sprite.txt.new && sync &&
		sudo mv /boot/settings/sprite.txt.new /boot/settings/sprite.txt && sync
	sudo /usr/lib/openframe/bootrw close
fi
rm -rf $T
sudo systemctl daemon-reload
sudo systemctl enable --now sprite-helper.socket >/dev/null 2>&1
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
		if systemctl is-active -q sprite.service; then sudo systemctl restart sprite.service; echo "sprite restarted"
		else echo "installed (not running; 'boot' starts it)"; fi
		;;
esac
REMOTE
