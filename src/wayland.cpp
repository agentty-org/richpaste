// Minimal Wayland client that reads the clipboard through data-control.
// Speaks the wire protocol directly over the socket, no libwayland.
//
// ext_data_control_v1 and zwlr_data_control_v1 have the same opcodes, so one
// code path handles both. GNOME has neither; there we fall back to X11.

#include "rp/clipboard.hpp"
#include "rp/io.hpp"

#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/socket.h>
#include <unistd.h>

namespace rp::wayland {

namespace {

constexpr std::uint32_t display_id = 1;

struct Message {
    std::uint32_t object = 0;
    std::uint16_t opcode = 0;
    Bytes args;
};

class ArgReader {
public:
    explicit ArgReader(ByteView b) : b_(b) {}
    auto u32() -> Result<std::uint32_t> {
        if (b_.size() < 4) return fail("short wayland message");
        std::uint32_t v;
        std::memcpy(&v, b_.data(), 4);
        b_ = b_.subspan(4);
        return v;
    }
    auto str() -> Result<std::string> {
        auto n = u32();
        if (!n) return std::unexpected(n.error());
        if (*n == 0) return std::string{};
        auto padded = (static_cast<std::size_t>(*n) + 3) & ~std::size_t{3};
        if (b_.size() < padded) return fail("short wayland string");
        std::string s(reinterpret_cast<const char*>(b_.data()), *n - 1);
        b_ = b_.subspan(padded);
        return s;
    }

private:
    ByteView b_;
};

class ArgWriter {
public:
    void u32(std::uint32_t v) {
        auto p = reinterpret_cast<const std::byte*>(&v);
        out_.insert(out_.end(), p, p + 4);
    }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size() + 1));
        out_.insert(out_.end(), reinterpret_cast<const std::byte*>(s.data()),
                    reinterpret_cast<const std::byte*>(s.data()) + s.size());
        out_.push_back(std::byte{0});
        while (out_.size() % 4) out_.push_back(std::byte{0});
    }
    [[nodiscard]] auto bytes() const -> const Bytes& { return out_; }

private:
    Bytes out_;
};

class Connection {
public:
    explicit Connection(Fd fd) : fd_(std::move(fd)) {}

    auto new_id() -> std::uint32_t { return next_id_++; }

    auto send(std::uint32_t object, std::uint16_t opcode, const ArgWriter& args, int pass_fd = -1) -> Result<void> {
        Bytes msg;
        auto size = static_cast<std::uint32_t>(8 + args.bytes().size());
        std::uint32_t header[2] = {object, (size << 16) | opcode};
        auto hp = reinterpret_cast<const std::byte*>(header);
        msg.insert(msg.end(), hp, hp + 8);
        msg.insert(msg.end(), args.bytes().begin(), args.bytes().end());
        if (pass_fd < 0) return write_all(fd_.get(), msg, Millis(2000));

        iovec iov{.iov_base = msg.data(), .iov_len = msg.size()};
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))] = {};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctrl;
        mh.msg_controllen = sizeof(ctrl);
        auto* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
        for (;;) {
            if (auto w = wait_writable(fd_.get(), Millis(2000)); !w) return w;
            auto n = ::sendmsg(fd_.get(), &mh, MSG_NOSIGNAL);
            if (n == static_cast<ssize_t>(msg.size())) return {};
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n < 0) return fail("sendmsg: {}", errno_str());
            return fail("short sendmsg");
        }
    }

    auto next(Millis timeout) -> Result<Message> {
        for (;;) {
            if (buf_.size() >= 8) {
                std::uint32_t header[2];
                std::memcpy(header, buf_.data(), 8);
                std::size_t size = header[1] >> 16;
                if (size < 8) return fail("bad wayland message size");
                if (buf_.size() >= size) {
                    Message m{header[0], static_cast<std::uint16_t>(header[1] & 0xFFFF),
                              Bytes(buf_.begin() + 8, buf_.begin() + static_cast<std::ptrdiff_t>(size))};
                    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(size));
                    return m;
                }
            }
            if (auto r = fill(timeout); !r) return std::unexpected(r.error());
        }
    }

private:
    auto fill(Millis timeout) -> Result<void> {
        if (auto w = wait_readable(fd_.get(), timeout); !w) return fail("compositor did not answer ({})", w.error().what);
        std::array<std::byte, 8192> data;
        iovec iov{.iov_base = data.data(), .iov_len = data.size()};
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 28)];
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctrl;
        mh.msg_controllen = sizeof(ctrl);
        auto n = ::recvmsg(fd_.get(), &mh, MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) return {};
        if (n < 0) return fail("recvmsg: {}", errno_str());
        if (n == 0) return fail("compositor closed the connection");
        // We never expect fds; close any we get so they don't leak.
        for (auto* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count; ++i) {
                int fd;
                std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                ::close(fd);
            }
        }
        buf_.insert(buf_.end(), data.begin(), data.begin() + n);
        return {};
    }

    Fd fd_;
    Bytes buf_;
    std::uint32_t next_id_ = 2;
};

struct Global {
    std::uint32_t name;
    std::string interface;
    std::uint32_t version;
};

struct Session {
    explicit Session(Fd fd) : conn(std::move(fd)) {}

    Connection conn;
    std::uint32_t registry = 0;
    std::vector<Global> globals;
    std::uint32_t device = 0;
    std::map<std::uint32_t, std::vector<std::string>> offers;  // offer id -> mime types
    std::optional<std::uint32_t> selection;                    // nullopt until told; 0 = empty

    // Handles events we always care about. Returns an error for protocol errors.
    auto dispatch(const Message& m) -> Result<void> {
        ArgReader a(m.args);
        if (m.object == display_id && m.opcode == 0) {
            auto obj = a.u32();
            auto code = a.u32();
            auto msg = a.str();
            return fail("compositor error on object {} code {}: {}", obj.value_or(0), code.value_or(0),
                        msg.value_or("?"));
        }
        if (m.object == registry && m.opcode == 0) {
            auto name = a.u32();
            auto iface = a.str();
            auto ver = a.u32();
            if (name && iface && ver) globals.push_back({*name, *iface, *ver});
            return {};
        }
        if (device != 0 && m.object == device) {
            if (m.opcode == 0) {  // data_offer(new_id)
                if (auto id = a.u32()) offers[*id] = {};
            } else if (m.opcode == 1) {  // selection(offer or null)
                if (auto id = a.u32()) selection = *id;
            } else if (m.opcode == 2) {  // finished
                return fail("data-control device was cancelled by the compositor");
            }
            return {};
        }
        if (auto it = offers.find(m.object); it != offers.end() && m.opcode == 0) {
            if (auto mime = a.str()) it->second.push_back(*mime);
        }
        return {};
    }

    auto roundtrip() -> Result<void> {
        auto cb = conn.new_id();
        ArgWriter w;
        w.u32(cb);
        if (auto s = conn.send(display_id, 0, w); !s) return s;
        for (;;) {
            auto m = conn.next(Millis(3000));
            if (!m) return std::unexpected(m.error());
            if (m->object == cb && m->opcode == 0) return {};
            if (auto d = dispatch(*m); !d) return d;
        }
    }

    auto find(std::string_view iface) const -> const Global* {
        for (auto& g : globals)
            if (g.interface == iface) return &g;
        return nullptr;
    }

    auto bind(const Global& g, std::uint32_t version) -> Result<std::uint32_t> {
        auto id = conn.new_id();
        ArgWriter w;
        w.u32(g.name);
        w.str(g.interface);
        w.u32(version);
        w.u32(id);
        if (auto s = conn.send(registry, 0, w); !s) return std::unexpected(s.error());
        return id;
    }

    auto receive(std::uint32_t offer, std::string_view mime, std::size_t max) -> Result<Bytes> {
        int p[2];
        if (::pipe2(p, O_CLOEXEC) < 0) return fail("pipe: {}", errno_str());
        Fd rd(p[0]), wr(p[1]);
        ArgWriter w;
        w.str(mime);
        if (auto s = conn.send(offer, 0, w, wr.get()); !s) return std::unexpected(s.error());
        wr.reset();
        // The copying app writes into the pipe and closes it when done.
        auto data = read_all(rd.get(), max, Millis(5000));
        if (!data) return fail("reading '{}' from the copying app: {}", mime, data.error().what);
        return data;
    }
};

}  // namespace

auto read(const std::string& socket_path, std::uint64_t max_bytes) -> Result<std::vector<Item>> {
    auto fd = unix_connect(socket_path);
    if (!fd) return std::unexpected(fd.error());
    Session s(std::move(*fd));

    s.registry = s.conn.new_id();
    ArgWriter w;
    w.u32(s.registry);
    if (auto r = s.conn.send(display_id, 1, w); !r) return std::unexpected(r.error());
    if (auto r = s.roundtrip(); !r) return std::unexpected(r.error());

    const Global* mgr = s.find("ext_data_control_manager_v1");
    if (!mgr) mgr = s.find("zwlr_data_control_manager_v1");
    if (!mgr) return fail("compositor has no data-control protocol (GNOME?), trying X11");
    const Global* seat = s.find("wl_seat");
    if (!seat) return fail("compositor has no seat");

    auto seat_id = s.bind(*seat, 1);
    if (!seat_id) return std::unexpected(seat_id.error());
    auto mgr_id = s.bind(*mgr, 1);
    if (!mgr_id) return std::unexpected(mgr_id.error());

    s.device = s.conn.new_id();
    ArgWriter dw;
    dw.u32(s.device);
    dw.u32(*seat_id);
    if (auto r = s.conn.send(*mgr_id, 1, dw); !r) return std::unexpected(r.error());

    // The compositor sends the current selection right after the device is made.
    for (int i = 0; i < 3 && !s.selection; ++i)
        if (auto r = s.roundtrip(); !r) return std::unexpected(r.error());
    if (!s.selection) return fail("compositor never sent the selection");
    if (*s.selection == 0) return fail("clipboard is empty");

    auto it = s.offers.find(*s.selection);
    if (it == s.offers.end()) return fail("compositor sent an unknown selection offer");
    auto offer_id = it->first;
    Offer offer{it->second, [&](std::string_view mime, std::size_t max) { return s.receive(offer_id, mime, max); }};
    return choose(offer, max_bytes);
}

}  // namespace rp::wayland
