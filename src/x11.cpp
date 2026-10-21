// Minimal X11 client that reads the CLIPBOARD selection.
// Speaks the core protocol directly over the socket, no Xlib/xcb.
// Handles Xauthority cookies and INCR transfers (big images).
#ifndef _WIN32

#include "rp/clipboard.hpp"
#include "rp/io.hpp"

#include <charconv>
#include <cstring>
#include <unistd.h>

namespace rp::x11 {

namespace {

constexpr std::uint32_t None = 0;
constexpr std::uint8_t SelectionNotify = 31;
constexpr std::uint8_t PropertyNotify = 28;
constexpr std::uint32_t PropertyChangeMask = 0x400000;

auto pad4(std::size_t n) -> std::size_t { return (n + 3) & ~std::size_t{3}; }

auto get32(ByteView b, std::size_t off) -> std::uint32_t {
    std::uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
auto get16(ByteView b, std::size_t off) -> std::uint16_t {
    std::uint16_t v;
    std::memcpy(&v, b.data() + off, 2);
    return v;
}

class Req {
public:
    Req(std::uint8_t opcode, std::uint8_t data) {
        u8(opcode);
        u8(data);
        u16(0);  // length, patched in done()
    }
    auto u8(std::uint8_t v) -> Req& {
        out_.push_back(std::byte{v});
        return *this;
    }
    auto u16(std::uint16_t v) -> Req& { return raw(&v, 2); }
    auto u32(std::uint32_t v) -> Req& { return raw(&v, 4); }
    auto str(std::string_view s) -> Req& {
        raw(s.data(), s.size());
        while (out_.size() % 4) out_.push_back(std::byte{0});
        return *this;
    }
    auto done() -> Bytes {
        auto units = static_cast<std::uint16_t>(out_.size() / 4);
        std::memcpy(out_.data() + 2, &units, 2);
        return std::move(out_);
    }

private:
    auto raw(const void* p, std::size_t n) -> Req& {
        auto b = static_cast<const std::byte*>(p);
        out_.insert(out_.end(), b, b + n);
        return *this;
    }
    Bytes out_;
};

struct Display {
    std::string socket;
    std::string number;
};

auto parse_display(std::string_view d) -> Result<Display> {
    if (d.starts_with('/')) {  // macOS launchd style: /path/to/sock:0
        auto colon = d.rfind(':');
        auto path = std::string(d.substr(0, colon));
        auto num = colon == std::string_view::npos ? "0" : std::string(d.substr(colon + 1));
        num = num.substr(0, num.find('.'));
        return Display{path, num};
    }
    if (d.starts_with("unix:")) d.remove_prefix(4);
    if (!d.starts_with(':')) return fail("only local X displays are supported, not '{}'", d);
    auto num = std::string(d.substr(1, d.find('.') == std::string_view::npos ? d.npos : d.find('.') - 1));
    int n = 0;
    auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), n);
    if (ec != std::errc{} || p != num.data() + num.size()) return fail("bad DISPLAY '{}'", d);
    return Display{"/tmp/.X11-unix/X" + num, num};
}

struct Cookie {
    std::string name;
    Bytes data;
};

// Finds the MIT-MAGIC-COOKIE-1 for a local display in the Xauthority file.
auto find_cookie(const std::string& number) -> std::optional<Cookie> {
    auto path = env("XAUTHORITY").value_or((home_dir() / ".Xauthority").string());
    auto file = read_file(path, 1 << 20);
    if (!file) return std::nullopt;
    char host[256] = {};
    ::gethostname(host, sizeof(host) - 1);

    ByteView b(*file);
    auto be16 = [&](std::size_t& off) -> std::optional<std::uint16_t> {
        if (off + 2 > b.size()) return std::nullopt;
        auto v = static_cast<std::uint16_t>((std::to_integer<unsigned>(b[off]) << 8) | std::to_integer<unsigned>(b[off + 1]));
        off += 2;
        return v;
    };
    auto field = [&](std::size_t& off) -> std::optional<ByteView> {
        auto n = be16(off);
        if (!n || off + *n > b.size()) return std::nullopt;
        auto f = b.subspan(off, *n);
        off += *n;
        return f;
    };
    std::size_t off = 0;
    while (off < b.size()) {
        auto family = be16(off);
        auto addr = field(off);
        auto num = field(off);
        auto name = field(off);
        auto data = field(off);
        if (!family || !addr || !num || !name || !data) break;
        bool host_ok = *family == 65535 || (*family == 256 && as_string(*addr) == host);
        bool num_ok = num->empty() || as_string(*num) == number;
        if (host_ok && num_ok && as_string(*name) == "MIT-MAGIC-COOKIE-1")
            return Cookie{as_string(*name), Bytes(data->begin(), data->end())};
    }
    return std::nullopt;
}

class Client {
public:
    static auto open(const Display& d, const std::optional<Cookie>& cookie) -> Result<Client> {
        auto fd = unix_connect(d.socket);
        if (!fd) return std::unexpected(fd.error());
        Client c(std::move(*fd));
        if (auto r = c.handshake(cookie); !r) return std::unexpected(r.error());
        return c;
    }

    auto intern(std::string_view name) -> Result<std::uint32_t> {
        auto r = request(Req(16, 0).u16(static_cast<std::uint16_t>(name.size())).u16(0).str(name).done());
        if (!r) return std::unexpected(r.error());
        return get32(*r, 8);
    }

    auto atom_name(std::uint32_t atom) -> Result<std::string> {
        auto r = request(Req(17, 0).u32(atom).done());
        if (!r) return std::unexpected(r.error());
        auto n = get16(*r, 8);
        if (r->size() < 32u + n) return fail("short GetAtomName reply");
        return as_string(ByteView(*r).subspan(32, n));
    }

    auto selection_owner(std::uint32_t selection) -> Result<std::uint32_t> {
        auto r = request(Req(23, 0).u32(selection).done());
        if (!r) return std::unexpected(r.error());
        return get32(*r, 8);
    }

    auto create_window() -> Result<std::uint32_t> {
        auto wid = id_base_ | 1;
        // InputOnly window, depth 0, CopyFromParent visual, listening for PropertyNotify.
        auto req = Req(1, 0)
                       .u32(wid).u32(root_)
                       .u16(0).u16(0).u16(1).u16(1).u16(0)
                       .u16(2).u32(0)
                       .u32(0x800).u32(PropertyChangeMask)
                       .done();
        if (auto r = send(req); !r) return std::unexpected(r.error());
        return wid;
    }

    // Asks the owner to convert and waits for SelectionNotify. Returns false if refused.
    auto convert(std::uint32_t win, std::uint32_t sel, std::uint32_t target, std::uint32_t prop) -> Result<bool> {
        if (auto r = send(Req(24, 0).u32(win).u32(sel).u32(target).u32(prop).u32(0).done()); !r)
            return std::unexpected(r.error());
        for (;;) {
            auto ev = event(Millis(3000));
            if (!ev) return fail("clipboard owner did not answer ({})", ev.error().what);
            if ((std::to_integer<std::uint8_t>((*ev)[0]) & 0x7F) == SelectionNotify)
                return get32(*ev, 20) != None;
        }
    }

    struct Property {
        std::uint32_t type;
        std::uint8_t format;
        Bytes data;
    };

    auto get_property(std::uint32_t win, std::uint32_t prop, bool del, std::size_t max) -> Result<Property> {
        Property out{};
        std::uint32_t offset = 0;
        for (;;) {
            // Fetch in 4 MB pieces so one reply never gets huge.
            auto r = request(Req(20, del ? 1 : 0).u32(win).u32(prop).u32(0).u32(offset).u32(1 << 20).done());
            if (!r) return std::unexpected(r.error());
            out.format = std::to_integer<std::uint8_t>((*r)[1]);
            out.type = get32(*r, 8);
            auto after = get32(*r, 12);
            auto units = get32(*r, 16);
            std::size_t bytes = out.format == 0 ? 0 : static_cast<std::size_t>(units) * (out.format / 8);
            if (r->size() < 32 + bytes) return fail("short GetProperty reply");
            if (out.data.size() + bytes > max) return fail("clipboard data is larger than the {} limit", human_size(max));
            out.data.insert(out.data.end(), r->begin() + 32, r->begin() + 32 + static_cast<std::ptrdiff_t>(bytes));
            if (after == 0) return out;
            offset += static_cast<std::uint32_t>(bytes / 4);
        }
    }

    auto delete_property(std::uint32_t win, std::uint32_t prop) -> Result<void> {
        return send(Req(19, 0).u32(win).u32(prop).done());
    }

    auto wait_new_value(std::uint32_t win, std::uint32_t prop) -> Result<void> {
        for (;;) {
            auto ev = event(Millis(5000));
            if (!ev) return fail("clipboard owner stalled during a big transfer ({})", ev.error().what);
            auto code = std::to_integer<std::uint8_t>((*ev)[0]) & 0x7F;
            if (code == PropertyNotify && get32(*ev, 4) == win && get32(*ev, 8) == prop &&
                std::to_integer<std::uint8_t>((*ev)[16]) == 0)
                return {};
        }
    }

private:
    explicit Client(Fd fd) : fd_(std::move(fd)) {}

    auto handshake(const std::optional<Cookie>& cookie) -> Result<void> {
        Bytes out;
        auto push16 = [&](std::uint16_t v) {
            auto p = reinterpret_cast<std::byte*>(&v);
            out.insert(out.end(), p, p + 2);
        };
        out.push_back(std::byte{0x6C});  // 'l' = little-endian
        out.push_back(std::byte{0});
        push16(11);
        push16(0);
        push16(static_cast<std::uint16_t>(cookie ? cookie->name.size() : 0));
        push16(static_cast<std::uint16_t>(cookie ? cookie->data.size() : 0));
        push16(0);
        if (cookie) {
            auto n = as_view(cookie->name);
            out.insert(out.end(), n.begin(), n.end());
            while (out.size() % 4) out.push_back(std::byte{0});
            out.insert(out.end(), cookie->data.begin(), cookie->data.end());
            while (out.size() % 4) out.push_back(std::byte{0});
        }
        if (auto w = write_all(fd_.get(), out, Millis(2000)); !w) return w;

        std::array<std::byte, 8> head;
        if (auto r = read_exact(fd_.get(), head, Millis(3000)); !r) return fail("X server: {}", r.error().what);
        auto extra = static_cast<std::size_t>(get16(head, 6)) * 4;
        Bytes rest(extra);
        if (auto r = read_exact(fd_.get(), rest, Millis(3000)); !r) return fail("X server: {}", r.error().what);
        auto status = std::to_integer<std::uint8_t>(head[0]);
        if (status != 1) {
            auto len = std::to_integer<std::size_t>(head[1]);
            auto reason = as_string(ByteView(rest).first(std::min(len, rest.size())));
            return fail("X server refused connection: {}", reason.empty() ? "authentication needed" : reason);
        }
        if (rest.size() < 32) return fail("short X setup reply");
        id_base_ = get32(rest, 4);
        auto vendor_len = get16(rest, 16);
        auto formats = std::to_integer<std::size_t>(rest[21]);
        auto screen_off = 32 + pad4(vendor_len) + 8 * formats;
        if (rest.size() < screen_off + 4) return fail("short X setup reply");
        root_ = get32(rest, screen_off);
        return {};
    }

    auto send(const Bytes& req) -> Result<void> {
        ++seq_;
        return write_all(fd_.get(), req, Millis(2000));
    }

    // Reads one 32-byte packet plus any reply body.
    auto packet(Millis timeout) -> Result<Bytes> {
        Bytes p(32);
        if (auto r = read_exact(fd_.get(), p, timeout); !r) return std::unexpected(r.error());
        if (std::to_integer<std::uint8_t>(p[0]) == 1) {
            auto extra = static_cast<std::size_t>(get32(p, 4)) * 4;
            if (extra > (256u << 20)) return fail("X reply too large");
            p.resize(32 + extra);
            if (auto r = read_exact(fd_.get(), std::span(p).subspan(32), timeout); !r) return std::unexpected(r.error());
        }
        return p;
    }

    auto request(const Bytes& req) -> Result<Bytes> {
        if (auto r = send(req); !r) return std::unexpected(r.error());
        auto want = static_cast<std::uint16_t>(seq_);
        for (;;) {
            auto p = packet(Millis(3000));
            if (!p) return std::unexpected(p.error());
            auto code = std::to_integer<std::uint8_t>((*p)[0]);
            if (code == 0 && get16(*p, 2) == want)
                return fail("X error {} on request {}", std::to_integer<int>((*p)[1]), std::to_integer<int>((*p)[10]));
            if (code == 1 && get16(*p, 2) == want) return p;
            if (code > 1) events_.push_back(std::move(*p));
        }
    }

    auto event(Millis timeout) -> Result<Bytes> {
        if (!events_.empty()) {
            auto e = std::move(events_.front());
            events_.erase(events_.begin());
            return e;
        }
        for (;;) {
            auto p = packet(timeout);
            if (!p) return std::unexpected(p.error());
            auto code = std::to_integer<std::uint8_t>((*p)[0]);
            if (code == 0)
                return fail("X error {} on request {}", std::to_integer<int>((*p)[1]), std::to_integer<int>((*p)[10]));
            if (code > 1) return p;
        }
    }

    Fd fd_;
    std::uint32_t id_base_ = 0;
    std::uint32_t root_ = 0;
    std::uint32_t seq_ = 0;
    std::vector<Bytes> events_;
};

}  // namespace

auto read(const std::string& display, std::uint64_t max_bytes) -> Result<std::vector<Item>> {
    auto d = parse_display(display);
    if (!d) return std::unexpected(d.error());
    auto cookie = find_cookie(d->number);
    auto c = Client::open(*d, cookie);
    if (!c && cookie) c = Client::open(*d, std::nullopt);  // some servers have no auth
    if (!c) return std::unexpected(c.error());

    auto clipboard = c->intern("CLIPBOARD");
    auto targets = c->intern("TARGETS");
    auto incr = c->intern("INCR");
    auto prop = c->intern("RICHPASTE");
    if (!clipboard || !targets || !incr || !prop) return fail("could not intern X atoms");
    auto owner = c->selection_owner(*clipboard);
    if (!owner) return std::unexpected(owner.error());
    if (*owner == None) return fail("clipboard is empty");
    auto win = c->create_window();
    if (!win) return std::unexpected(win.error());

    auto fetch = [&](std::uint32_t target, std::size_t max) -> Result<Bytes> {
        auto ok = c->convert(*win, *clipboard, target, *prop);
        if (!ok) return std::unexpected(ok.error());
        if (!*ok) return fail("clipboard owner refused the request");
        auto p = c->get_property(*win, *prop, true, max);
        if (!p) return std::unexpected(p.error());
        if (p->type != *incr) return std::move(p->data);
        // INCR: the owner sends chunks; each one arrives as a new property value.
        Bytes all;
        for (;;) {
            if (auto w = c->wait_new_value(*win, *prop); !w) return std::unexpected(w.error());
            auto chunk = c->get_property(*win, *prop, true, max - all.size());
            if (!chunk) return std::unexpected(chunk.error());
            if (chunk->data.empty()) return all;
            all.insert(all.end(), chunk->data.begin(), chunk->data.end());
        }
    };

    auto list = fetch(*targets, 1 << 20);
    if (!list) return fail("reading clipboard formats: {}", list.error().what);
    Offer offer;
    for (std::size_t i = 0; i + 4 <= list->size(); i += 4) {
        auto name = c->atom_name(get32(*list, i));
        if (name) offer.targets.push_back(std::move(*name));
    }
    offer.read = [&](std::string_view target, std::size_t max) -> Result<Bytes> {
        auto atom = c->intern(target);
        if (!atom) return std::unexpected(atom.error());
        return fetch(*atom, max);
    };
    return choose(offer, max_bytes);
}

}  // namespace rp::x11

#endif
