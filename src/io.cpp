// Platform-neutral IO helpers. The OS-specific parts are in io_posix.cpp and io_win.cpp.

#include "rp/io.hpp"

#include <array>

namespace rp {

namespace fs = std::filesystem;

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

auto random_hex(std::size_t bytes) -> std::string { return hex_of(random_bytes(bytes)); }

auto hex_of(ByteView b) -> std::string {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (auto x : b) {
        out += digits[std::to_integer<unsigned>(x) >> 4];
        out += digits[std::to_integer<unsigned>(x) & 15];
    }
    return out;
}

auto passwd_field(int field) -> std::optional<std::string> {
    // Avoids getpwuid so static agentty builds need no glibc NSS at runtime.
    auto file = read_file("/etc/passwd", 16 << 20);
    if (!file) return std::nullopt;
    auto uid = current_uid();
    if (!uid) return std::nullopt;
    auto want = std::to_string(*uid);
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
        if (f.size() >= 7 && f[2] == want) return std::string(f[static_cast<std::size_t>(field)]);
    }
    return std::nullopt;
}

auto user_name() -> std::string {
    if (auto n = passwd_field(0)) return *n;
    for (auto v : {"USER", "LOGNAME", "USERNAME"})
        if (auto e = env(v)) return *e;
    return "user";
}

auto home_dir() -> fs::path {
    if (auto h = env("HOME")) return *h;
    if (auto h = env("USERPROFILE")) return *h;
    if (auto h = passwd_field(5)) return *h;
    return ".";
}

}  // namespace rp
