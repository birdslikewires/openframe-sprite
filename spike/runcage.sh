#!/bin/sh
# runcage.sh <client args...>: run wlbench under cage on the panel, report cage's CPU and RSS.
# Run as root. Leaves getty@tty1 stopped; restart it with: systemctl start getty@tty1
cd /home/of/sprite-spike
systemctl stop getty@tty1 2>/dev/null
export XDG_RUNTIME_DIR=/run/sprite-spike; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
export LIBSEAT_BACKEND=builtin WLR_RENDERER=pixman WLR_NO_HARDWARE_CURSORS=1
CLK=$(getconf CLK_TCK)
cage -d -- sh -c "sleep 1; ./wlbench $*; sleep 0.3" > cage.log 2>&1 &
sleep 0.5
CPID=$(pgrep -o -x cage)
t0=$(cut -d' ' -f14,15 /proc/$CPID/stat 2>/dev/null)
s0=$(date +%s.%N)
rss=0
while kill -0 $CPID 2>/dev/null; do
	r=$(awk '/VmRSS/{print $2}' /proc/$CPID/status 2>/dev/null); [ -n "$r" ] && [ "$r" -gt "$rss" ] && rss=$r
	t1=$(cut -d' ' -f14,15 /proc/$CPID/stat 2>/dev/null); s1=$(date +%s.%N)
	sleep 0.2
done
echo "cage cpu: $(echo "$t0 $t1 $s0 $s1 $CLK" | awk '{c=($3+$4-$1-$2)/$7; e=$6-$5; printf "%.2f s over %.1f s = %.0f%% of one thread", c, e, 100*c/e}')"
echo "cage peak RSS: $((rss/1024)) MB"
grep -E "wlbench|surface|frames|interval|flags|client|idle" cage.log
