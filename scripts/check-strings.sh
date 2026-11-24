#!/bin/sh
# check-strings.sh: src/strings/en_gb.txt against what uses it. Every reason sprite-helper sends as a
# key (helper.*) is in the file, and every message in the file is used: by the code, as
# TX_<KEY>, or by the helper. Run by the Makefile as it makes build/gen/sprite_strings.h.
cd "$(dirname "$0")/.."
T=src/strings/en_gb.txt
keys=$(sed -n 's/^[[:space:]]*\([a-z0-9_.]*\)[[:space:]]*=.*/\1/p' "$T")
bad=0
for k in $(grep -o '"error: helper\.[a-z0-9_]*"' system/sprite-helper src/*.c | sed 's/.*"error: \(helper\.[a-z0-9_]*\)"/\1/' | sort -u); do
	echo "$keys" | grep -qx "$k" || { echo "$T: no $k, which sprite-helper or Sprite sends"; bad=1; }
done
for k in $keys; do
	case $k in
		helper.*) grep -q "\"error: $k\"" system/sprite-helper src/*.c ;;
		*) grep -q "STR_$(echo "$k" | tr 'a-z.' 'A-Z_')\b" src/*.c ;;
	esac || { echo "$T: $k isn't used"; bad=1; }
done
exit $bad
