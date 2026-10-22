#!/bin/sh
# End-to-end: laptop agent -> ssh RemoteForward -> remote app, plain, inside tmux,
# and from a tmux pane that outlived its ssh session.
#
# Runs a throwaway sshd on 127.0.0.1:22999 with its own keys. Touches nothing in ~/.ssh.
set -eu
unset AGENTTY_HOME

# Resolve symlinks: `pkill -f "$BIN agent"` must match the path the agent was
# started with, or a stray agent outlives the test and answers the next one.
BIN=$(readlink -f "$(cd "$(dirname "$0")/.." && pwd)/build/rp_driver")
T=$(mktemp -d /tmp/rp-e2e.XXXXXX)
LAPTOP_HOME=$T/laptop          # HOME of the "machine you type on"
mkdir -p "$LAPTOP_HOME/.agentty/paste"
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "ok   $1"; }
bad()  { FAIL=$((FAIL+1)); echo "FAIL $1"; }

cleanup() {
    [ -f "$T/sshd.pid" ] && kill "$(cat "$T/sshd.pid")" 2>/dev/null || true
    [ -n "${AGENT:-}" ] && kill "$AGENT" 2>/dev/null || true
    pkill -f "$BIN agent" 2>/dev/null || true
    tmux -L rpe2e kill-server 2>/dev/null || true
    rm -f /tmp/richpaste-"$(id -un)"-*.sock
    rm -rf "$T"
}
trap cleanup EXIT

# --- throwaway sshd on a free port ---
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
ssh-keygen -q -t ed25519 -N '' -f "$T/hostkey"
ssh-keygen -q -t ed25519 -N '' -f "$T/userkey"
cp "$T/userkey.pub" "$T/authorized_keys"
cat > "$T/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $T/hostkey
PidFile $T/sshd.pid
AuthorizedKeysFile $T/authorized_keys
StrictModes no
UsePAM no
PasswordAuthentication no
EOF
/usr/bin/sshd -f "$T/sshd_config" -E "$T/sshd.log"
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -f "$T/sshd.pid" ] && break; sleep 0.1; done

# Nothing in this test may read your real clipboard, including the agent setup starts.
export RICHPASTE_NO_DISPLAY=1
# ssh config exactly as `agentty paste setup` writes it, plus the test host.
HOME=$LAPTOP_HOME "$BIN" setup >/dev/null 2>&1 || true
SNIPPET=$(sed -n '/# richpaste:/,/# richpaste end/p' "$LAPTOP_HOME/.ssh/config")
cat > "$T/ssh_config" <<EOF
$SNIPPET
Host rptest
  HostName 127.0.0.1
  Port $PORT
  IdentityFile $T/userkey
  IdentitiesOnly yes
  StrictHostKeyChecking no
  UserKnownHostsFile /dev/null
  LogLevel ERROR
EOF
# setup started a real agent for that HOME; swap in one that serves a fixed PNG.
pkill -f "$BIN agent" 2>/dev/null || true
sleep 0.2
head -c 2500000 /dev/urandom > "$T/shot.png"
WANT=$(sha256sum < "$T/shot.png" | cut -d' ' -f1)
"$BIN" serve-file "$LAPTOP_HOME/.agentty/paste/agent.sock" "$T/shot.png" image 2>"$T/agent.log" &
AGENT=$!
sleep 0.3

sshr() { HOME=$LAPTOP_HOME ssh -F "$T/ssh_config" rptest "$@"; }
got_sha() { sed -n 's/.*sha256 \([0-9a-f]*\).*/\1/p' | head -1; }

# 1. plain ssh
OUT=$(sshr "$BIN doctor" 2>&1 || true)
echo "$OUT" | grep -q "paste          ok via ssh" && [ "$(echo "$OUT" | got_sha)" = "$(echo "$WANT" | cut -c1-16)" ] \
    && ok "ssh: 2.4 MB image arrives intact" || { bad "ssh"; echo "$OUT"; }

# 2. real bytes, not just the hash prefix
sshr "$BIN paste -o /tmp/rp-e2e-out" > "$T/paths" 2>&1 || true
P=$(head -1 "$T/paths")
[ -f "$P" ] && [ "$(sha256sum < "$P" | cut -d' ' -f1)" = "$WANT" ] \
    && ok "ssh: saved file is byte-identical" || { bad "saved file"; cat "$T/paths"; }
rm -rf /tmp/rp-e2e-out

# 3. inside tmux, inside ssh. Hold the session open while the pane runs.
sshr "tmux -L rpe2e new-session -d -s s '$BIN doctor > $T/tmux1.out 2>&1; sleep 30'; sleep 1.5; cat $T/tmux1.out" > "$T/t1" 2>&1 || true
grep -q "paste          ok via ssh" "$T/t1" && ok "ssh + tmux" || { bad "ssh + tmux"; cat "$T/t1"; }

# 4. tmux pane from an old ssh session, after reconnecting. Its env is stale and
#    the forward it was born with is dead.
sshr "tmux -L rpe2e respawn-pane -k -t s '$BIN doctor > $T/tmux2.out 2>&1; sleep 30'; sleep 1.5; cat $T/tmux2.out" > "$T/t2" 2>&1 || true
grep -q "paste          ok via ssh" "$T/t2" && ok "old tmux pane after reconnect" || { bad "stale tmux"; cat "$T/t2"; }

# 5. dead forwards from earlier sessions are skipped and cleaned up.
LEFT=$(ls /tmp/richpaste-"$(id -un)"-*.sock 2>/dev/null | wc -l)
OUT=$(sshr "$BIN doctor" 2>&1 || true)
echo "$OUT" | grep -q "paste          ok via ssh" && ok "works with $LEFT stale socket(s) on disk" || { bad "stale sockets"; echo "$OUT"; }

# 6. two hops: laptop -> host A -> host B. A's agent relays to the laptop.
#    Simulated with a second agent on "host A" that has no display.
HOP=$T/hopA; mkdir -p "$HOP/.agentty/paste"
OUT=$(sshr "RICHPASTE_NO_DISPLAY=1 HOME=$HOP sh -c '$BIN agent 2>/dev/null & sleep 0.3; RICHPASTE_SOCK=$HOP/.agentty/paste/agent.sock $BIN doctor; kill \$!'" 2>&1 || true)
echo "$OUT" | grep -q "paste          ok" && [ "$(echo "$OUT" | got_sha)" = "$(echo "$WANT" | cut -c1-16)" ] \
    && ok "two hops (relay agent)" || { bad "two hops"; echo "$OUT"; }

# 7. hook restarts a dead agent. No display for it, so it never reads your real clipboard.
kill "$AGENT"; AGENT=
sleep 0.2

# 6b. clipboard holds an image *as text* (what the PowerShell/web/Mac snippets leave).
#     Plain Ctrl+V through the forward must still deliver the image.
"$BIN" encode "$T/shot.png" > "$T/armored.txt"
"$BIN" serve-file "$LAPTOP_HOME/.agentty/paste/agent.sock" "$T/armored.txt" text 2>/dev/null &
AGENT=$!
sleep 0.3
sshr "$BIN paste -o /tmp/rp-e2e-out2" > "$T/paths2" 2>&1 || true
P=$(head -1 "$T/paths2")
[ -f "$P" ] && [ "$(sha256sum < "$P" | cut -d' ' -f1)" = "$WANT" ] \
    && ok "clipboard has the image as text: Ctrl+V over ssh still gives the image" || { bad "armored over ssh"; cat "$T/paths2"; }
rm -rf /tmp/rp-e2e-out2
kill "$AGENT"; AGENT=
sleep 0.2
RICHPASTE_NO_DISPLAY=1 sshr true >/dev/null 2>&1 || true
HOME=$LAPTOP_HOME RICHPASTE_NO_DISPLAY=1 "$BIN" doctor 2>&1 | grep -q "(answering)" \
    && ok "ssh hook restarts a dead agent" || bad "agent auto-start"
pkill -f "$BIN agent" 2>/dev/null || true

# 8. ssh without the hook: clear message, no hang.
sed '/# richpaste:/,/# richpaste end/d' "$T/ssh_config" > "$T/ssh_config_plain"
START=$(date +%s)
OUT=$(HOME=$LAPTOP_HOME ssh -F "$T/ssh_config_plain" rptest "RICHPASTE_NO_DISPLAY=1 $BIN paste" 2>&1 || true)
END=$(date +%s)
echo "$OUT" | grep -q "rp_driver setup" && [ $((END-START)) -lt 10 ] \
    && ok "no forward: clear error in $((END-START))s" || { bad "no forward"; echo "$OUT"; }

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
