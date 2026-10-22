#!/bin/sh
# Real browser: put a PNG on the X clipboard, open the page in Firefox, press Ctrl+V,
# and check the page put a valid richpaste:1: line back on the clipboard.
# Needs: firefox, xdotool, xsel, an X server.  Usage: DISPLAY=:91 tests/browser_page.sh
set -u
unset AGENTTY_HOME
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/rp_driver
OWNER=$ROOT/build/x11_owner
T=$(mktemp -d /tmp/rp-br.XXXXXX)
cleanup() { [ -n "${FF:-}" ] && kill "$FF" 2>/dev/null; [ -n "${OW:-}" ] && kill "$OW" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
"$BIN" page > "$T/rp.html"
head -c 400000 /dev/urandom > "$T/shot.png"
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
for _ in $(seq 1 90); do W=$(xdotool search --name 'richpaste.*Firefox' 2>/dev/null | head -1); [ -n "$W" ] && break; sleep 0.5; done
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
if [ -n "$P" ] && cmp -s "$P" "$T/shot.png"; then
    echo "ok   firefox: pasted image -> page -> richpaste:1: on clipboard -> byte-identical"
else
    echo "FAIL firefox page"; echo "clipboard starts: [$OUT]"; cat "$T/err"
    exit 1
fi
