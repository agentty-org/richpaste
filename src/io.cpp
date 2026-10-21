#include "rp/io.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace rp {

namespace fs = std::filesystem;

void Fd::reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

auto errno_str() -> std::string { return std::strerror(errno); }

namespace {

auto wait_for(int fd, short events, Millis timeout) -> Result<void> {
    pollfd p{.fd = fd, .events = events, .revents = 0};
    for (;;) {
        int r = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (r > 0) return {};
        if (r == 0) return fail("timed out");
        if (errno != EINTR) return fail("poll: {}", errno_str());
    }
}

auto make_addr(const fs::path& path) -> Result<sockaddr_un> {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    auto s = path.native();
    if (s.size() >= sizeof(addr.sun_path)) return fail("socket path too long: {}", s);
    std::memcpy(addr.sun_path, s.c_str(), s.size() + 1);
    return addr;
}

}  // namespace

auto wait_readable(int fd, Millis timeout) -> Result<void> { return wait_for(fd, POLLIN, timeout); }
auto wait_writable(int fd, Millis timeout) -> Result<void> { return wait_for(fd, POLLOUT, timeout); }

auto read_some(int fd, std::span<std::byte> out, Millis timeout) -> Result<std::size_t> {
    for (;;) {
        if (auto w = wait_readable(fd, timeout); !w) return std::unexpected(w.error());
        auto n = ::read(fd, out.data(), out.size());
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno != EINTR && errno != EAGAIN) return fail("read: {}", errno_str());
    }
}

auto read_exact(int fd, std::span<std::byte> out, Millis timeout) -> Result<void> {
    while (!out.empty()) {
        auto n = read_some(fd, out, timeout);
        if (!n) return std::unexpected(n.error());
        if (*n == 0) return fail("connection closed early");
        out = out.subspan(*n);
    }
    return {};
}

auto read_all(int fd, std::size_t max, Millis idle_timeout) -> Result<Bytes> {
    Bytes out;
    std::array<std::byte, 64 * 1024> buf;
    for (;;) {
        auto n = read_some(fd, buf, idle_timeout);
        if (!n) return std::unexpected(n.error());
        if (*n == 0) return out;
        if (out.size() + *n > max) return fail("data is larger than the {} limit", human_size(max));
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    }
}

auto write_all(int fd, ByteView data, Millis timeout) -> Result<void> {
    while (!data.empty()) {
        if (auto w = wait_writable(fd, timeout); !w) return std::unexpected(w.error());
        auto n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == ENOTSOCK) n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return fail("write: {}", errno_str());
        }
        data = data.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

auto unix_listen(const fs::path& path) -> Result<Fd> {
    auto addr = make_addr(path);
    if (!addr) return std::unexpected(addr.error());
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd) return fail("socket: {}", errno_str());
    ::unlink(path.c_str());
    auto old = ::umask(0177);
    int r = ::bind(fd.get(), reinterpret_cast<sockaddr*>(&*addr), sizeof(*addr));
    ::umask(old);
    if (r < 0) return fail("bind {}: {}", path.native(), errno_str());
    if (::listen(fd.get(), 16) < 0) return fail("listen: {}", errno_str());
    return fd;
}

auto unix_connect(const fs::path& path) -> Result<Fd> {
    auto addr = make_addr(path);
    if (!addr) return std::unexpected(addr.error());
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd) return fail("socket: {}", errno_str());
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&*addr), sizeof(*addr)) < 0)
        return fail("connect {}: {}", path.native(), errno_str());
    return fd;
}

auto read_file(const fs::path& path, std::size_t max) -> Result<Bytes> {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) return fail("open {}: {}", path.native(), errno_str());
    struct stat st{};
    if (::fstat(fd.get(), &st) < 0) return fail("stat {}: {}", path.native(), errno_str());
    if (!S_ISREG(st.st_mode)) return fail("{} is not a regular file", path.native());
    if (static_cast<std::uint64_t>(st.st_size) > max)
        return fail("{} is {} (limit {})", path.native(), human_size(st.st_size), human_size(max));
    Bytes out(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < out.size()) {
        auto n = ::read(fd.get(), out.data() + got, out.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return fail("read {}: {}", path.native(), errno_str());
        if (n == 0) break;
        got += static_cast<std::size_t>(n);
    }
    out.resize(got);
    return out;
}

auto write_file_atomic(const fs::path& path, ByteView data) -> Result<void> {
    auto tmp = path;
    tmp += ".tmp" + random_hex(4);
    Fd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!fd) return fail("create {}: {}", tmp.native(), errno_str());
    if (auto w = write_all(fd.get(), data, Millis(5000)); !w) {
        ::unlink(tmp.c_str());
        return w;
    }
    fd.reset();
    if (::rename(tmp.c_str(), path.c_str()) < 0) {
        ::unlink(tmp.c_str());
        return fail("rename to {}: {}", path.native(), errno_str());
    }
    return {};
}

auto env(const char* name) -> std::optional<std::string> {
    const char* v = std::getenv(name);
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

auto passwd_field(int field) -> std::optional<std::string> {
    // Avoids getpwuid so a static binary needs no glibc NSS at runtime.
    auto file = read_file("/etc/passwd", 16 << 20);
    if (!file) return std::nullopt;
    auto uid = std::to_string(::getuid());
    std::string_view all(reinterpret_cast<const char*>(file->data()), file->size());
    while (!all.empty()) {
        auto nl = all.find('\n');
        auto line = all.substr(0, nl);
        all = nl == std::string_view::npos ? std::string_view{} : all.substr(nl + 1);
        std::vector<std::string_view> f;
        for (std::size_t s = 0;;) {
            auto c = line.find(':', s);
            f.push_back(line.substr(s, c == std::string_view::npos ? c : c - s));
            if (c == std::string_view::npos) break;
            s = c + 1;
        }
        if (f.size() >= 7 && f[2] == uid) return std::string(f[static_cast<std::size_t>(field)]);
    }
    return std::nullopt;
}

auto user_name() -> std::string {
    if (auto n = passwd_field(0)) return *n;
    return env("USER").or_else([] { return env("LOGNAME"); }).value_or(std::to_string(::getuid()));
}

auto home_dir() -> fs::path {
    if (auto h = env("HOME")) return *h;
    if (auto h = passwd_field(5)) return *h;
    return "/";
}

auto random_hex(std::size_t bytes) -> std::string {
    Bytes buf(bytes);
    Fd fd(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (fd) {
        auto ok = read_exact(fd.get(), buf, Millis(1000));
        if (ok) {
            std::string out;
            for (auto b : buf) out += std::format("{:02x}", std::to_integer<unsigned>(b));
            return out;
        }
    }
    // /dev/urandom should never fail; this is only a fallback.
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::format("{:0{}x}", static_cast<unsigned long long>(t ^ ::getpid()), bytes * 2).substr(0, bytes * 2);
}

}  // namespace rp
