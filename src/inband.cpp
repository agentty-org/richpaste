#include "rp/inband.hpp"

#include "rp/agent.hpp"   // config()
#include "rp/clipboard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace rp::inband {

namespace {

// Splits "a=1:b=2" into pairs.
auto split_meta(std::string_view meta) -> std::vector<std::pair<std::string_view, std::string_view>> {
    std::vector<std::pair<std::string_view, std::string_view>> out;
    while (!meta.empty()) {
        auto c = meta.find(':');
        auto kv = meta.substr(0, c);
        meta = c == std::string_view::npos ? std::string_view{} : meta.substr(c + 1);
        auto eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        out.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    }
    return out;
}

}  // namespace

auto parse_5522(std::string_view body) -> Result<Packet> {
    // body is everything between "ESC ] 5522;" and the terminator.
    auto semi = body.find(';');
    auto meta = body.substr(0, semi);
    auto payload = semi == std::string_view::npos ? std::string_view{} : body.substr(semi + 1);
    Packet p;
    for (auto [k, v] : split_meta(meta)) {
        if (k == "type") p.type = v;
        else if (k == "status") p.status = v;
        else if (k == "mime" || k == "pw") {
            auto d = base64_decode(v);
            if (!d) return fail("bad base64 in 5522 {}", k);
            (k == "mime" ? p.mime : p.pw) = as_string(*d);
        }
    }
    if (!payload.empty()) {
        auto d = base64_decode(payload);
        if (!d) return fail("bad base64 in 5522 payload");
        p.data = std::move(*d);
    }
    return p;
}

auto ReadReply::add(const Packet& p, std::uint64_t max_bytes) -> Result<bool> {
    if (p.type != "read") return false;
    if (p.status == "OK") return false;
    if (p.status == "DONE") return true;
    if (p.status == "DATA") {
        total_ += p.data.size();
        if (total_ > max_bytes) return fail("clipboard is larger than the {} limit", human_size(max_bytes));
        if (parts_.empty() || parts_.back().first != p.mime) parts_.emplace_back(p.mime, Bytes{});
        auto& b = parts_.back().second;
        b.insert(b.end(), p.data.begin(), p.data.end());
        return false;
    }
    if (p.status == "EPERM") return fail("the terminal refused clipboard access");
    if (p.status == "ENOSYS") return fail("the terminal has no clipboard here");
    if (p.status == "EBUSY") return fail("the clipboard is busy, try again");
    return fail("terminal clipboard error {}", p.status);
}

auto ReadReply::mimes() const -> std::vector<std::string> {
    std::vector<std::string> out;
    for (auto& [mime, data] : parts_) {
        if (mime != ".") continue;
        auto s = as_string(data);
        std::size_t i = 0;
        while (i < s.size()) {
            auto j = s.find_first_of(" \n", i);
            auto t = s.substr(i, j == std::string::npos ? j : j - i);
            if (!t.empty()) out.push_back(t);
            if (j == std::string::npos) break;
            i = j + 1;
        }
    }
    return out;
}

auto ReadReply::items() const -> Result<std::vector<Item>> {
    // Offer what arrived to the normal chooser, so files/images/text are ranked the same way.
    Offer o;
    for (auto& [m, d] : parts_)
        if (m != ".") o.targets.push_back(m);
    o.read = [this](std::string_view t, std::size_t) -> Result<Bytes> {
        for (auto& [m, d] : parts_)
            if (m == t) return d;
        return fail("not sent");
    };
    return choose(o, ~0ull);
}

auto tmux_wrap(std::string_view esc) -> std::string {
    std::string out = "\x1bPtmux;";
    for (char c : esc) {
        if (c == '\x1b') out += '\x1b';
        out += c;
    }
    out += "\x1b\\";
    return out;
}

auto query_5522_read(std::span<const std::string> mimes, std::string_view pw) -> std::string {
    std::string list;
    for (auto& m : mimes) list += (list.empty() ? "" : " ") + m;
    std::string meta = "type=read";
    if (!pw.empty()) meta += ":pw=" + base64_encode(as_view(pw)) + ":name=" + base64_encode(as_view("Paste event"));
    return "\x1b]5522;" + meta + ";" + base64_encode(as_view(list)) + "\x1b\\";
}

auto choose_mimes(std::span<const std::string> offered) -> std::vector<std::string> {
    auto has = [&](std::string_view m) { return std::ranges::contains(offered, m); };
    for (auto f : {"text/uri-list"})
        if (has(f)) return {f};
    for (auto i : {"image/png", "image/jpeg", "image/webp", "image/gif", "image/avif", "image/bmp", "image/tiff"})
        if (has(i)) return {i};
    for (auto& m : offered)
        if (m.starts_with("image/")) return {m};
    for (auto t : {"text/plain;charset=utf-8", "text/plain"})
        if (has(t)) return {t};
    return {"text/plain"};
}

// ---------- blocking session ----------

namespace {

enum class Kind5 { osc5522, osc52, decrqm, da1 };

struct Reply {
    Kind5 kind;
    std::string body;
};

// Pulls terminal replies out of a byte stream. Anything that isn't a reply is
// returned as `other` so the caller can feed it back to its normal input.
class ReplyScanner {
public:
    void feed(ByteView b) { buf_ += as_string(b); }

    auto next() -> std::optional<Reply> {
        for (;;) {
            auto esc = buf_.find('\x1b');
            if (esc == std::string::npos) {
                other_ += buf_;
                buf_.clear();
                return std::nullopt;
            }
            other_ += buf_.substr(0, esc);
            buf_.erase(0, esc);
            if (buf_.size() < 2) return std::nullopt;
            if (buf_[1] == ']') {
                auto end = find_st(2);
                if (!end) return std::nullopt;
                auto [stop, len] = *end;
                std::string body = buf_.substr(2, stop - 2);
                buf_.erase(0, stop + len);
                if (body.starts_with("5522;")) return Reply{Kind5::osc5522, body.substr(5)};
                if (body.starts_with("52;")) return Reply{Kind5::osc52, body.substr(3)};
                continue;
            }
            if (buf_[1] == '[') {
                std::size_t i = 2;
                while (i < buf_.size() && (static_cast<unsigned char>(buf_[i]) < 0x40 || static_cast<unsigned char>(buf_[i]) > 0x7E)) ++i;
                if (i >= buf_.size()) return std::nullopt;
                std::string seq = buf_.substr(0, i + 1);
                buf_.erase(0, i + 1);
                if (seq.starts_with("\x1b[?5522;") && seq.ends_with("$y")) return Reply{Kind5::decrqm, seq.substr(8, seq.size() - 10)};
                if (seq.starts_with("\x1b[?") && seq.ends_with("c")) return Reply{Kind5::da1, seq};
                other_ += seq;
                continue;
            }
            other_ += buf_.substr(0, 2);
            buf_.erase(0, 2);
        }
    }

    auto take_other() -> std::string { return std::exchange(other_, {}); }

private:
    // Finds BEL or ESC \ from pos. Returns (index, terminator length).
    auto find_st(std::size_t pos) const -> std::optional<std::pair<std::size_t, std::size_t>> {
        for (std::size_t i = pos; i < buf_.size(); ++i) {
            if (buf_[i] == '\x07') return std::pair{i, std::size_t{1}};
            if (buf_[i] == '\x1b' && i + 1 < buf_.size() && buf_[i + 1] == '\\') return std::pair{i, std::size_t{2}};
        }
        return std::nullopt;
    }
    std::string buf_;
    std::string other_;
};

auto send(const Session& s, std::string_view esc) -> Result<void> {
    auto wrapped = s.caps.tmux && s.caps.passthrough ? tmux_wrap(esc) : std::string(esc);
    return s.write(wrapped);
}

// Sends q, then a DA1 as a fence. Every terminal answers DA1, and replies come in order,
// so once DA1 arrives we know nothing else is coming.
template <class F>
auto exchange(const Session& s, std::string_view q, Millis timeout, F&& on_reply) -> Result<void> {
    if (auto w = send(s, q); !w) return w;
    // The fence goes the same way the query went, so it can't overtake it.
    if (auto w = send(s, "\x1b[c"); !w) return w;
    ReplyScanner sc;
    std::array<std::byte, 64 * 1024> buf;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto left = std::chrono::duration_cast<Millis>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return fail("terminal did not answer");
        auto n = s.read(buf, left);
        if (!n) return fail("terminal did not answer");
        if (*n == 0) return fail("terminal closed");
        sc.feed(std::span(buf).first(*n));
        while (auto r = sc.next()) {
            if (r->kind == Kind5::da1) return {};
            if (auto d = on_reply(*r); !d) return d;
            // Long transfers keep the deadline fresh while data flows.
            deadline = std::chrono::steady_clock::now() + timeout;
        }
    }
}

}  // namespace

auto detect(Session& s, Millis timeout) -> Caps {
    Caps c;
    // $TMUX can be stale (inherited by a terminal started from inside tmux), so ask tmux.
    if (env("TMUX") && env("TMUX_PANE")) {
        std::vector<std::string> q{"tmux", "display", "-p", "-t", *env("TMUX_PANE"), "#{client_termtype}"};
        if (auto t = run_capture(q, Millis(1000))) {
            c.tmux = true;
            c.terminal = *t;
        }
    }
    if (c.tmux) {
        // Let our wrapped escapes through for this pane only. Harmless if already on.
        std::vector<std::string> cmd{"tmux", "set", "-p", "-t", *env("TMUX_PANE"), "allow-passthrough", "on"};
        c.passthrough = run_capture(cmd, Millis(1000)).has_value();
    } else {
        c.terminal = env("TERM_PROGRAM").or_else([] { return env("TERM"); }).value_or("");
    }
    s.caps = c;
    // A forced mode, for terminals that misreport (agentty: paste.terminal).
    if (auto m = config().terminal; m == TerminalMode::osc5522 || m == TerminalMode::osc52) {
        c.osc5522 = m == TerminalMode::osc5522;
        s.caps = c;
        return c;
    }
    (void)exchange(s, "\x1b[?5522$p", timeout, [&](const Reply& r) -> Result<void> {
        // 1 = set, 2 = reset (supported but off). 0 = unknown.
        if (r.kind == Kind5::decrqm && (r.body == "1" || r.body == "2")) c.osc5522 = true;
        return {};
    });
    s.caps = c;
    return c;
}

auto read_clipboard(const Session& s, std::uint64_t max_bytes, Millis timeout) -> Result<std::vector<Item>> {
    if (s.caps.osc5522) {
        std::vector<std::string> dot{"."};
        ReadReply listing;
        std::optional<Error> err;
        auto r = exchange(s, query_5522_read(dot), timeout, [&](const Reply& rep) -> Result<void> {
            if (rep.kind != Kind5::osc5522) return {};
            auto p = parse_5522(rep.body);
            if (!p) return std::unexpected(p.error());
            if (auto a = listing.add(*p, 1 << 20); !a) err = a.error();
            return {};
        });
        if (!r) return std::unexpected(r.error());
        if (err) return std::unexpected(*err);
        auto offered = listing.mimes();
        if (offered.empty()) return fail("clipboard is empty");
        auto want = choose_mimes(offered);
        ReadReply data;
        r = exchange(s, query_5522_read(want), timeout, [&](const Reply& rep) -> Result<void> {
            if (rep.kind != Kind5::osc5522) return {};
            auto p = parse_5522(rep.body);
            if (!p) return std::unexpected(p.error());
            if (auto a = data.add(*p, max_bytes); !a) err = a.error();
            return {};
        });
        if (!r) return std::unexpected(r.error());
        if (err) return std::unexpected(*err);
        auto items = data.items();
        if (!items) return items;
        // A uri-list from the terminal names files on the terminal's machine.
        // If we can read them here (local session) choose() already did.
        return items;
    }

    // OSC 52: text only. Inside tmux, ask tmux to fetch it from the outer terminal.
    if (s.caps.tmux) {
        auto t = tmux_refresh_clipboard(timeout);
        if (t && !t->empty()) return std::vector<Item>{Text::repair(*t)};
        if (!t) return std::unexpected(t.error());
        return fail("clipboard is empty");
    }
    std::string text;
    bool got = false;
    auto r = exchange(s, "\x1b]52;c;?\x1b\\", timeout, [&](const Reply& rep) -> Result<void> {
        if (rep.kind != Kind5::osc52) return {};
        auto semi = rep.body.find(';');
        auto b64 = semi == std::string::npos ? std::string_view{} : std::string_view(rep.body).substr(semi + 1);
        if (b64 == "?") return {};
        auto d = base64_decode(b64);
        if (!d) return std::unexpected(d.error());
        if (d->size() > max_bytes) return fail("clipboard is larger than the {} limit", human_size(max_bytes));
        text = as_string(*d);
        got = true;
        return {};
    });
    if (!r) return std::unexpected(r.error());
    if (!got) return fail("the terminal didn't answer a clipboard read (OSC 52 reads are off or unsupported)");
    if (text.empty()) return fail("clipboard is empty");
    return std::vector<Item>{Text::repair(text)};
}

auto tmux_refresh_clipboard(Millis timeout) -> Result<std::string> {
    // tmux asks the outer terminal (OSC 52) and stores the answer as a new paste buffer.
    // Works with default tmux options; we wait for that buffer to show up.
    std::vector<std::string> list{"tmux", "list-buffers", "-F", "#{buffer_created} #{buffer_name}"};
    auto newest = [&]() -> std::pair<long long, std::string> {
        auto l = run_capture(list, Millis(1000));
        if (!l || l->empty()) return {0, ""};
        auto line = l->substr(0, l->find('\n'));
        auto sp = line.find(' ');
        long long t = 0;
        try { t = std::stoll(line.substr(0, sp)); } catch (...) {}
        return {t, sp == std::string::npos ? "" : line.substr(sp + 1)};
    };
    auto before = newest();
    std::vector<std::string> refresh{"tmux", "refresh-client", "-l"};
    if (!run_capture(refresh, Millis(1000))) return fail("tmux refresh-client -l failed");
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto now = newest();
        if (!now.second.empty() && now != before) {
            std::vector<std::string> show{"tmux", "show-buffer", "-b", now.second};
            auto out = run_capture(show, Millis(2000));
            if (out) return *out;
        }
        std::this_thread::sleep_for(Millis(50));
    }
    return fail("the terminal outside tmux didn't answer a clipboard read (OSC 52 reads are off or unsupported)");
}

}  // namespace rp::inband
