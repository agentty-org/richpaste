#!/bin/sh
# Text armor: an image/file survives a plain-text terminal paste, over ssh, inside tmux.
# Also checks the browser page's encoder (with node) produces what the decoder accepts.
set -u
unset AGENTTY_HOME
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/rp_driver
T=$(mktemp -d /tmp/rp-armor.XXXXXX)
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL $1"; }
cleanup() { tmux -L rparm kill-server 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
unset TMUX TMUX_PANE

head -c 3000000 /dev/urandom > "$T/shot.png"
WANT=$(sha256sum < "$T/shot.png" | cut -c1-16)

# 1. roundtrip
"$BIN" encode "$T/shot.png" > "$T/a.txt"
P=$("$BIN" decode "$T/o1" < "$T/a.txt" | head -1)
cmp -s "$P" "$T/shot.png" && ok "3 MB image roundtrips through text" || bad "roundtrip"

# 2. terminals that wrap or change line endings
fold -w 76 "$T/a.txt" | sed 's/$/\r/' > "$T/wrapped.txt"
P=$("$BIN" decode "$T/o2" < "$T/wrapped.txt" | head -1)
cmp -s "$P" "$T/shot.png" && ok "survives wrapping and CRLF" || bad "wrapping"

# 3. cut short / corrupted: rejected, not saved
head -c 100000 "$T/a.txt" > "$T/short.txt"
"$BIN" decode "$T/o3" < "$T/short.txt" 2> "$T/e3" >/dev/null && bad "truncated accepted" || \
    { grep -q "cut short" "$T/e3" && ok "truncated paste rejected"  || { bad "truncated msg"; cat "$T/e3"; }; }
python3 -c "
import sys;b=bytearray(open(sys.argv[1],'rb').read());i=len(b)//2;b[i]=ord('A') if b[i]!=ord('A') else ord('B');open(sys.argv[2],'wb').write(b)" "$T/a.txt" "$T/bad.txt"
"$BIN" decode "$T/o4" < "$T/bad.txt" 2> "$T/e4" >/dev/null && bad "corrupt accepted" || \
    { grep -q "sha256" "$T/e4" && ok "corrupted paste rejected (sha256)" || { bad "corrupt msg"; cat "$T/e4"; }; }

# 4. real bracketed paste inside tmux over a pty (what Cmd+V does), read back by decode
tmux -L rparm -f /dev/null new-session -d -x 120 -y 10 "sh -c 'stty raw -echo; head -c $(wc -c < "$T/a.txt") > $T/pasted.txt'"
sleep 0.6
tmux -L rparm load-buffer "$T/a.txt"
tmux -L rparm paste-buffer -p      # -p = bracketed, like a real terminal paste
sleep 2
tmux -L rparm kill-server 2>/dev/null
P=$("$BIN" decode "$T/o4b" < "$T/pasted.txt" 2>/dev/null | head -1)
[ -n "$P" ] && [ "$(sha256sum < "$P" | cut -c1-16)" = "$WANT" ] \
    && ok "tmux paste: 3 MB pasted as text becomes the image, sha matches" || bad "tmux paste"

# 5. a file keeps its name
printf 'quarterly numbers\n' > "$T/report.csv"
"$BIN" encode "$T/report.csv" | "$BIN" decode "$T/o5" > "$T/p5"
grep -q 'report.csv$' "$T/p5" && cmp -s "$(cat "$T/p5")" "$T/report.csv" && ok "file keeps its name" || { bad "file name"; cat "$T/p5"; }

# 6. the browser page's encoder (same JS the page runs) -> our decoder
if command -v node >/dev/null; then
    "$BIN" page > "$T/rp.html"
    node -e "
const fs=require('fs');const html=fs.readFileSync(process.argv[1],'utf8');
const js=html.split('<script>')[1].split('</script>')[0];
const pre='const mk=()=>({focus(){},addEventListener(){},append(){},replaceChildren(){},classList:{add(){},remove(){},toggle(){}},textContent:String(),innerHTML:String(),hidden:true});const stubEl=mk();const document={getElementById:()=>stubEl,createElement:mk,addEventListener(){}};const navigator={};';
const module={exports:{}};new Function('module','crypto','btoa',pre+js)(module,globalThis.crypto,globalThis.btoa);
module.exports.armor('image/png','',new Uint8Array(fs.readFileSync(process.argv[2]))).then(s=>fs.writeFileSync(process.argv[3],s));
" "$T/rp.html" "$T/shot.png" "$T/js.txt"
    P=$("$BIN" decode "$T/o6" < "$T/js.txt" | head -1)
    cmp -s "$P" "$T/shot.png" && ok "browser page encoder -> decoder, byte-identical" || bad "browser page"
    # The page must not be able to reach the network, and its CSP must allow
    # exactly its own script and style (a stale hash would break the page).
    node -e "
const fs=require('fs'),c=require('crypto');const h=fs.readFileSync(process.argv[1],'utf8');
const csp=(h.match(/Content-Security-Policy\" content=\"([^\"]+)/)||[])[1]||String();
const hash=s=>'sha256-'+c.createHash('sha256').update(s).digest('base64');
const js=h.split('<script>')[1].split('</script>')[0],css=h.split('<style>')[1].split('</style>')[0];
const ok=/default-src 'none'/.test(csp)&&/connect-src 'none'/.test(csp)&&csp.includes(hash(js))&&csp.includes(hash(css))
  &&(h.match(/<script/g)||[]).length===1&&!/ src=|href=\"(?!https:\/\/agentty\.org)/.test(h);
process.exit(ok?0:1)" "$T/rp.html" && ok "page CSP: no network, only its own script and style" || bad "page CSP"
fi

# 7. the macOS snippet only uses tools that ship with macOS
S=$("$BIN" mac-snippet)
for tool in curl brew python3 perl ruby jq; do
    echo "$S" | grep -qw "$tool" && { bad "mac snippet uses $tool"; continue; }
done
echo "$S" | sh -n 2>/dev/null && ok "mac snippet is valid sh, only osascript/base64/shasum/pbcopy" || bad "mac snippet syntax"
H=$("$BIN" mac-hotkey)
for tool in curl brew python3 perl ruby jq; do
    echo "$H" | grep -qw "$tool" && bad "hotkey script uses $tool"
done
echo "$H" | sh -n 2>/dev/null && ok "mac hotkey script is valid sh, built-in tools only" || bad "hotkey syntax"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
