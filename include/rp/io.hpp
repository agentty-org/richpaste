#pragma once

#include "rp/core.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>

namespace rp {

using Millis = std::chrono::milliseconds;

// Owns a socket or (on POSIX) any file descriptor. Move-only.
// On Windows it holds a SOCKET; those are small kernel handles that fit in an int.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    auto operator=(Fd&& o) noexcept -> Fd& {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    auto operator=(const Fd&) -> Fd& = delete;
    ~Fd() { reset(); }

    [[nodiscard]] auto get() const -> int { return fd_; }
    [[nodiscard]] explicit operator bool() const { return fd_ >= 0; }
    auto release() -> int { return std::exchange(fd_, -1); }
    void reset();

private:
    int fd_ = -1;
};

auto errno_str() -> std::string;

// Waits until fd is readable (or writable). Fails on timeout.
auto wait_readable(int fd, Millis timeout) -> Result<void>;
auto wait_writable(int fd, Millis timeout) -> Result<void>;

// Reads exactly n bytes, or fails (including on early EOF).
auto read_exact(int fd, std::span<std::byte> out, Millis timeout) -> Result<void>;
// Reads up to out.size() bytes; returns 0 on EOF.
auto read_some(int fd, std::span<std::byte> out, Millis timeout) -> Result<std::size_t>;
// Reads until EOF, failing if more than max bytes arrive.
auto read_all(int fd, std::size_t max, Millis idle_timeout) -> Result<Bytes>;
auto write_all(int fd, ByteView data, Millis timeout) -> Result<void>;

// Unix domain sockets (POSIX only).
auto unix_listen(const std::filesystem::path& path) -> Result<Fd>;
auto unix_connect(const std::filesystem::path& path) -> Result<Fd>;

// Loopback TCP. Port 0 picks a free port; the chosen port is returned.
auto tcp_listen_loopback(std::uint16_t port) -> Result<std::pair<Fd, std::uint16_t>>;
auto tcp_connect_loopback(std::uint16_t port) -> Result<Fd>;
// Accepts one connection, waiting up to timeout.
auto accept_one(int listener, Millis timeout) -> Result<Fd>;
// True if the process on the other end of a loopback TCP connection runs as this user.
// Windows only (POSIX agents use unix sockets, which the OS already restricts).
auto peer_is_same_user(int fd) -> bool;

auto read_file(const std::filesystem::path& path, std::size_t max) -> Result<Bytes>;
auto write_file_atomic(const std::filesystem::path& path, ByteView data) -> Result<void>;
auto ensure_private_dir(const std::filesystem::path& p) -> Result<void>;

// Holds an exclusive lock on a file until destroyed. Fails if someone else holds it.
class FileLock {
public:
    static auto try_lock(const std::filesystem::path& path) -> Result<FileLock>;
    FileLock(FileLock&& o) noexcept : h_(std::exchange(o.h_, -1)) {}
    FileLock(const FileLock&) = delete;
    auto operator=(FileLock&&) -> FileLock& = delete;
    ~FileLock();

private:
    explicit FileLock(std::intptr_t h) : h_(h) {}
    std::intptr_t h_ = -1;
};

auto env(const char* name) -> std::optional<std::string>;
auto home_dir() -> std::filesystem::path;
auto user_name() -> std::string;
auto random_bytes(std::size_t n) -> Bytes;
auto random_hex(std::size_t bytes) -> std::string;
auto hex_of(ByteView b) -> std::string;
auto self_exe() -> std::filesystem::path;
// uid on POSIX; nullopt on Windows.
auto current_uid() -> std::optional<unsigned>;

// Starts exe with args in the background, detached from this terminal.
// stderr goes to log (appended). Returns once it has been launched.
auto spawn_detached(const std::filesystem::path& exe, std::span<const std::string> args,
                    const std::filesystem::path& log) -> Result<void>;

// Runs a command, returns its stdout (trimmed). For tmux queries.
auto run_capture(std::span<const std::string> argv, Millis timeout) -> Result<std::string>;

// Call once at startup: ignores SIGPIPE, starts Winsock, etc.
void platform_init();

}  // namespace rp
