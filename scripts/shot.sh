#!/bin/sh
# Take a screenshot of sprite's shell on an OpenFrame, as out/shot.png.
#   OFH=of@192.168.11.131 scripts/shot.sh [name]
set -e
cd "$(dirname "$0")/.."
: "${OFH:?set OFH to user@address of the OpenFrame}"
name=${1:-shot}
ssh $SSH_OPTS "$OFH" 'pkill -USR1 -x sprite; sleep 0.5; cat "$XDG_RUNTIME_DIR/sprite-shot.ppm" 2>/dev/null || cat /run/user/$(id -u)/sprite-shot.ppm' </dev/null > "out/$name.ppm"
python3 - "out/$name.ppm" "out/$name.png" <<'PY'
import sys, zlib, struct
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
raw = b''.join(b'\x00' + px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
PY
rm -f "out/$name.ppm"
echo "out/$name.png"
