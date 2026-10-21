// POSIX (Linux, macOS, BSD) side of the IO helpers.
#ifndef _WIN32

#include "rp/io.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SO_NOSIGPIPE / SIG_IGN covers it
#endif

namespace rp {

namespace fs = std::filesystem;

void Fd::reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

auto errno_str() -> std::string { return std::strerror(errno); }

void platform_init() { std::signal(SIGPIPE, SIG_IGN); }

namespace {

auto wait_for(int fd, short events, Millis timeout) -> Result<void> {
    pollfd p{.fd = fd, .events = events, .revents = 0};
    for (;;) {
        int r = ::poll(&p, 1, static_cast<int>(std::min<long long>(timeout.count(), 1 << 30)));
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

void cloexec(int fd) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }

auto new_socket(int family) -> Result<Fd> {
    Fd fd(::socket(family, SOCK_STREAM, 0));
    if (!fd) return fail("socket: {}", errno_str());
    cloexec(fd.get());
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
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
    auto fd = new_socket(AF_UNIX);
    if (!fd) return fd;
    ::unlink(path.c_str());
    auto old = ::umask(0177);
    int r = ::bind(fd->get(), reinterpret_cast<sockaddr*>(&*addr), sizeof(*addr));
    ::umask(old);
    if (r < 0) return fail("bind {}: {}", path.native(), errno_str());
    if (::listen(fd->get(), 16) < 0) return fail("listen: {}", errno_str());
    return fd;
}

auto unix_connect(const fs::path& path) -> Result<Fd> {
    auto addr = make_addr(path);
    if (!addr) return std::unexpected(addr.error());
    auto fd = new_socket(AF_UNIX);
    if (!fd) return fd;
    if (::connect(fd->get(), reinterpret_cast<sockaddr*>(&*addr), sizeof(*addr)) < 0)
        return fail("connect {}: {}", path.native(), errno_str());
    return fd;
}

auto tcp_listen_loopback(std::uint16_t port) -> Result<std::pair<Fd, std::uint16_t>> {
    auto fd = new_socket(AF_INET);
    if (!fd) return std::unexpected(fd.error());
    int one = 1;
    ::setsockopt(fd->get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd->get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0)
        return fail("bind 127.0.0.1:{}: {}", port, errno_str());
    if (::listen(fd->get(), 16) < 0) return fail("listen: {}", errno_str());
    socklen_t len = sizeof(a);
    ::getsockname(fd->get(), reinterpret_cast<sockaddr*>(&a), &len);
    return std::pair{std::move(*fd), ntohs(a.sin_port)};
}

auto tcp_connect_loopback(std::uint16_t port) -> Result<Fd> {
    auto fd = new_socket(AF_INET);
    if (!fd) return fd;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd->get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0)
        return fail("connect 127.0.0.1:{}: {}", port, errno_str());
    return fd;
}

auto accept_one(int listener, Millis timeout) -> Result<Fd> {
    if (auto w = wait_readable(listener, timeout); !w) return std::unexpected(w.error());
    Fd c(::accept(listener, nullptr, nullptr));
    if (!c) return fail("accept: {}", errno_str());
    cloexec(c.get());
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(c.get(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return c;
}

auto peer_is_same_user(int) -> bool { return false; }

auto read_file(const fs::path& path, std::size_t max) -> Result<Bytes> {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) return fail("open {}: {}", path.native(), errno_str());
    struct stat st{};
    if (::fstat(fd.get(), &st) < 0) return fail("stat {}: {}", path.native(), errno_str());
    if (!S_ISREG(st.st_mode)) return fail("{} is not a regular file", path.native());
    if (static_cast<std::uint64_t>(st.st_size) > max)
        return fail("{} is {} (limit {})", path.native(), human_size(static_cast<std::uint64_t>(st.st_size)), human_size(max));
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
    for (auto rest = data; !rest.empty();) {
        auto n = ::write(fd.get(), rest.data(), rest.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            ::unlink(tmp.c_str());
            return fail("write {}: {}", tmp.native(), errno_str());
        }
        rest = rest.subspan(static_cast<std::size_t>(n));
    }
    fd.reset();
    if (::rename(tmp.c_str(), path.c_str()) < 0) {
        ::unlink(tmp.c_str());
        return fail("rename to {}: {}", path.native(), errno_str());
    }
    return {};
}

auto ensure_private_dir(const fs::path& p) -> Result<void> {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) return fail("create {}: {}", p.string(), ec.message());
    ::chmod(p.c_str(), 0700);
    return {};
}

auto FileLock::try_lock(const fs::path& path) -> Result<FileLock> {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return fail("open {}: {}", path.string(), errno_str());
    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        ::close(fd);
        return fail("another agent is already running");
    }
    return FileLock(fd);
}

FileLock::~FileLock() {
    if (h_ >= 0) ::close(static_cast<int>(h_));
}

auto env(const char* name) -> std::optional<std::string> {
    const char* v = std::getenv(name);
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

auto current_uid() -> std::optional<unsigned> { return static_cast<unsigned>(::getuid()); }

auto random_bytes(std::size_t n) -> Bytes {
    Bytes buf(n);
    Fd fd(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (fd && read_exact(fd.get(), buf, Millis(1000))) return buf;
    // /dev/urandom should never fail. Don't silently hand out weak tokens.
    std::abort();
}

auto self_exe() -> fs::path {
#if defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = fs::canonical(buf, ec);
        return ec ? fs::path(buf) : p;
    }
    return "richpaste";
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path("richpaste") : p;
#endif
}

auto spawn_detached(const fs::path& exe, std::span<const std::string> args, const fs::path& log) -> Result<void> {
    std::vector<std::string> argv{exe.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    auto log_s = log.string();

    pid_t pid = ::fork();
    if (pid < 0) return fail("fork: {}", errno_str());
    if (pid == 0) {
        ::setsid();
        if (::fork() != 0) ::_exit(0);
        int null = ::open("/dev/null", O_RDWR);
        ::dup2(null, 0);
        ::dup2(null, 1);
        int lf = ::open(log_s.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        ::dup2(lf >= 0 ? lf : null, 2);
        for (int fd = 3; fd < 1024; ++fd) ::close(fd);
        ::execv(cargv[0], cargv.data());
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return {};
}

auto run_capture(std::span<const std::string> args, Millis timeout) -> Result<std::string> {
    if (args.empty()) return fail("empty command");
    int p[2];
    if (::pipe(p) < 0) return fail("pipe: {}", errno_str());
    Fd rd(p[0]), wr(p[1]);
    cloexec(rd.get());
    std::vector<std::string> copy(args.begin(), args.end());
    std::vector<char*> cargv;
    for (auto& a : copy) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) return fail("fork: {}", errno_str());
    if (pid == 0) {
        ::dup2(wr.get(), 1);
        int null = ::open("/dev/null", O_RDWR);
        ::dup2(null, 0);
        ::dup2(null, 2);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);
    }
    wr.reset();
    auto out = read_all(rd.get(), 1 << 20, timeout);
    int status = 0;
    if (!out) ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    if (!out) return std::unexpected(out.error());
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return fail("{} failed", args[0]);
    auto s = as_string(*out);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

}  // namespace rp

#endif
