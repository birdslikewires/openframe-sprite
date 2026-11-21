#!/bin/sh
# runapp.sh <command...>: run a client under the Mesa-free cage on the panel; report CPU and peak RSS
# of cage and Xwayland. Run as root. Leaves getty@tty1 stopped.
cd /home/of/sprite-spike
systemctl stop getty@tty1 2>/dev/null
export XDG_RUNTIME_DIR=/run/sprite-spike; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
export LIBSEAT_BACKEND=builtin WLR_RENDERER=pixman WLR_NO_HARDWARE_CURSORS=1
export LD_LIBRARY_PATH=/opt/sprite/lib/i386-linux-gnu:/opt/sprite/lib PATH=/opt/sprite/bin:$PATH
CLK=$(getconf CLK_TCK)
cage -d -- sh -c "sleep 1; $*; sleep 0.3" > app.log 2>&1 &
sleep 0.5
CPID=$(pgrep -o -x cage)
s0=$(date +%s.%N); rc=0; rx=0; cc=0; cx=0
while kill -0 $CPID 2>/dev/null; do
	XPID=$(pgrep -o -x Xwayland)
	r=$(awk '/VmRSS/{print $2}' /proc/$CPID/status 2>/dev/null); [ -n "$r" ] && [ "$r" -gt "$rc" ] && rc=$r
	c=$(awk '{print $14+$15}' /proc/$CPID/stat 2>/dev/null); [ -n "$c" ] && cc=$c
	if [ -n "$XPID" ]; then
		r=$(awk '/VmRSS/{print $2}' /proc/$XPID/status 2>/dev/null); [ -n "$r" ] && [ "$r" -gt "$rx" ] && rx=$r
		c=$(awk '{print $14+$15}' /proc/$XPID/stat 2>/dev/null); [ -n "$c" ] && cx=$c
	fi
	sleep 0.2
done
s1=$(date +%s.%N)
echo "$cc $cx $s0 $s1 $CLK $rc $rx" | awk '{e=$4-$3; printf "cage cpu %.2f s, Xwayland cpu %.2f s, over %.1f s; peak RSS cage %d MB, Xwayland %d MB\n", $1/$5, $2/$5, e, $6/1024, $7/1024}'
grep -vE "^\s*$|ERROR.*xdg_surface" app.log | tail -15
