// Unit tests. No framework: each check prints and counts failures.

#include "rp/agent.hpp"
#include "rp/armor.hpp"
#include "rp/clipboard.hpp"
#include "rp/image.hpp"
#include "rp/inband.hpp"
#include "rp/sha256.hpp"
#include "rp/store.hpp"
#include "rp/wire.hpp"

#include <print>

using namespace rp;

namespace {

int failures = 0;

void check(bool ok, std::string_view what) {
    if (!ok) {
        ++failures;
        std::println("FAIL {}", what);
    }
}

auto bytes_of(std::string_view s) -> Bytes { return to_bytes(s); }

void test_sha256() {
    check(hex(sha256(bytes_of(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
    check(hex(sha256(bytes_of("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    check(hex(sha256(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256 two blocks");
    Bytes million(1'000'000, std::byte{'a'});
    Sha256 s;
    for (std::size_t i = 0; i < million.size(); i += 777) s.update(std::span(million).subspan(i, std::min<std::size_t>(777, million.size() - i)));
    check(hex(s.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 million a, chunked");
}

void test_types() {
    check(Mime::parse("image/png").has_value(), "mime ok");
    check(Mime::parse("Image/PNG")->str() == "image/png", "mime lowercased");
    check(!Mime::parse("png").has_value(), "mime needs slash");
    check(!Mime::parse("image/").has_value(), "mime needs subtype");
    check(!Mime::parse("ima ge/png").has_value(), "mime rejects space");
    check(Mime::parse("text/plain;charset=utf-8")->essence() == "text/plain", "mime essence");

    check(!FileName::parse("../etc/passwd").has_value(), "filename rejects traversal");
    check(!FileName::parse("a/b").has_value(), "filename rejects slash");
    check(FileName::parse("photo.png").has_value(), "filename ok");
    check(FileName::sanitize("/home/x/../../evil.sh").str() == "evil.sh", "sanitize strips dirs");
    check(FileName::sanitize("..").str() == "file", "sanitize dotdot");
    check(FileName::sanitize(".bashrc").str() == "_bashrc", "sanitize hidden");
    check(FileName::sanitize("a\x01" "b").str() == "ab", "sanitize control");

    check(valid_utf8("héllo 🎉"), "utf8 valid");
    check(!valid_utf8("\xC0\xAF"), "utf8 overlong");
    check(!valid_utf8("\xED\xA0\x80"), "utf8 surrogate");
    check(repair_utf8("a\xFF" "b") == "a\xEF\xBF\xBD" "b", "utf8 repair");
    check(latin1_to_utf8("caf\xE9") == "café", "latin1");
}

void test_wire() {
    auto req = wire::encode_request({wire::Op::clipboard, 1234, 2, "tok"});
    check(req.size() == wire::request_head + 3, "request size");
    auto back = wire::decode_request(req);
    check(back && back->max_total_bytes == 1234 && back->hops == 2 && back->token == "tok", "request roundtrip");
    auto loop = wire::encode_request({wire::Op::clipboard, 1, 99, ""});
    check(!wire::decode_request(loop), "request rejects hop loop");

    Bytes png = {std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}, std::byte{0}, std::byte{0xFF}};
    std::vector<Item> items;
    items.push_back(Text::repair("hello\nworld"));
    items.push_back(Image{*Mime::parse("image/png"), png});
    items.push_back(File{*FileName::parse("a.pdf"), *Mime::parse("application/pdf"), bytes_of("%PDF-1.4")});
    auto enc = wire::encode_items(items);
    auto dec = wire::decode_response(enc);
    check(dec.has_value(), "response decodes");
    if (dec) {
        check(dec->size() == 3, "three items");
        check(std::get<Text>((*dec)[0]).str() == "hello\nworld", "text survives");
        check(std::get<Image>((*dec)[1]).data == png, "image bytes survive");
        check(std::get<File>((*dec)[2]).name.str() == "a.pdf", "file name survives");
    }

    // Flip one payload byte: hash must catch it.
    auto bad = enc;
    bad[bad.size() - 40] ^= std::byte{1};
    auto r = wire::decode_response(bad);
    check(!r && r.error().what.contains("sha256"), "corruption detected");

    // Every truncation must fail cleanly, never crash.
    bool all_fail = true;
    for (std::size_t n = 0; n < enc.size(); ++n)
        if (wire::decode_response(std::span(enc).first(n))) all_fail = false;
    check(all_fail, "every truncation rejected");

    // A lying size field must not allocate or read past the end.
    wire::Writer w;
    w.put_magic(wire::response_magic);
    w.put(std::uint8_t{0});
    w.put(std::uint32_t{1});
    w.put(std::uint8_t{2});
    w.put_str16("image/png");
    w.put_str16("");
    w.put(std::uint64_t{1} << 60);
    auto liar = std::move(w).bytes();
    check(!wire::decode_response(liar), "huge size rejected");

    // A file item with a path in its name is rejected at the boundary.
    wire::Writer w2;
    w2.put_magic(wire::response_magic);
    w2.put(std::uint8_t{0});
    w2.put(std::uint32_t{1});
    w2.put(std::uint8_t{3});
    w2.put_str16("text/plain");
    w2.put_str16("../../.bashrc");
    w2.put(std::uint64_t{1});
    w2.put(as_view("x"));
    w2.put(ByteView(sha256(as_view("x"))));
    check(!wire::decode_response(std::move(w2).bytes()), "path traversal name rejected");

    auto err = wire::decode_response(wire::encode_error("clipboard is empty", Code::empty));
    check(!err && err.error().what == "clipboard is empty", "error passes through");
    check(!err && err.error().code == Code::empty, "error code passes through");

    // An older agent sends no code byte.
    wire::Writer old;
    old.put_magic(wire::response_magic);
    old.put(std::uint8_t{1});
    old.put_str16("nope");
    auto o = wire::decode_response(std::move(old).bytes());
    check(!o && o.error().code == Code::other && o.error().what == "nope", "old agent error still reads");

    check(!r || r.error().code == Code::corrupt, "corruption has its code");
}

void test_errors() {
    Error e{Code::no_route, "x", {}, {}};
    e.causes.push_back(Error{Code::unreachable, "a", {}, Route::forward});
    e.causes.push_back(Error{Code::no_answer, "b", {}, Route::tmux_buffer});
    check(e.any(Code::no_answer) && !e.any(Code::empty), "any() looks in causes");
    check(e.from(Route::tmux_buffer) && e.from(Route::tmux_buffer)->what == "b", "from() finds the route");
    check(!e.from(Route::display), "from() misses untried routes");
    auto c = context(Error{Code::too_big, "big", {}, {}}, "tmux");
    check(c.error().code == Code::too_big && c.error().what == "tmux: big", "context keeps the code");
}

void test_plan() {
    Config c;
    using V = std::vector<Route>;
    using I = Images;
    check(plan({.ssh = false, .tmux = false}, c) == V{Route::display, Route::forward, Route::terminal}, "plan: local");
    check(plan({.ssh = true, .tmux = false}, c) == V{Route::forward, Route::terminal}, "plan: ssh skips server display");
    // Inside tmux: kitty is asked first (it sends images), tmux's buffer is the
    // text fallback. Any other terminal: text through tmux only.
    check(plan({.ssh = true, .tmux = true, .images = I::yes}, c) == V{Route::forward, Route::terminal, Route::tmux_buffer},
          "plan: ssh + tmux + kitty");
    check(plan({.ssh = true, .tmux = true, .images = I::no}, c) == V{Route::forward, Route::tmux_buffer},
          "plan: ssh + tmux + other terminal");
    check(plan({.ssh = true, .tmux = true, .images = I::unknown}, c) == V{Route::forward, Route::tmux_buffer},
          "plan: ssh + tmux + unknown terminal");
    c.server_display = true;
    check(plan({.ssh = true, .tmux = false}, c).back() == Route::display, "plan: server_display last");
    c.terminal = TerminalMode::off;
    check(plan({.ssh = true, .tmux = true}, c) == V{Route::forward, Route::display}, "plan: terminal off drops tmux too");
    c.socket = "/tmp/x.sock";
    check(plan({.ssh = true, .tmux = true}, c) == V{Route::socket}, "plan: a set socket is the only route");
}

void test_walk() {
    // No terminal handed in: the walk stops at the terminal for the caller to
    // ask, and resumes after skip().
    Walk w{{.ssh = true, .tmux = false, .images = Images::yes}, {Route::terminal}, 0, {}};
    auto r = advance(w);
    check(!r && r.error().code == Code::needs_terminal && r.error().route == Route::terminal,
          "walk: stops at the terminal for the caller");
    skip(w, Error{Code::no_answer, "silent", {}, {}});
    check(w.next == 1 && w.causes.size() == 1 && w.causes[0].route == Route::terminal, "walk: skip records why");
    auto end = advance(w);
    check(!end && end.error().code == Code::no_route && end.error().from(Route::terminal), "walk: no_route with causes");
    // The terminal saying "empty" (no text for an image) is a cause; if that's
    // all there was, the clipboard is empty.
    Walk em{{.ssh = true}, {Route::terminal}, 0, {}};
    skip(em, Error{Code::empty, "clipboard is empty", {}, {}});
    auto ee = advance(em);
    check(!ee && ee.error().code == Code::empty, "walk: only empties is empty");
}

void test_progress() {
    using S = armor::Progress::State;
    Bytes img(3000);
    for (std::size_t i = 0; i < img.size(); ++i) img[i] = std::byte(i * 11);
    auto line = armor::encode(Image{*Mime::parse("image/png"), img});
    check(armor::progress("plain text").state == S::none, "progress: none");
    check(armor::progress(line).state == S::complete, "progress: whole line");
    check(armor::progress(line + "\nmore").state == S::complete, "progress: text after");
    check(armor::progress(line.substr(0, 20)).state == S::partial, "progress: header arriving");
    auto half = armor::progress(line.substr(0, line.size() / 2));
    check(half.state == S::partial && half.need == img.size() && half.have > 0 && half.have < img.size(),
          "progress: half the data");
    // Wrapped by a terminal: whitespace inside the data doesn't count.
    std::string wrapped;
    for (std::size_t i = 0; i < line.size(); i += 80) wrapped += line.substr(i, 80) + "\n";
    check(armor::progress(wrapped).state == S::complete, "progress: wrapped line");
    check(armor::progress(wrapped.substr(0, wrapped.size() - 200)).state == S::partial, "progress: wrapped, short");
    // Cut and followed by other text: nothing more is coming for it.
    check(armor::progress(line.substr(0, line.size() / 2) + "!? and then").state == S::complete,
          "progress: cut block is final");
    // Two blocks, the second still arriving.
    check(armor::progress(line + " " + line.substr(0, 100)).state == S::partial, "progress: second block arriving");
}

void test_stripped() {
    using S = inband::Stripped;
    Bytes img(6000);
    for (std::size_t i = 0; i < img.size(); ++i) img[i] = std::byte(i * 3);
    std::string s = "]5522;type=read:status=OK\\";
    for (std::size_t off = 0; off < img.size(); off += 4096) {
        auto chunk = ByteView(img).subspan(off, std::min<std::size_t>(4096, img.size() - off));
        s += "]5522;type=read:status=DATA:mime=aW1hZ2UvcG5n;" + base64_encode(chunk) + "\\";
    }
    std::string head = s;
    s += "]5522;type=read:status=DONE\\";
    check(inband::stripped_reply("hello") == S::none, "stripped: none");
    check(inband::stripped_reply(head) == S::partial, "stripped: no DONE yet");
    check(inband::stripped_reply(head.substr(0, head.size() - 30)) == S::partial, "stripped: mid packet");
    check(inband::stripped_reply(s) == S::complete, "stripped: complete");
    auto items = inband::decode_stripped_reply(s, 1 << 20);
    check(items && items->size() == 1 && std::get<Image>((*items)[0]).data == img, "stripped: image back");
    check(!inband::decode_stripped_reply(head, 1 << 20), "stripped: cut short fails");
    auto big = inband::decode_stripped_reply(s, 100);
    check(!big && big.error().code == Code::too_big, "stripped: size limit");
    // BEL terminators, as some terminals send.
    std::string bel = "]5522;type=read:status=DATA:mime=dGV4dC9wbGFpbg==;aGk=\x07]5522;type=read:status=DONE\x07";
    auto t = inband::decode_stripped_reply(bel, 1 << 20);
    check(t && std::get<Text>((*t)[0]).str() == "hi", "stripped: BEL terminated text");
    auto perm = inband::decode_stripped_reply("]5522;type=read:status=EPERM\\", 1 << 20);
    check(!perm && perm.error().code == Code::refused, "stripped: EPERM is refused");
}

void test_choose() {
    Bytes png = bytes_of("\x89PNG fake");
    Offer o{{"TARGETS", "image/png", "text/plain", "UTF8_STRING"}, [&](std::string_view t, std::size_t) -> Result<Bytes> {
                if (t == "image/png") return png;
                return bytes_of("alt text");
            }};
    auto r = choose(o, 1 << 20);
    check(r && r->size() == 1 && std::holds_alternative<Image>((*r)[0]), "image beats text");

    Offer t{{"UTF8_STRING", "STRING"}, [](std::string_view tt, std::size_t) -> Result<Bytes> {
                return tt == "UTF8_STRING" ? bytes_of("ünïcode") : bytes_of("?");
            }};
    auto rt = choose(t, 1 << 20);
    check(rt && std::get<Text>((*rt)[0]).str() == "ünïcode", "utf8 text chosen");

    Offer s{{"STRING"}, [](std::string_view, std::size_t) -> Result<Bytes> { return bytes_of("caf\xE9"); }};
    auto rs = choose(s, 1 << 20);
    check(rs && std::get<Text>((*rs)[0]).str() == "café", "latin1 STRING converted");

    Offer none{{}, nullptr};
    check(!choose(none, 1), "empty offer fails");

    auto paths = parse_uri_list("# comment\r\nfile:///tmp/a%20b.png\r\nfile://localhost/etc/hosts\nhttp://x/y\nfile://otherhost/z\n");
    check(paths.size() == 2 && paths[0] == "/tmp/a b.png" && paths[1] == "/etc/hosts", "uri list parsed");
}

void test_store() {
    auto dir = std::filesystem::temp_directory_path() / ("rp-store-" + random_hex(6));
    Bytes png(64, std::byte{7});
    std::vector<Item> items{Text::repair("see "), Image{*Mime::parse("image/png"), png},
                            File{FileName::sanitize("a.pdf"), *Mime::parse("application/pdf"), png}};
    auto r = store(items, dir);
    check(r && r->parts.size() == 3, "store: three parts");
    if (r && r->parts.size() == 3) {
        check(std::get<std::string>(r->parts[0]) == "see ", "store: text passes through");
        auto& img = std::get<Saved>(r->parts[1]);
        check(img.path.extension() == ".png" && img.size == 64, "store: image saved as .png");
        auto& f = std::get<Saved>(r->parts[2]);
        check(f.path.filename().string().ends_with("-a.pdf"), "store: file keeps its name");
        auto back = read_file(img.path, 1 << 20);
        check(back && *back == png, "store: bytes on disk match");
        auto again = store(items, dir);
        check(again && std::get<Saved>(again->parts[1]).path == img.path, "store: same bytes, same path");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_base64() {
    check(base64_encode(as_view("")) == "", "b64 empty");
    check(base64_encode(as_view("f")) == "Zg==", "b64 f");
    check(base64_encode(as_view("fo")) == "Zm8=", "b64 fo");
    check(base64_encode(as_view("foobar")) == "Zm9vYmFy", "b64 foobar");
    check(as_string(*base64_decode("Zm9vYg==")) == "foob", "b64 decode padded");
    check(!base64_decode("Zm9vYg"), "b64 rejects missing padding");
    check(!base64_decode("Zm9v\nYmFy"), "b64 rejects whitespace");
    check(!base64_decode("Zg==Zg=="), "b64 rejects padding in the middle");
    Bytes all(256);
    for (int i = 0; i < 256; ++i) all[static_cast<std::size_t>(i)] = std::byte(i);
    check(*base64_decode(base64_encode(all)) == all, "b64 roundtrip all bytes");
    check(hex(hmac_sha256(as_view("key"), as_view("The quick brown fox jumps over the lazy dog"))) ==
              "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
          "hmac-sha256");
}

void test_inband() {
    auto p = inband::parse_5522("type=read:status=DATA:mime=aW1hZ2UvcG5n;AAEC");
    check(p && p->status == "DATA" && p->mime == "image/png" && p->data.size() == 3, "5522 DATA parsed");
    check(!inband::parse_5522("type=read:status=DATA:mime=@@;AAEC"), "5522 bad mime rejected");

    inband::ReadReply list;
    (void)list.add(*inband::parse_5522("type=read:status=OK"), 1 << 20);
    (void)list.add(*inband::parse_5522("type=read:status=DATA:mime=Lg==;" + base64_encode(as_view("text/plain\nimage/png\n"))), 1 << 20);
    auto done = list.add(*inband::parse_5522("type=read:status=DONE"), 1 << 20);
    check(done && *done, "5522 listing done");
    auto mimes = list.mimes();
    check(mimes.size() == 2 && mimes[1] == "image/png", "5522 listing mimes");
    auto want = inband::choose_mimes(mimes);
    check(want.size() == 1 && want[0] == "image/png", "5522 prefers image");

    inband::ReadReply data;
    Bytes img(10000);
    for (std::size_t i = 0; i < img.size(); ++i) img[i] = std::byte(i * 7);
    for (std::size_t off = 0; off < img.size(); off += 4096) {
        auto chunk = ByteView(img).subspan(off, std::min<std::size_t>(4096, img.size() - off));
        (void)data.add(*inband::parse_5522("type=read:status=DATA:mime=aW1hZ2UvcG5n;" + base64_encode(chunk)), 1 << 20);
    }
    (void)data.add(*inband::parse_5522("type=read:status=DONE"), 1 << 20);
    auto items = data.items();
    check(items && std::get<Image>((*items)[0]).data == img, "5522 chunks join into one image");

    inband::ReadReply small;
    auto big = small.add(*inband::parse_5522("type=read:status=DATA:mime=aW1hZ2UvcG5n;" + base64_encode(img)), 100);
    check(!big, "5522 size limit enforced");
    inband::ReadReply perm;
    check(!perm.add(*inband::parse_5522("type=read:status=EPERM"), 1), "5522 EPERM is an error");

    check(inband::tmux_wrap("\x1b]52;c;?\x07") == "\x1bPtmux;\x1b\x1b]52;c;?\x07\x1b\\", "tmux wrap doubles ESC");
}

void test_image() {
    check(image::crc32(as_view("123456789")) == 0xCBF43926, "crc32 check value");
    check(image::adler32(as_view("Wikipedia")) == 0x11E60398, "adler32 check value");

    // 24-bit bottom-up 2x2 DIB: row0 (bottom) = blue, green; row1 (top) = red, white.
    Bytes dib(40 + 2 * 8);
    auto put32 = [&](std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) dib[o + i] = std::byte(v >> (8 * i)); };
    put32(0, 40);
    put32(4, 2);
    put32(8, 2);
    dib[12] = std::byte{1};
    dib[14] = std::byte{24};
    std::uint8_t px[] = {255, 0, 0, 0, 255, 0, 0, 0, /* bottom: blue, green */ 0, 0, 255, 255, 255, 255, 0, 0};
    for (std::size_t i = 0; i < 16; ++i) dib[40 + i] = std::byte{px[i]};
    auto img = image::decode_dib(dib);
    check(img && img->width == 2 && img->height == 2, "dib decoded");
    if (img) {
        auto at = [&](std::size_t i) { return std::to_integer<int>(img->pixels[i]); };
        check(at(0) == 255 && at(1) == 0 && at(2) == 0 && at(3) == 255, "dib top-left is red");
        check(at(12) == 0 && at(13) == 255 && at(14) == 0, "dib bottom-right is green");
        auto png = image::encode_png(*img);
        check(png.size() > 50 && png[1] == std::byte{'P'}, "png encoded");
    }
    check(!image::decode_dib(ByteView(dib).first(50)), "truncated dib rejected");
}

void test_deflate_roundtrip_shape() {
    // A highly repetitive input must compress well (proves matches are emitted).
    Bytes rep(200000, std::byte{'a'});
    auto z = image::zlib_compress(rep);
    check(z.size() < 2000, "deflate compresses repeats");
    check(z[0] == std::byte{0x78}, "zlib header");
}

}  // namespace

int main() {
    test_sha256();
    test_types();
    test_wire();
    test_errors();
    test_plan();
    test_walk();
    test_progress();
    test_stripped();
    test_choose();
    test_store();
    test_base64();
    test_inband();
    test_image();
    test_deflate_roundtrip_shape();
    {
        Bytes img(5000);
        for (std::size_t i = 0; i < img.size(); ++i) img[i] = std::byte(i * 13);
        auto line = armor::encode(Image{*Mime::parse("image/png"), img});
        auto out = armor::expand({Text::repair("see " + line + " ok")});
        check(out.size() == 3, "expand: text, image, text");
        check(std::holds_alternative<Image>(out[1]) && std::get<Image>(out[1]).data == img, "expand: image bytes");
        auto only = armor::expand({Text::repair(line + "\n")});
        check(only.size() == 1 && std::holds_alternative<Image>(only[0]), "expand: lone block, no stray text");
        auto plain = armor::expand({Text::repair("just text")});
        check(plain.size() == 1 && std::get<Text>(plain[0]).str() == "just text", "expand: plain text untouched");
        std::vector<std::string> errs;
        auto cut = armor::expand({Text::repair(line.substr(0, line.size() / 2))}, &errs);
        check(cut.empty() && errs.size() == 1, "expand: cut-short block reported");
    }
    if (failures) {
        std::println("{} check(s) failed", failures);
        return 1;
    }
    std::println("all unit checks passed");
    return 0;
}
