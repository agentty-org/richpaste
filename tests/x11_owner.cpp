// Test-only X11 clipboard owner. Serves one file as a given mime type,
// using INCR (chunked) when it's bigger than one request can carry.
//   x11_owner DISPLAYNUM MIME FILE

#include "rp/io.hpp"

#include <cstring>
#include <print>

using namespace rp;

namespace {

Fd conn;
std::uint32_t id_base = 0, root = 0;

auto g32(ByteView b, std::size_t o) -> std::uint32_t {
    std::uint32_t v;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}

struct Req {
    Bytes b;
    Req(std::uint8_t op, std::uint8_t d) { b = {std::byte{op}, std::byte{d}, {}, {}}; }
    auto u32(std::uint32_t v) -> Req& { auto p = reinterpret_cast<std::byte*>(&v); b.insert(b.end(), p, p + 4); return *this; }
    auto u16(std::uint16_t v) -> Req& { auto p = reinterpret_cast<std::byte*>(&v); b.insert(b.end(), p, p + 2); return *this; }
    auto u8(std::uint8_t v) -> Req& { b.push_back(std::byte{v}); return *this; }
    auto raw(ByteView d) -> Req& { b.insert(b.end(), d.begin(), d.end()); while (b.size() % 4) b.push_back({}); return *this; }
    void send() {
        auto n = static_cast<std::uint16_t>(b.size() / 4);
        std::memcpy(b.data() + 2, &n, 2);
        (void)write_all(conn.get(), b, Millis(5000));
    }
};

auto packet() -> Bytes {
    Bytes p(32);
    if (!read_exact(conn.get(), p, Millis(600000))) std::exit(1);
    if (std::to_integer<int>(p[0]) == 1) {
        p.resize(32 + g32(p, 4) * 4);
        (void)read_exact(conn.get(), std::span(p).subspan(32), Millis(5000));
    }
    return p;
}

std::vector<Bytes> queued;

auto intern(std::string_view s) -> std::uint32_t {
    Req(16, 0).u16(static_cast<std::uint16_t>(s.size())).u16(0).raw(as_view(s)).send();
    for (;;) {
        auto p = packet();
        if (std::to_integer<int>(p[0]) == 1) return g32(p, 8);
        queued.push_back(std::move(p));
    }
}

auto next_event() -> Bytes {
    if (!queued.empty()) {
        auto e = std::move(queued.front());
        queued.erase(queued.begin());
        return e;
    }
    return packet();
}

void notify(std::uint32_t requestor, std::uint32_t sel, std::uint32_t target, std::uint32_t prop) {
    Req r(25, 0);
    r.u32(requestor).u32(0);
    Bytes ev(32);
    ev[0] = std::byte{31};
    std::uint32_t f[4] = {requestor, sel, target, prop};
    std::memcpy(ev.data() + 8, f, 16);
    r.raw(ev).send();
}

void change(std::uint32_t win, std::uint32_t prop, std::uint32_t type, std::uint8_t fmt, ByteView data) {
    Req(18, 0).u32(win).u32(prop).u32(type).u8(fmt).u8(0).u16(0)
        .u32(static_cast<std::uint32_t>(data.size() / (fmt / 8))).raw(data).send();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    auto data = read_file(argv[3], 1u << 30);
    if (!data) return 1;
    auto fd = unix_connect(std::string("/tmp/.X11-unix/X") + argv[1]);
    if (!fd) return 1;
    conn = std::move(*fd);
    Bytes setup = {std::byte{0x6C}, {}, std::byte{11}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    (void)write_all(conn.get(), setup, Millis(2000));
    Bytes head(8);
    (void)read_exact(conn.get(), head, Millis(2000));
    std::uint16_t extra;
    std::memcpy(&extra, head.data() + 6, 2);
    Bytes rest(extra * 4u);
    (void)read_exact(conn.get(), rest, Millis(2000));
    id_base = g32(rest, 4);
    std::uint16_t vlen;
    std::memcpy(&vlen, rest.data() + 16, 2);
    root = g32(rest, 32 + ((vlen + 3u) & ~3u) + 8 * std::to_integer<unsigned>(rest[21]));

    auto win = id_base | 1;
    Req(1, 0).u32(win).u32(root).u16(0).u16(0).u16(1).u16(1).u16(0).u16(2).u32(0).u32(0).send();
    auto clipboard = intern("CLIPBOARD"), targets = intern("TARGETS"), incr = intern("INCR"), mime = intern(argv[2]);
    Req(22, 0).u32(win).u32(clipboard).u32(0).send();
    std::println("owner ready");
    std::fflush(stdout);

    constexpr std::size_t chunk = 64 * 1024;
    for (;;) {
        auto ev = next_event();
        if ((std::to_integer<int>(ev[0]) & 0x7F) != 30) continue;  // SelectionRequest
        auto requestor = g32(ev, 12), sel = g32(ev, 16), target = g32(ev, 20), prop = g32(ev, 24);
        if (target == targets) {
            std::uint32_t list[2] = {targets, mime};
            change(requestor, prop, 4, 32, std::as_bytes(std::span(list)));
            notify(requestor, sel, target, prop);
        } else if (target == mime && data->size() <= chunk) {
            change(requestor, prop, mime, 8, *data);
            notify(requestor, sel, target, prop);
        } else if (target == mime) {
            Req(2, 0).u32(requestor).u32(0x800).u32(0x400000).send();  // watch requestor's properties
            auto size = static_cast<std::uint32_t>(data->size());
            change(requestor, prop, incr, 32, std::as_bytes(std::span(&size, 1)));
            notify(requestor, sel, target, prop);
            std::size_t off = 0;
            for (bool done = false; !done;) {
                auto e = next_event();
                // PropertyNotify, state Deleted, on our property: send the next chunk.
                if ((std::to_integer<int>(e[0]) & 0x7F) != 28 || g32(e, 4) != requestor || g32(e, 8) != prop ||
                    std::to_integer<int>(e[16]) != 1)
                    continue;
                auto n = std::min(chunk, data->size() - off);
                change(requestor, prop, mime, 8, std::span(*data).subspan(off, n));
                off += n;
                done = n == 0;
            }
            Req(2, 0).u32(requestor).u32(0x800).u32(0).send();
        } else {
            notify(requestor, sel, target, 0);
        }
    }
}
