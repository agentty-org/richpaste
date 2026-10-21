#pragma once

#include "rp/core.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>

namespace rp {

using Millis = std::chrono::milliseconds;

// Owns a file descriptor. Move-only.
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

auto unix_listen(const std::filesystem::path& path) -> Result<Fd>;
auto unix_connect(const std::filesystem::path& path) -> Result<Fd>;

auto read_file(const std::filesystem::path& path, std::size_t max) -> Result<Bytes>;
auto write_file_atomic(const std::filesystem::path& path, ByteView data) -> Result<void>;

auto env(const char* name) -> std::optional<std::string>;
auto home_dir() -> std::filesystem::path;
auto user_name() -> std::string;
auto random_hex(std::size_t bytes) -> std::string;

}  // namespace rp
