#include "rp/armor.hpp"

#include "rp/sha256.hpp"

#include <algorithm>
#include <charconv>

namespace rp::armor {

namespace {

auto is_b64(char c) -> bool {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
           c == '=';
}
auto is_space(char c) -> bool { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

// Reads one ':'-terminated header field, dropping any whitespace a terminal wrapped into it.
auto field(std::string_view s, std::size_t& i) -> std::optional<std::string> {
    std::string f;
    for (std::size_t n = 0; i < s.size() && n < 400; ++i, ++n) {
        if (s[i] == ':') {
            ++i;
            return f;
        }
        if (!is_space(s[i])) f += s[i];
    }
    return std::nullopt;
}

struct Block {
    std::optional<Item> item;
    std::string error;
    std::size_t end;  // index just past the block
};

// Parses one block that starts at s[i] (at the magic).
auto parse_block(std::string_view s, std::size_t i) -> Block {
    auto skip_token = [&](std::size_t from) {
        while (from < s.size() && !is_space(s[from])) ++from;
        return from;
    };
    std::size_t p = i + magic.size();
    auto mime_s = field(s, p);
    auto name_s = field(s, p);
    auto size_s = field(s, p);
    auto sha_s = field(s, p);
    if (!mime_s || !name_s || !size_s || !sha_s) return {std::nullopt, "pasted block has a broken header", skip_token(i)};

    std::uint64_t size = 0;
    auto [ptr, ec] = std::from_chars(size_s->data(), size_s->data() + size_s->size(), size);
    if (ec != std::errc{} || ptr != size_s->data() + size_s->size() || size > (1ull << 30))
        return {std::nullopt, "pasted block has a bad size", skip_token(i)};

    // Collect exactly the base64 this size needs, skipping whitespace terminals may add.
    std::size_t need = (size + 2) / 3 * 4;
    std::string b64;
    b64.reserve(need);
    while (p < s.size() && b64.size() < need) {
        char c = s[p];
        if (is_b64(c)) b64 += c;
        else if (!is_space(c)) break;
        ++p;
    }
    if (b64.size() < need)
        return {std::nullopt, std::format("pasted block is cut short ({} of {} bytes)", b64.size() / 4 * 3, size), p};

    auto data = base64_decode(b64);
    if (!data || data->size() != size) return {std::nullopt, "pasted block is corrupted (bad base64)", p};
    if (!sha_s->empty() && hex(sha256(*data)) != *sha_s) return {std::nullopt, "pasted block is corrupted (sha256 mismatch)", p};

    std::string name;
    if (!name_s->empty()) {
        auto n = base64_decode(*name_s);
        if (!n) return {std::nullopt, "pasted block has a bad name", p};
        name = as_string(*n);
    }
    auto mime = Mime::parse(*mime_s);
    if (!mime) return {std::nullopt, "pasted block has a bad mime type", p};

    if (name.empty() && mime->is_image()) return {Item{Image{std::move(*mime), std::move(*data)}}, {}, p};
    if (name.empty() && mime->essence() == "text/plain") {
        auto t = Text::parse(as_string(*data));
        if (!t) return {std::nullopt, "pasted text block is not UTF-8", p};
        return {Item{std::move(*t)}, {}, p};
    }
    auto fname = FileName::sanitize(name.empty() ? std::format("file.{}", ext_for_mime(mime->str())) : name);
    // Senders that don't know the type say octet-stream; the name usually does.
    if (mime->essence() == "application/octet-stream") mime = mime_for_name(fname.str());
    return {Item{File{std::move(fname), std::move(*mime), std::move(*data)}}, {}, p};
}

}  // namespace

auto encode(const Item& item) -> std::string {
    auto data = payload(item);
    auto name = name_of(item);
    return std::format("{}{}:{}:{}:{}:{}", magic, mime_of(item), name.empty() ? "" : base64_encode(as_view(name)),
                       data.size(), hex(sha256(data)), base64_encode(data));
}

auto contains(std::string_view text) -> bool { return text.contains(magic); }

auto progress(std::string_view s) -> Progress {
    Progress out;
    for (auto at = s.find(magic); at != std::string_view::npos; at = s.find(magic, at + magic.size())) {
        out.state = Progress::State::complete;
        std::size_t p = at + magic.size();
        std::optional<std::string> f[4];
        for (auto& x : f) {
            x = field(s, p);
            if (!x) break;
        }
        if (!f[3]) {
            // No ':' yet: still arriving if we ran off the end, else broken.
            if (p >= s.size()) return {Progress::State::partial, 0, 0};
            continue;
        }
        std::uint64_t size = 0;
        auto [ptr, ec] = std::from_chars(f[2]->data(), f[2]->data() + f[2]->size(), size);
        if (ec != std::errc{} || ptr != f[2]->data() + f[2]->size() || size > (1ull << 30)) continue;
        const std::size_t need = (size + 2) / 3 * 4;
        std::size_t got = 0;
        while (p < s.size() && got < need) {
            if (is_b64(s[p])) ++got;
            else if (!is_space(s[p])) break;
            ++p;
        }
        // Short, and the text just ends: more is coming. Short and something
        // else follows: it was cut, split() will say so.
        if (got < need && p >= s.size()) return {Progress::State::partial, std::min<std::uint64_t>(got / 4 * 3, size), size};
        at = p - magic.size();
    }
    return out;
}

auto split(std::string_view s) -> Split {
    Split out;
    std::size_t i = 0;
    while (i < s.size()) {
        auto at = s.find(magic, i);
        if (at == std::string_view::npos) {
            out.parts.emplace_back(std::string(s.substr(i)));
            break;
        }
        if (at > i) out.parts.emplace_back(std::string(s.substr(i, at - i)));
        auto b = parse_block(s, at);
        if (b.item) out.parts.emplace_back(std::move(*b.item));
        else out.errors.push_back(std::move(b.error));
        i = b.end;
    }
    return out;
}

auto expand(std::vector<Item> items, std::vector<std::string>* errors) -> std::vector<Item> {
    std::vector<Item> out;
    for (auto& item : items) {
        auto* t = std::get_if<Text>(&item);
        if (!t || !contains(t->str())) {
            out.push_back(std::move(item));
            continue;
        }
        auto sp = split(t->str());
        for (auto& part : sp.parts) {
            if (auto* s = std::get_if<std::string>(&part)) {
                // Keep surrounding text, but drop the whitespace left between blocks.
                bool blank = std::ranges::all_of(*s, [](char c) { return is_space(c); });
                if (!blank) out.push_back(Text::repair(*s));
            } else {
                out.push_back(std::move(std::get<Item>(part)));
            }
        }
        if (errors) errors->insert(errors->end(), sp.errors.begin(), sp.errors.end());
    }
    return out;
}

auto mac_snippet() -> std::string {
    // Only stock macOS tools. Copied file -> that file; otherwise the image (as PNG).
    return R"SH(d=$(mktemp -d); f=$d/c; if p=$(osascript -e 'POSIX path of (the clipboard as «class furl»)' 2>/dev/null) && [ -f "$p" ]; then cp "$p" "$f"; n=$(printf %s "$(basename "$p")" | base64 | tr -d '\n'); m=application/octet-stream; else osascript -e "set f to open for access POSIX file \"$f\" with write permission" -e 'write (the clipboard as «class PNGf») to f' -e 'close access f' >/dev/null 2>&1; n=; m=image/png; fi; if [ -s "$f" ]; then printf 'richpaste:1:%s:%s:%s:%s:%s' "$m" "$n" "$(wc -c < "$f" | tr -d ' ')" "$(shasum -a 256 "$f" | cut -c1-64)" "$(base64 < "$f" | tr -d '\n')" | pbcopy; echo 'ready: press Cmd+V in the ssh window'; else echo 'no image or file on the clipboard'; fi; rm -rf "$d")SH";
}

auto mac_hotkey_script() -> std::string {
    // For a Shortcuts "Run Shell Script" action bound to a key. Built-in tools only.
    // Encodes the clipboard, pastes it into the front window, then puts the original back.
    return R"SH(d=$(mktemp -d); f=$d/c; r=
if p=$(osascript -e 'POSIX path of (the clipboard as «class furl»)' 2>/dev/null) && [ -f "$p" ]; then
  cp "$p" "$f"; n=$(printf %s "$(basename "$p")" | base64 | tr -d '\n'); m=application/octet-stream; r=file
else
  osascript -e "set f to open for access POSIX file \"$f\" with write permission" -e 'write (the clipboard as «class PNGf») to f' -e 'close access f' >/dev/null 2>&1
  n=; m=image/png; r=png
fi
if [ ! -s "$f" ]; then
  # Plain text: just paste it as usual.
  sleep 0.3; osascript -e 'tell application "System Events" to keystroke "v" using command down'; rm -rf "$d"; exit 0
fi
printf 'richpaste:1:%s:%s:%s:%s:%s' "$m" "$n" "$(wc -c < "$f" | tr -d ' ')" "$(shasum -a 256 "$f" | cut -c1-64)" "$(base64 < "$f" | tr -d '\n')" | pbcopy
sleep 0.3
osascript -e 'tell application "System Events" to keystroke "v" using command down'
sleep 1
if [ "$r" = png ]; then osascript -e "set the clipboard to (read POSIX file \"$f\" as «class PNGf»)"; else osascript -e "set the clipboard to (POSIX file \"$p\")"; fi
rm -rf "$d")SH";
}

auto windows_snippet() -> std::string {
    // Stock Windows PowerShell 5.1. Copied file -> that file; otherwise the image (as PNG).
    return R"PS(Add-Type -A System.Windows.Forms,System.Drawing; $c=[Windows.Forms.Clipboard]; if($c::ContainsFileDropList()){$p=$c::GetFileDropList()[0]; $b=[IO.File]::ReadAllBytes($p); $n=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([IO.Path]::GetFileName($p))); $m='application/octet-stream'} elseif($c::ContainsImage()){$s=New-Object IO.MemoryStream; $c::GetImage().Save($s,[Drawing.Imaging.ImageFormat]::Png); $b=$s.ToArray(); $n=''; $m='image/png'} else {'no image or file on the clipboard'; return}; $h=-join([Security.Cryptography.SHA256]::Create().ComputeHash($b)|%{$_.ToString('x2')}); Set-Clipboard -Value "richpaste:1:${m}:${n}:$($b.Length):${h}:$([Convert]::ToBase64String($b))"; 'ready: paste in the ssh window')PS";
}

namespace {

// A Content-Security-Policy source for exactly this text.
auto csp_hash(std::string_view s) -> std::string {
    auto d = sha256(as_view(s));
    return "'sha256-" + base64_encode(ByteView(d)) + "'";
}

constexpr std::string_view page_style = R"CSS(
:root{color-scheme:light dark;--bg:#f7f7f9;--fg:#16161b;--mut:#62626e;--line:#e2e2ea;--card:#fff;--acc:#6d4aff;--tint:#efeaff;--ok:#15803d;--warn:#b45309;--bad:#dc2626}
@media (prefers-color-scheme:dark){:root{--bg:#0e0e12;--fg:#ececf1;--mut:#9d9dab;--line:#2b2b35;--card:#16161c;--acc:#a08cff;--tint:#211b3a;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171}}
*{box-sizing:border-box}[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:44rem;margin:0 auto;padding:2.25rem 1.25rem 3rem}
a{color:var(--acc)}
.brand{font:600 .9rem ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--acc)}
h1{font-size:1.85rem;line-height:1.2;margin:.35rem 0 .5rem;letter-spacing:-.01em}
.lead{color:var(--mut);margin:0 0 1.4rem}
#z{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:.4rem;min-height:15rem;padding:2rem;border:2px dashed var(--line);border-radius:16px;background:var(--card);text-align:center;caret-color:transparent;outline:none;transition:border-color .15s,background .15s}
#z:focus{border-color:var(--acc)}#z.over{border-color:var(--acc);background:var(--tint)}#z.busy{border-style:solid;border-color:var(--acc)}
body.has #z{min-height:7rem;padding:1rem}body.has #z svg{width:28px;height:28px}body.has #z .sub{display:none}
#z svg{width:44px;height:44px;color:var(--acc)}#z .big{font-size:1.25rem;font-weight:600}#z .sub{color:var(--mut)}
kbd{font:600 .85em ui-monospace,Menlo,monospace;padding:.1em .45em;border:1px solid var(--line);border-bottom-width:2px;border-radius:6px;background:var(--bg);white-space:nowrap}
.pick{display:inline-block;margin-top:.6rem;color:var(--acc);font-weight:600;cursor:pointer;text-decoration:underline;text-underline-offset:3px}
#s{margin:.9rem 0 0;font-weight:600}#s:empty{display:none}#s.done{color:var(--ok)}#s.error{color:var(--bad)}#s.hint{color:var(--mut);font-weight:500}
#r{margin-top:.75rem;padding:1rem;border:1px solid var(--line);border-radius:14px;background:var(--card)}
#list{list-style:none;margin:0;padding:0;display:grid;gap:.75rem}
#list li{display:flex;gap:.9rem;align-items:center;min-width:0}
#list img,.fileic{flex:none;width:96px;height:64px;border-radius:8px;border:1px solid var(--line);background:var(--bg)}
#list img{object-fit:cover}.fileic{display:grid;place-items:center;font-weight:700;color:var(--mut);font-size:.8rem}
#list b{display:block;overflow-wrap:anywhere}#list small{display:block;color:var(--mut)}
.row{display:flex;gap:1rem;align-items:center;flex-wrap:wrap;margin-top:1rem}
button{font:inherit;font-weight:600;padding:.5rem 1rem;border-radius:10px;border:0;background:var(--acc);color:#fff;cursor:pointer}
button:focus-visible,.pick:focus-visible{outline:3px solid var(--tint);outline-offset:2px}
summary{cursor:pointer;color:var(--mut)}
textarea{width:100%;height:6rem;margin-top:.5rem;font:12px ui-monospace,Menlo,monospace;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg)}
#w{margin:.75rem 0 0;color:var(--warn)}
.steps{display:grid;grid-template-columns:repeat(3,1fr);gap:.75rem;padding:0;margin:1.5rem 0 0;list-style:none;counter-reset:s}
.steps li{counter-increment:s;padding:.9rem;border:1px solid var(--line);border-radius:12px;background:var(--card);font-size:.93rem}
.steps li::before{content:counter(s);display:grid;place-items:center;width:1.6rem;height:1.6rem;margin-bottom:.4rem;border-radius:50%;background:var(--tint);color:var(--acc);font-weight:700}
@media (max-width:560px){.steps{grid-template-columns:1fr}h1{font-size:1.5rem}}
.safe{margin-top:2rem;padding:1.2rem 1.3rem .6rem;border-radius:14px;background:var(--tint)}
.safe h2{margin:0 0 .25rem;font-size:1.15rem;display:flex;gap:.5rem;align-items:center}.safe h2 svg{width:22px;height:22px;color:var(--acc)}
.safe ul{margin:.5rem 0;padding-left:1.1rem}.safe li{margin:.45rem 0}
code{font:.88em ui-monospace,Menlo,monospace;padding:.05em .3em;border-radius:5px;background:var(--line)}
footer{margin-top:1.75rem;color:var(--mut);font-size:.92rem}
)CSS";

constexpr std::string_view page_body = R"HTML(<main>
<div class="brand">agentty paste</div>
<h1>Paste a screenshot into agentty, even over ssh</h1>
<p class="lead">Terminals only carry text. This page turns an image or file into one line of text,
and agentty turns that line back into the image. It all happens in your browser.</p>
<div id="z" contenteditable="true" spellcheck="false" role="textbox" aria-label="Paste area" autofocus>
<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="5" y="4" width="14" height="17" rx="2"/><path d="M9 4V3h6v1M12 9v7m-3-3 3 3 3-3"/></svg>
<div class="big">Press <kbd id="k1">Ctrl+V</kbd> to paste</div>
<div class="sub">a screenshot, an image or a file, or drop one here</div>
</div>
<label class="pick" for="f" tabindex="0">or choose a file</label><input id="f" type="file" multiple hidden>
<div id="s" role="status" aria-live="polite"></div>
<section id="r" hidden>
<ul id="list"></ul>
<p id="w" hidden></p>
<div class="row"><button id="again" type="button">Copy again</button>
<details id="d"><summary>Show the text</summary><textarea id="t" readonly aria-label="The line to paste into agentty"></textarea></details></div>
</section>
<ol class="steps">
<li>Take a screenshot with <kbd id="k3">PrtSc</kbd>, or copy a file.</li>
<li>Paste it here. The line of text is copied for you.</li>
<li>Press <kbd id="k2">Ctrl+Shift+V</kbd> in agentty. It shows up as the image.</li>
</ol>
<section class="safe">
<h2><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3 5 6v5c0 4.5 3 8.5 7 10 4-1.5 7-5.5 7-10V6z"/><path d="m9 12 2 2 4-4"/></svg>Private by design</h2>
<ul>
<li><b>Nothing leaves this page.</b> Your browser blocks every network request here: the page's
Content-Security-Policy is <code>connect-src 'none'</code> and lets only the one script in this file run.
Open your browser's network tab and paste to see it for yourself.</li>
<li><b>No accounts, cookies, storage or trackers.</b> No libraries either. It's one small file you can
read with View Source.</li>
<li><b>Screenshots lose their hidden data.</b> A pasted screenshot or copied image is redrawn before
it's encoded, so things like a photo's GPS location don't come along. A file (dropped, chosen, or
copied in your file manager) is sent byte for byte, as it is.</li>
<li><b>Checked on arrival.</b> The line carries a SHA-256 of the data. agentty checks it and refuses
anything that changed on the way.</li>
<li><b>Only ever data.</b> agentty turns the line back into an image or file. It never runs it.</li>
<li><b>The same page, offline.</b> <code>agentty paste page &gt; paste.html</code> writes this exact page,
and agentty's tests check the website's copy matches it byte for byte.</li>
</ul>
</section>
<footer>Only needed when your laptop can't hand agentty its clipboard, like Terminal.app over ssh.
Run <code>agentty paste setup</code> on your laptop once and Ctrl+V in agentty pastes images by itself,
with no size limit. More in <a href="https://agentty.org/docs/clipboard" rel="noopener">Clipboard &amp; Images</a>.</footer>
</main>
)HTML";

constexpr std::string_view page_script = R"JS(
'use strict';
// Everything happens on this page. The CSP above stops it sending anything anywhere.

// The line format agentty reads: richpaste:1:<mime>:<name, base64>:<size>:<sha256>:<data, base64>
function b64(bytes){let s='';for(let i=0;i<bytes.length;i+=32768)s+=String.fromCharCode.apply(null,bytes.subarray(i,i+32768));return btoa(s)}
function utf8b64(str){return b64(new TextEncoder().encode(str))}
async function sha(bytes){try{const d=await crypto.subtle.digest('SHA-256',bytes);return [...new Uint8Array(d)].map(x=>x.toString(16).padStart(2,'0')).join('')}catch(e){return ''}}
async function armor(mime,name,bytes){return 'richpaste:1:'+(mime||'application/octet-stream')+':'+(name?utf8b64(name):'')+':'+bytes.length+':'+(await sha(bytes))+':'+b64(bytes)}

// A screenshot should be small: every byte crosses ssh and the terminal, and
// tmux drops a clipboard reply over ~2.4 MB outright. Scale to at most
// MAX_SIDE, keep the smallest of PNG/WebP/JPEG, and step size and quality
// down until it fits BUDGET. Redrawing also drops metadata (EXIF, location).
const MAX_SIDE=1600,BUDGET=1100000,BIG_LINE=1600000;
async function shrink(file){let bmp;try{bmp=await createImageBitmap(file)}catch(e){return null}
  const blob=(c,t,q)=>new Promise(r=>c.toBlob(r,t,q));
  let side=Math.min(MAX_SIDE,Math.max(bmp.width,bmp.height)),best=null;
  for(let round=0;round<8;round++){
    const k=side/Math.max(bmp.width,bmp.height);
    const w=Math.max(1,Math.round(bmp.width*k)),h=Math.max(1,Math.round(bmp.height*k));
    const c=document.createElement('canvas');c.width=w;c.height=h;
    const g=c.getContext('2d');g.imageSmoothingQuality='high';g.drawImage(bmp,0,0,w,h);
    const q=round<2?0.9:round<4?0.8:0.7;best=null;
    for(const [t,qq] of [['image/png'],['image/webp',q],['image/jpeg',q]]){const b=await blob(c,t,qq);if(b&&b.type===t&&(!best||b.size<best.blob.size))best={blob:b,w,h}}
    if(best&&best.blob.size<=BUDGET)break;
    side=Math.round(side*0.75)}
  if(best){best.srcW=bmp.width;best.srcH=bmp.height}
  return best}

// One file or image -> its line, plus what to show about it.
async function take(file){
  const named=file.name&&!/^image\.(png|jpe?g|gif|webp|tiff?)$/i.test(file.name);
  if(!named&&/^image\/(png|jpeg|webp|bmp|tiff)$/i.test(file.type)){
    const s=await shrink(file);
    if(s){const bytes=new Uint8Array(await s.blob.arrayBuffer());
      return {line:await armor(s.blob.type,'',bytes),image:true,type:s.blob.type,w:s.w,h:s.h,srcW:s.srcW,srcH:s.srcH,from:file.size,blob:s.blob}}}
  const bytes=new Uint8Array(await file.arrayBuffer());
  return {line:await armor(file.type,named?file.name:'',bytes),image:/^image\//.test(file.type),type:file.type,name:named?file.name:'',from:file.size,blob:file}}
async function encodeAll(files){const items=[];for(const f of files)items.push(await take(f));return {items,text:items.map(i=>i.line).join(' ')}}

// ---- the page ----
const $=id=>document.getElementById(id);
const Z=$('z'),S=$('s'),R=$('r'),L=$('list'),W=$('w'),T=$('t'),D=$('d'),F=$('f'),A=$('again');
const ua=(navigator.platform||'')+' '+(navigator.userAgent||'');
const MAC=/Mac|iPhone|iPad/.test(ua),WIN=/Win/.test(ua);
const KEY_HERE=MAC?'\u2318V':'Ctrl+V',KEY_TERM=MAC?'\u2318V':'Ctrl+Shift+V';
$('k1').textContent=KEY_HERE;$('k2').textContent=KEY_TERM;
$('k3').textContent=MAC?'\u2303\u21e7\u23184':WIN?'Win+Shift+S':'PrtSc';
const ZH=Z.innerHTML;
let urls=[];

function human(n){return n<1024?n+' B':n<1048576?Math.round(n/1024)+' KB':(n/1048576).toFixed(1)+' MB'}
function kind(t){return {'image/png':'PNG','image/webp':'WebP','image/jpeg':'JPEG','image/gif':'GIF'}[t]||(t?t.split('/').pop().toUpperCase():'file')}
function set(cls,msg){S.className=cls;S.textContent=msg;Z.classList.toggle('busy',cls==='busy')}
function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e}

function show(r){
  urls.forEach(u=>URL.revokeObjectURL(u));urls=[];
  L.replaceChildren(...r.items.map(it=>{
    const li=el('li');
    if(it.image){const img=el('img');img.alt='';const u=URL.createObjectURL(it.blob);urls.push(u);img.src=u;li.append(img)}
    else li.append(el('span','fileic',kind(it.type)));
    const title=it.image?(it.w?it.w+' \u00d7 '+it.h+' \u00b7 ':'')+kind(it.type):(it.name||'file')+' \u00b7 '+kind(it.type);
    let meta=human(it.line.length)+' of text';
    if(it.srcW&&(it.srcW!==it.w||it.from>it.blob.size))meta+=', from a '+human(it.from)+' image at '+it.srcW+' \u00d7 '+it.srcH;
    const d=el('div');d.append(el('b',null,title),el('small',null,meta));li.append(d);return li}));
  T.value=r.text;R.hidden=false;document.body.classList.add('has');R.scrollIntoView({block:'nearest',behavior:'smooth'});
  W.hidden=r.text.length<=BIG_LINE;
  W.textContent='This line is '+human(r.text.length)+'. Inside tmux, lines over about 2 MB may not arrive. '+
    'For big files, run agentty paste setup on this computer: it has no size limit.'}

async function copyText(text){
  try{await navigator.clipboard.writeText(text);return true}catch(e){}
  try{D.open=true;T.focus();T.select();if(document.execCommand('copy'))return true}catch(e){}
  return false}
function copied(){set('done','\u2713 Copied. Now press '+KEY_TERM+' in agentty.')}

function pasteNow(files){
  if(!files.length){set('hint','Nothing to use in that paste. Copy a screenshot or a file first.');return}
  set('busy',files.length>1?'Encoding '+files.length+' files\u2026':/^image\//.test(files[0].type)?'Encoding the image\u2026':'Encoding the file\u2026');
  const p=encodeAll(files);
  // Safari only allows a clipboard write inside the paste itself. Handing it a
  // ClipboardItem whose content is a promise, right now, keeps that while we encode.
  let wrote=null;
  if(typeof ClipboardItem!=='undefined'&&navigator.clipboard&&navigator.clipboard.write){
    try{wrote=navigator.clipboard.write([new ClipboardItem({'text/plain':p.then(r=>new Blob([r.text],{type:'text/plain'}))})]).then(()=>true,()=>false)}catch(e){wrote=null}}
  p.then(async r=>{show(r);let ok=wrote?await wrote:false;if(!ok)ok=await copyText(r.text);
      if(ok)copied();else{D.open=true;set('hint','Ready. Select the text below, copy it, then paste into agentty.')}},
    e=>set('error','Couldn\u2019t read that: '+((e&&e.message)||e)))}

function filesOf(dt){if(!dt)return [];const fs=[...(dt.files||[])];if(fs.length)return fs;
  return [...(dt.items||[])].filter(i=>i.kind==='file').map(i=>i.getAsFile()).filter(Boolean)}

document.addEventListener('paste',e=>{e.preventDefault();const files=filesOf(e.clipboardData);if(files.length)return pasteNow(files);
  const t=e.clipboardData?e.clipboardData.getData('text/plain'):'';
  if(t.startsWith('richpaste:1:'))set('hint','That\u2019s already a line for agentty. Paste it there.');
  else if(t)set('hint','That\u2019s plain text, and agentty pastes text directly. This page is for images and files.');
  else set('hint','Nothing to use in that paste. Copy a screenshot or a file first.')});
let depth=0;
document.addEventListener('dragenter',e=>{e.preventDefault();depth++;Z.classList.add('over')});
document.addEventListener('dragleave',()=>{if(--depth<=0){depth=0;Z.classList.remove('over')}});
document.addEventListener('dragover',e=>e.preventDefault());
document.addEventListener('drop',e=>{e.preventDefault();depth=0;Z.classList.remove('over');pasteNow(filesOf(e.dataTransfer))});
F.addEventListener('change',()=>{pasteNow([...F.files]);F.value=''});
A.addEventListener('click',async()=>{if(await copyText(T.value))copied();else set('hint','Select the text below and copy it.')});
// The box is editable only so Safari sends it pastes. Keep its content ours.
Z.addEventListener('beforeinput',e=>e.preventDefault());
Z.addEventListener('input',()=>{if(Z.innerHTML!==ZH)Z.innerHTML=ZH});
Z.focus();
if(typeof module!=='undefined')module.exports={armor};
)JS";

}  // namespace

auto html_page() -> std::string {
    // Only this page's own style and script may run, and nothing may be fetched.
    const std::string csp = "default-src 'none'; connect-src 'none'; script-src " + csp_hash(page_script) +
                            "; style-src " + csp_hash(page_style) +
                            "; img-src blob:; base-uri 'none'; form-action 'none'";
    std::string out;
    out += "<!doctype html>\n<html lang=\"en\">\n<meta charset=\"utf-8\">\n";
    out += "<meta http-equiv=\"Content-Security-Policy\" content=\"" + csp + "\">\n";
    out += "<meta name=\"referrer\" content=\"no-referrer\">\n";
    out += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
    out += "<title>agentty paste: a screenshot or file as one line of text</title>\n";
    out += "<meta name=\"description\" content=\"Turn a screenshot or file into one line of text you can paste into "
           "agentty over ssh. Runs entirely in your browser; the page can't upload anything.\">\n";
    out += "<style>";
    out += page_style;
    out += "</style>\n";
    out += page_body;
    out += "<script>";
    out += page_script;
    out += "</script>\n";
    return out;
}

}  // namespace rp::armor
