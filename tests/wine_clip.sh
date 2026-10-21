#!/bin/sh
# Windows clipboard + agent tests under wine. Needs a display for wine (Xvfb is fine).
# Usage: tests/wine_clip.sh build-win
set -u
B=$(cd "$1" && pwd)
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-/tmp/rp-wine}
C=$WINEPREFIX/drive_c
mkdir -p "$C"
cp "$B"/*.exe "$C"/
cd "$C" || exit 1
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL $1"; }
w() { timeout 60 wine "$@" </dev/null 2>/dev/null | tr -d '\r'; }

# wine's clipboard lives in the explorer process; make sure it's up before we set data.
timeout 20 wine 'C:\richpaste.exe' >/dev/null 2>&1 </dev/null

printf 'héllo from win32 ✓ 中文 👩‍💻' > "$C/t.txt"
w 'C:\win_setclip.exe' text 'C:\t.txt'
OUT=$(w 'C:\richpaste.exe' paste)
[ "$OUT" = 'héllo from win32 ✓ 中文 👩‍💻' ] && ok "unicode text (incl. emoji surrogates)" || { bad "unicode text"; echo "got: [$OUT]"; }

w 'C:\win_setclip.exe' dib 320 200
OUT=$(w 'C:\richpaste.exe' paste -o 'C:\out')
P=$C/out/$(basename "$(echo "$OUT" | tr '\\' '/')")
python3 - "$P" <<'EOF' && ok "CF_DIB bitmap -> valid PNG with right pixels" || bad "dib"
import struct, sys, zlib
b = open(sys.argv[1], "rb").read()
assert b[:8] == b"\x89PNG\r\n\x1a\n"
w, h = struct.unpack(">II", b[16:24]); assert (w, h) == (320, 200), (w, h)
pos, idat = 8, b""
while pos < len(b):
    n, = struct.unpack(">I", b[pos:pos+4]); t = b[pos+4:pos+8]
    if t == b"IDAT": idat += b[pos+8:pos+8+n]
    pos += 12 + n
raw = zlib.decompress(idat); s = w*4; prev = bytearray(s); rows = []
for y in range(h):
    f = raw[y*(s+1)]; line = bytearray(raw[y*(s+1)+1:(y+1)*(s+1)])
    for i in range(s):
        l = line[i-4] if i >= 4 else 0
        line[i] = (line[i] + (l if f == 1 else prev[i] if f == 2 else 0)) & 255
    rows.append(line); prev = line
for (x, y) in [(0, 0), (319, 0), (0, 199), (123, 77)]:
    px = tuple(rows[y][x*4:x*4+4])
    want = ((x + y) & 255, (y * 5) & 255, (x * 3) & 255, 255)
    assert px == want, ((x, y), px, want)
EOF

head -c 200000 /dev/urandom > "$C/shot.png"
w 'C:\win_setclip.exe' png 'C:\shot.png'
OUT=$(w 'C:\richpaste.exe' paste -o 'C:\out2')
P=$C/out2/$(basename "$(echo "$OUT" | tr '\\' '/')")
[ -f "$P" ] && cmp -s "$P" "$C/shot.png" && ok "registered PNG format, byte-identical" || bad "png format"

printf 'report body' > "$C/report.txt"; head -c 5000 /dev/urandom > "$C/pic.bin"
w 'C:\win_setclip.exe' files 'C:\report.txt' 'C:\pic.bin'
OUT=$(w 'C:\richpaste.exe' paste -o 'C:\out3')
N=$(echo "$OUT" | grep -c .)
F1=$(ls "$C"/out3/*report.txt 2>/dev/null | head -1)
[ "$N" = 2 ] && [ -n "$F1" ] && cmp -s "$F1" "$C/report.txt" && ok "Explorer file copy (CF_HDROP), 2 files" || { bad "hdrop"; echo "$OUT"; }

# Agent on loopback TCP with a token; a client with the wrong token is refused.
printf 'via-agent' > "$C/t2.txt"
w 'C:\win_setclip.exe' text 'C:\t2.txt'
timeout 30 wine 'C:\richpaste.exe' agent </dev/null >/dev/null 2>"$C/agent.err" &
AG=$!
sleep 3
INFO=$(cat "$C/users/$(whoami)/.richpaste/agent.tcp" 2>/dev/null || cat "$WINEPREFIX"/drive_c/users/*/.richpaste/agent.tcp 2>/dev/null)
PORT=${INFO%% *}
[ -n "$PORT" ] && ok "agent wrote agent.tcp (port $PORT)" || bad "agent.tcp"
python3 - "$INFO" <<'EOF' && ok "agent serves right token, refuses wrong one" || bad "token check"
import socket, struct, sys
port, tok = sys.argv[1].split()
def ask(t):
    s = socket.create_connection(("127.0.0.1", int(port)), timeout=10)
    s.sendall(b"RPQ1" + struct.pack("<IQIH", 1, 1 << 20, 0, len(t)) + t.encode())
    d = b""
    while True:
        c = s.recv(65536)
        if not c: break
        d += c
    return d
good, bad = ask(tok), ask("x" * len(tok))
assert good[:5] == b"RPA1\x00" and b"via-agent" in good, good[:60]
assert bad[:5] == b"RPA1\x01" and b"wrong agent token" in bad, bad
EOF
kill $AG 2>/dev/null

# Windows agent -> ssh RemoteForward (unix socket on the server -> TCP on Windows) -> Linux app.
# Wine has no OpenSSH, so the Linux ssh client stands in for Windows' ssh.exe; the forward
# target and the agent on the other end are the real Windows ones.
LINUX_BIN=${LINUX_BIN:-$(cd "$B/../build" 2>/dev/null && pwd)/richpaste}
if [ -x "$LINUX_BIN" ]; then
    head -c 300000 /dev/urandom > "$C/w.png"
    w 'C:\win_setclip.exe' png 'C:\w.png'
    timeout 60 wine 'C:\richpaste.exe' agent </dev/null >/dev/null 2>&1 &
    AG=$!
    sleep 3
    INFO=$(cat "$WINEPREFIX"/drive_c/users/*/.richpaste/agent.tcp)
    PORT=${INFO%% *}; TOKEN=${INFO#* }
    S=$(mktemp -d /tmp/rp-wssh.XXXXXX)
    SP=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
    ssh-keygen -q -t ed25519 -N '' -f "$S/hk"; ssh-keygen -q -t ed25519 -N '' -f "$S/uk"; cp "$S/uk.pub" "$S/ak"
    printf 'Port %s\nListenAddress 127.0.0.1\nHostKey %s/hk\nPidFile %s/pid\nAuthorizedKeysFile %s/ak\nStrictModes no\nUsePAM no\nPasswordAuthentication no\n' "$SP" "$S" "$S" "$S" > "$S/sc"
    /usr/bin/sshd -f "$S/sc" -E "$S/log"; sleep 0.3
    SOCK=/tmp/richpaste-$(id -un)-wine$$.sock
    # Same-user path: a Windows process of this user may connect without the token.
    NT=$(timeout 20 wine 'C:\richpaste.exe' test-fetch-notoken </dev/null 2>/dev/null | tr -d '\r')
    [ "$NT" = served ] && ok "same Windows user is let in without the token" || { bad "same-user check"; echo "got: $NT"; }
    # Transport: the forwarding ssh here is a Linux process (wine can't vouch for it), so it
    # carries the token. On Windows, ssh.exe runs as you and the same-user check covers it.
    OUT=$(ssh -p "$SP" -i "$S/uk" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
        -R "$SOCK:127.0.0.1:$PORT" 127.0.0.1 \
        "RICHPASTE_NO_DISPLAY=1 RICHPASTE_TOKEN=$TOKEN $LINUX_BIN paste -o $S/out" 2>&1)
    P=$(echo "$OUT" | head -1)
    [ -f "$P" ] && cmp -s "$P" "$C/w.png" && ok "Windows agent -> ssh -R -> Linux: 300 KB image byte-identical" || { bad "windows over ssh"; echo "$OUT"; }
    kill "$(cat "$S/pid")" 2>/dev/null; kill $AG 2>/dev/null; rm -rf "$S" "$SOCK"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
