#!/bin/sh
# Copy sprite, its apps and the run script to an OpenFrame, then (optionally) start it.
#   OFH=of@192.168.11.131 scripts/deploy.sh [run]
# Extra ssh options can be given in SSH_OPTS. The unit needs /opt/sprite from 'build.sh stack'.
set -e
cd "$(dirname "$0")/.."
: "${OFH:?set OFH to user@address of the OpenFrame}"
scp $SSH_OPTS -q out/sprite.stripped scripts/sprite-run apps/*.app "$OFH":/tmp/ </dev/null
ssh $SSH_OPTS "$OFH" 'sudo install -D -m 755 /tmp/sprite.stripped /opt/sprite/bin/sprite &&
	sudo install -D -m 755 /tmp/sprite-run /opt/sprite/bin/sprite-run &&
	sudo mkdir -p /opt/sprite/share/sprite/apps && sudo rm -f /opt/sprite/share/sprite/apps/*.app &&
	sudo install -m 644 /tmp/*.app /opt/sprite/share/sprite/apps/ && rm -f /tmp/sprite.stripped /tmp/sprite-run /tmp/*.app &&
	echo deployed: $(ls /opt/sprite/share/sprite/apps | wc -l) apps' </dev/null
if [ "$1" = run ]; then
	ssh $SSH_OPTS "$OFH" 'sudo systemd-run --unit=sprite --collect -p ExecStopPost="/bin/systemctl start getty@tty1" /opt/sprite/bin/sprite-run && echo started' </dev/null
fi
