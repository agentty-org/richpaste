#!/bin/sh
# Zero-install paste through the terminal itself, in a real kitty window.
# The app can't see any display or agent, so the only way in is OSC 5522 / OSC 52.
# Needs: kitty, tmux, an X server for kitty (Xvfb is fine), xsel.
# Usage: DISPLAY=:N tests/inband_kitty.sh
set -u
unset AGENTTY_HOME
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/rp_driver
OWNER=$ROOT/build/x11_owner
T=$(mktemp -d /tmp/rp-ib.XXXXXX)
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL $1"; }
DNUM=${DISPLAY#:}
cleanup() { [ -n "${OW:-}" ] && kill "$OW" 2>/dev/null; tmux -L rpib kill-server 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

# kitty must be allowed to answer clipboard reads; that's the user's choice in kitty.conf.
printf 'clipboard_control write-clipboard write-primary read-clipboard read-primary\nconfirm_os_window_close 0\n' > "$T/kitty.conf"

# The app runs with no display env, no agent and no forwards: only the terminal can help.
# Also drop this shell's own tmux/ssh vars so they don't leak into the test terminals,
# and WAYLAND_DISPLAY so kitty opens on $DISPLAY (the test X server), not your desktop.
unset TMUX TMUX_PANE SSH_CONNECTION SSH_TTY SSH_CLIENT WAYLAND_DISPLAY
APPENV="env -u DISPLAY -u WAYLAND_DISPLAY RICHPASTE_NO_DISPLAY=1 HOME=$T/home"
mkdir -p "$T/home"

run_kitty() {  # $1 = command inside kitty
    LIBGL_ALWAYS_SOFTWARE=1 timeout 40 kitty --config "$T/kitty.conf" sh -c "$1" >/dev/null 2>&1
}

# --- text through OSC 52 (forced, so kitty takes the plain OSC 52 path) ---
# ASCII on purpose: under Xvfb, kitty's OSC 52 reply sometimes drops non-ASCII chars
# (a bare python probe sees the same truncated base64 from kitty), so it isn't our decoder.
setclip() {  # set the clipboard and wait until it reads back
    printf '%s' "$1" | xsel -b -i
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ "$(xsel -b -o 2>/dev/null)" = "$1" ] && break; sleep 0.1; done
    sleep 0.3
}
setclip 'plain text via osc52 ok'
run_kitty "RICHPASTE_TERMINAL=osc52 $APPENV $BIN paste > $T/out3 2>&1"
[ "$(cat "$T/out3")" = 'plain text via osc52 ok' ] && ok "kitty: text via OSC 52" || { bad "osc52"; cat "$T/out3"; }

# --- image through OSC 5522, kitty directly ---
head -c 150000 /dev/urandom > "$T/shot.png"
WANT=$(sha256sum < "$T/shot.png" | cut -c1-16)
"$OWNER" "$DNUM" image/png "$T/shot.png" >/dev/null & OW=$!
sleep 0.3
run_kitty "$APPENV $BIN doctor > $T/out1 2>&1"
grep -q "ok via terminal (OSC 5522)" "$T/out1" && grep -q "$WANT" "$T/out1" \
    && ok "kitty: image via OSC 5522, sha matches" || { bad "kitty 5522"; cat "$T/out1"; }

# --- image through OSC 5522, inside tmux (passthrough) ---
run_kitty "tmux -L rpib -f /dev/null new '$APPENV $BIN doctor > $T/out2 2>&1'"
grep -q "ok via terminal (OSC 5522)" "$T/out2" && grep -q "$WANT" "$T/out2" \
    && ok "kitty + tmux: image via passthrough, sha matches" || { bad "kitty+tmux 5522"; cat "$T/out2"; }

# --- over ssh in tmux, image on the clipboard, no agent: kitty is asked first
# (it sends images), not after tmux's text-only read has timed out ---
cat > "$T/order.sh" <<EOF
S=\$(date +%s%N)
$APPENV SSH_CONNECTION=x RICHPASTE_NO_FORWARDS=1 $BIN doctor > $T/out2b 2>&1
echo ms=\$(( (\$(date +%s%N)-S)/1000000 )) >> $T/out2b
EOF
run_kitty "tmux -L rpib -f /dev/null new 'sh $T/order.sh'"
MS=$(sed -n 's/^ms=//p' "$T/out2b")
grep -q "plan .*terminal -> tmux_buffer" "$T/out2b" && grep -q "ok via terminal (OSC 5522)" "$T/out2b" && grep -q "$WANT" "$T/out2b" \
    && [ "${MS:-99999}" -lt 2500 ] \
    && ok "kitty + tmux over ssh: image first try (${MS} ms)" || { bad "kitty+tmux ssh image order"; cat "$T/out2b"; }
kill "$OW"; wait "$OW" 2>/dev/null; OW=
# tmux setting must be per-pane only; the global one must be untouched.
tmux -L rpib kill-server 2>/dev/null

# --- text through tmux asking the outer terminal (refresh-client -l), default tmux config ---
setclip 'text via tmux refresh'
run_kitty "tmux -L rpib -f /dev/null new 'RICHPASTE_TERMINAL=osc52 $APPENV $BIN paste > $T/out4 2>&1'"
[ "$(cat "$T/out4")" = 'text via tmux refresh' ] && ok "kitty + tmux: text via tmux's own OSC 52" || { bad "tmux refresh"; cat "$T/out4"; }
tmux -L rpib kill-server 2>/dev/null

# --- terminal that won't answer (wezterm doesn't do clipboard reads): fails fast, clear message ---
if command -v wezterm >/dev/null; then
    START=$(date +%s)
    LIBGL_ALWAYS_SOFTWARE=1 timeout 30 wezterm --config 'front_end="Software"' --config 'window_close_confirmation="NeverPrompt"' \
        start --always-new-process -- sh -c "$APPENV $BIN paste > $T/out5 2>&1" >/dev/null 2>&1
    END=$(date +%s)
    grep -q "rp_driver setup" "$T/out5" && [ $((END-START)) -lt 15 ] \
        && ok "wezterm (no clipboard reads): clear error in $((END-START))s" || { bad "silent terminal"; cat "$T/out5"; }
fi

# --- image copied *as text* (snippet/web page output), Ctrl+V in kitty via plain OSC 52 ---
"$BIN" encode "$T/shot.png" > "$T/armored.txt"
xsel -b -i < "$T/armored.txt"; sleep 0.5
run_kitty "RICHPASTE_TERMINAL=osc52 $APPENV $BIN paste -o $T/out8d > $T/out8 2>&1"
P=$(head -1 "$T/out8")
[ -f "$P" ] && cmp -s "$P" "$T/shot.png" && ok "image copied as text: Ctrl+V via OSC 52 gives the image" || { bad "armored via osc52"; head -c 300 "$T/out8"; }

# --- nothing installed on the laptop: kitty -> ssh -> tmux on the server -> image ---
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
ssh-keygen -q -t ed25519 -N '' -f "$T/hk"; ssh-keygen -q -t ed25519 -N '' -f "$T/uk"; cp "$T/uk.pub" "$T/ak"
printf 'Port %s\nListenAddress 127.0.0.1\nHostKey %s/hk\nPidFile %s/sshd.pid\nAuthorizedKeysFile %s/ak\nStrictModes no\nUsePAM no\nPasswordAuthentication no\n' "$PORT" "$T" "$T" "$T" > "$T/sc"
/usr/bin/sshd -f "$T/sc" -E "$T/sshd.log"
sleep 0.3
"$OWNER" "$DNUM" image/png "$T/shot.png" >/dev/null & OW=$!
sleep 0.3
SSHC="ssh -F /dev/null -tt -p $PORT -i $T/uk -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR 127.0.0.1"
# No forwards: this case is about the terminal route alone.
run_kitty "$SSHC \"tmux -L rpib -f /dev/null new '$APPENV RICHPASTE_NO_FORWARDS=1 $BIN doctor > $T/out7 2>&1'\""
grep -q "ok via terminal (OSC 5522)" "$T/out7" && grep -q "$WANT" "$T/out7" \
    && ok "kitty -> ssh -> tmux, no agent: image via OSC 5522" || { bad "ssh no-agent"; cat "$T/out7"; }
kill "$(cat "$T/sshd.pid")" 2>/dev/null
tmux -L rpib kill-server 2>/dev/null

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
