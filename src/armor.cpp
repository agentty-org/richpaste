#include "rp/armor.hpp"

#include "rp/sha256.hpp"

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

auto mac_snippet() -> std::string {
    // Only stock macOS tools. Copied file -> that file; otherwise the image (as PNG).
    return R"SH(d=$(mktemp -d); f=$d/c; if p=$(osascript -e 'POSIX path of (the clipboard as «class furl»)' 2>/dev/null) && [ -f "$p" ]; then cp "$p" "$f"; n=$(printf %s "$(basename "$p")" | base64 | tr -d '\n'); m=application/octet-stream; else osascript -e "set f to open for access POSIX file \"$f\" with write permission" -e 'write (the clipboard as «class PNGf») to f' -e 'close access f' >/dev/null 2>&1; n=; m=image/png; fi; if [ -s "$f" ]; then printf 'richpaste:1:%s:%s:%s:%s:%s' "$m" "$n" "$(wc -c < "$f" | tr -d ' ')" "$(shasum -a 256 "$f" | cut -c1-64)" "$(base64 < "$f" | tr -d '\n')" | pbcopy; echo 'ready: press Cmd+V in the ssh window'; else echo 'no image or file on the clipboard'; fi; rm -rf "$d")SH";
}

auto windows_snippet() -> std::string {
    // Stock Windows PowerShell 5.1. Copied file -> that file; otherwise the image (as PNG).
    return R"PS(Add-Type -A System.Windows.Forms,System.Drawing; $c=[Windows.Forms.Clipboard]; if($c::ContainsFileDropList()){$p=$c::GetFileDropList()[0]; $b=[IO.File]::ReadAllBytes($p); $n=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([IO.Path]::GetFileName($p))); $m='application/octet-stream'} elseif($c::ContainsImage()){$s=New-Object IO.MemoryStream; $c::GetImage().Save($s,[Drawing.Imaging.ImageFormat]::Png); $b=$s.ToArray(); $n=''; $m='image/png'} else {'no image or file on the clipboard'; return}; $h=-join([Security.Cryptography.SHA256]::Create().ComputeHash($b)|%{$_.ToString('x2')}); Set-Clipboard -Value "richpaste:1:${m}:${n}:$($b.Length):${h}:$([Convert]::ToBase64String($b))"; 'ready: paste in the ssh window')PS";
}

auto html_page() -> std::string {
    return R"HTML(<!doctype html>
<meta charset="utf-8"><title>richpaste</title>
<style>body{font:16px system-ui;margin:3em;max-width:40em}#s{margin-top:1em}textarea{width:100%;height:6em}</style>
<h2>Paste or drop an image or file here</h2>
<p>Press Cmd+V (or Ctrl+V), or drop a file. It becomes one line of text on your clipboard.
Then paste that into the terminal app. Nothing leaves this page.</p>
<div id="s">waiting...</div><button id="b" hidden>Copy text</button><textarea id="t" hidden readonly></textarea>
<script>
function b64(bytes){let s='';for(let i=0;i<bytes.length;i+=32768)s+=String.fromCharCode.apply(null,bytes.subarray(i,i+32768));return btoa(s)}
function utf8b64(str){return b64(new TextEncoder().encode(str))}
async function sha(bytes){try{const d=await crypto.subtle.digest('SHA-256',bytes);return [...new Uint8Array(d)].map(x=>x.toString(16).padStart(2,'0')).join('')}catch(e){return ''}}
async function armor(mime,name,bytes){return 'richpaste:1:'+(mime||'application/octet-stream')+':'+(name?utf8b64(name):'')+':'+bytes.length+':'+(await sha(bytes))+':'+b64(bytes)}
async function take(file){const bytes=new Uint8Array(await file.arrayBuffer());const named=file.name&&!/^image\.(png|jpe?g|gif|webp|tiff?)$/i.test(file.name);return armor(file.type,named?file.name:'',bytes)}
const S=document.getElementById('s'),B=document.getElementById('b'),T=document.getElementById('t');
async function done(text){T.value=text;try{await navigator.clipboard.writeText(text);S.textContent='Copied ('+text.length+' chars). Now paste in the terminal.';return}catch(e){}
T.hidden=false;B.hidden=false;S.textContent='Ready. Click "Copy text", then paste in the terminal.'}
B.onclick=()=>{T.select();document.execCommand('copy');S.textContent='Copied. Now paste in the terminal.'};
async function handle(files){if(!files.length){S.textContent='No image or file in that paste.';return}S.textContent='Encoding...';const parts=[];for(const f of files)parts.push(await take(f));done(parts.join(' '))}
document.addEventListener('paste',e=>{e.preventDefault();const fs=[...e.clipboardData.items].filter(i=>i.kind==='file').map(i=>i.getAsFile());handle(fs)});
document.addEventListener('dragover',e=>e.preventDefault());
document.addEventListener('drop',e=>{e.preventDefault();handle([...e.dataTransfer.files])});
if(typeof module!=='undefined')module.exports={armor};
</script>
)HTML";
}

}  // namespace rp::armor
