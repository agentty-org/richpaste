#!/bin/sh
# Real browser: put a big screenshot-like PNG on the X clipboard, open the page in
# Firefox, press Ctrl+V, and check the page put a valid richpaste:1: line back on the
# clipboard that decodes to a smaller image of at most 1600 px on a side.
# Needs: firefox, xdotool, xsel, an X server.  Usage: DISPLAY=:91 tests/browser_page.sh
set -u
unset AGENTTY_HOME WAYLAND_DISPLAY   # firefox must open on $DISPLAY, not your desktop
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/rp_driver
OWNER=$ROOT/build/x11_owner
T=$(mktemp -d /tmp/rp-br.XXXXXX)
cleanup() { [ -n "${FF:-}" ] && kill "$FF" 2>/dev/null; [ -n "${OW:-}" ] && kill "$OW" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
"$BIN" page > "$T/rp.html"
python3 - "$T/shot.png" <<'EOF'
import sys, zlib, struct
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
w, h = 2880, 1800   # a retina screenshot: big, mostly flat colour with some detail
rows = []
for y in range(h):
    r = bytearray(b'\x00')
    for x in range(w):
        r += bytes(((x // 40) * 9 & 255, (y // 30) * 7 & 255, (x * y) & 255 if (x // 200 + y // 120) % 7 == 0 else 200))
    rows.append(bytes(r))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
EOF
"$OWNER" "${DISPLAY#:}" image/png "$T/shot.png" >/dev/null & OW=$!
sleep 0.3
mkdir -p "$T/prof"
# Let file:// pages write to the clipboard without a prompt, like a normal click would.
cat > "$T/prof/user.js" <<'EOF'
user_pref("dom.events.asyncClipboard.clipboardItem", true);
user_pref("dom.events.testing.asyncClipboard", true);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("browser.aboutwelcome.enabled", false);
EOF
firefox --no-remote --profile "$T/prof" --new-window "file://$T/rp.html" >"$T/ff.log" 2>&1 & FF=$!
W=
for _ in $(seq 1 90); do W=$(xdotool search --name 'agentty paste.*Firefox' 2>/dev/null | head -1); [ -n "$W" ] && break; sleep 0.5; done
[ -z "$W" ] && { echo "FAIL firefox window never appeared"; tail -5 "$T/ff.log"; exit 1; }
sleep 2
xdotool windowfocus "$W" 2>/dev/null; sleep 0.3
xdotool mousemove --window "$W" 400 300 click 1; sleep 0.3
xdotool key --window "$W" ctrl+v
OUT=
for _ in $(seq 1 30); do
    OUT=$(xsel -b -o 2>/dev/null | head -c 20)
    case "$OUT" in richpaste:1:*) break ;; esac
    sleep 0.5
done
xsel -b -o > "$T/clip.txt" 2>/dev/null
P=$("$BIN" decode "$T/out" < "$T/clip.txt" 2>"$T/err" | head -1)
# The page scales a screenshot to <= 1600 px and keeps the smallest of PNG/WebP/JPEG.
DIMS=$(python3 - "$P" <<'EOF' 2>/dev/null
import sys, struct
b = open(sys.argv[1], 'rb').read()
if b[:8] == b'\x89PNG\r\n\x1a\n': print(*struct.unpack('>II', b[16:24]))
elif b[:4] == b'RIFF' and b[8:12] == b'WEBP': print('webp')
elif b[:2] == b'\xff\xd8': print('jpeg')
EOF
)
IN=$(wc -c < "$T/shot.png"); OUTSZ=$(wc -c < "$T/clip.txt")
if [ -n "$P" ] && [ -n "$DIMS" ] && [ "$OUTSZ" -lt "$IN" ] && [ "$OUTSZ" -le 1600000 ]; then
    echo "ok   firefox: $IN-byte screenshot -> page -> $OUTSZ-char line -> valid image ($DIMS)"
else
    echo "FAIL firefox page"; echo "clipboard starts: [$OUT]"; cat "$T/err"
    exit 1
fi
