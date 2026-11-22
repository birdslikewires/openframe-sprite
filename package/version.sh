#!/bin/bash
# Prints the openframe-sprite version, from the repository's root with its full history;
# with --news, the owners' notes for every version (below).
#
# Version is year.month.N, where N counts the commits that changed the package in that month
# (the third this month is 26.11.3), as for the other OpenFrame packages. Every path the
# package is built from is listed below, the workflow included, so a change to any of them
# gives a new version.

[ "${1:-}" = --news ] && NEWS=1
paths=(src apps/settings.app build package system settings Makefile scripts/fetch-lvgl.sh .github/workflows/package.yml)
EPOCH=""
# With --news: the owners' notes instead, from the "News:" trailers of the commits that made
# each version, newest first, a line each: the version, a tab, the note. A commit may carry
# several; most carry none ("Bug fixes and improvements", as Sprite puts it).
if [ "${NEWS:-}" ]; then
	TZ=UTC git log --reverse --format='%H %cd' --date=format-local:%y.%m -- "${paths[@]}" |
		awk '{ n[$2]++; m = $2; sub(/\..*/, "", m); mm = substr($2, 4) + 0; print $1, m "." mm "." n[$2] }' |
		while read -r hash ver; do
			i=$((${i:-0} + 1))
			git log -1 --format='%(trailers:key=News,valueonly,unfold)' "$hash" | sed '/^$/d' |
				awk -v i="$i" -v v="$EPOCH$ver" '{ print i "\t" NR "\t" v "\t" $0 }'
		done | sort -t "$(printf '\t')" -k1,1nr -k2,2n | cut -f3-
	exit 0
fi

month=$(TZ=UTC git log -1 --format=%cd --date=format-local:%y.%m -- "${paths[@]}")
count=$(TZ=UTC git log --format=%cd --date=format-local:%y.%m -- "${paths[@]}" | grep -cx "${month}")
[ -n "$month" ] && [ "$count" -gt 0 ] || { echo "Can't work out the version: no history?" >&2; exit 1; }
echo "${month%.*}.$((10#${month#*.})).${count}"
