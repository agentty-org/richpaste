#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rp {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

// What went wrong, for code to act on. `what` is for people; never parse it.
enum class Code : std::uint8_t {
    other = 0,
    empty,        // the clipboard has nothing on it
    too_big,      // over the size limit
    refused,      // the owner said no (terminal EPERM, bad token)
    no_answer,    // asked a terminal, nothing came back in time
    unreachable,  // no agent at that address (dead forward, not running)
    no_display,   // no display server to read here
    corrupt,      // data arrived but failed a check (hash, base64, framing)
    no_route,     // every route was tried; see causes
    needs_terminal,  // only the terminal is left, and the caller gave no session to ask it
};

auto code_name(Code c) -> std::string_view;

// Where a paste came from, or may come from.
enum class Route : std::uint8_t {
    socket,       // the one agent socket set in Config::socket
    forward,      // an ssh-forwarded agent on the machine you type on
    display,      // this machine's own clipboard
    tmux_buffer,  // tmux asks the outer terminal, we read its paste buffer
    terminal,     // we ask the terminal ourselves (OSC 5522 / 52)
};

auto route_name(Route r) -> std::string_view;

struct Error {
    Code code = Code::other;
    std::string what;
    std::vector<Error> causes;    // for no_route: one per route tried
    std::optional<Route> route;   // the route that failed, when paste() knows

    // True if this error or any cause has code c.
    [[nodiscard]] auto any(Code c) const -> bool;
    // The cause from route r, if that route was tried.
    [[nodiscard]] auto from(Route r) const -> const Error*;
};

template <class T>
using Result = std::expected<T, Error>;

template <class... Args>
[[nodiscard]] auto fail(std::format_string<Args...> fmt, Args&&... args) -> std::unexpected<Error> {
    return std::unexpected(Error{Code::other, std::format(fmt, std::forward<Args>(args)...), {}, {}});
}
template <class... Args>
[[nodiscard]] auto fail(Code code, std::format_string<Args...> fmt, Args&&... args) -> std::unexpected<Error> {
    return std::unexpected(Error{code, std::format(fmt, std::forward<Args>(args)...), {}, {}});
}
// Same error with "<prefix>: " in front of the message, code kept.
inline auto context(Error e, std::string_view prefix) -> std::unexpected<Error> {
    e.what = std::format("{}: {}", prefix, e.what);
    return std::unexpected(std::move(e));
}

template <class... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};

inline auto as_view(std::string_view s) -> ByteView { return std::as_bytes(std::span(s)); }
inline auto as_string(ByteView b) -> std::string { return {reinterpret_cast<const char*>(b.data()), b.size()}; }
inline auto to_bytes(std::string_view s) -> Bytes {
    auto v = as_view(s);
    return {v.begin(), v.end()};
}

// True if s is well-formed UTF-8 (no overlongs, no surrogates).
auto valid_utf8(std::string_view s) -> bool;
// Replaces bad sequences with U+FFFD.
auto repair_utf8(std::string_view s) -> std::string;
auto latin1_to_utf8(std::string_view s) -> std::string;

// A MIME type like "image/png" or "text/plain;charset=utf-8". Only parse() makes one.
class Mime {
public:
    static auto parse(std::string_view s) -> Result<Mime>;
    [[nodiscard]] auto str() const -> std::string_view { return s_; }
    [[nodiscard]] auto essence() const -> std::string_view { return str().substr(0, s_.find(';')); }
    [[nodiscard]] auto is_image() const -> bool { return s_.starts_with("image/"); }
    friend auto operator==(const Mime&, const Mime&) -> bool = default;

private:
    explicit Mime(std::string s) : s_(std::move(s)) {}
    std::string s_;
};

// A bare file name: no separators, no "..", no control chars.
class FileName {
public:
    static auto parse(std::string_view s) -> Result<FileName>;
    // Always succeeds: strips directories and replaces anything unsafe.
    static auto sanitize(std::string_view s) -> FileName;
    [[nodiscard]] auto str() const -> std::string_view { return s_; }
    friend auto operator==(const FileName&, const FileName&) -> bool = default;

private:
    explicit FileName(std::string s) : s_(std::move(s)) {}
    std::string s_;
};

// Valid UTF-8 text.
class Text {
public:
    static auto parse(std::string s) -> Result<Text>;
    static auto repair(std::string_view s) -> Text { return Text{repair_utf8(s)}; }
    [[nodiscard]] auto str() const -> const std::string& { return utf8_; }

private:
    explicit Text(std::string s) : utf8_(std::move(s)) {}
    std::string utf8_;
};

struct Image {
    Mime mime;  // always image/*
    Bytes data;
};

struct File {
    FileName name;
    Mime mime;
    Bytes data;
};

// One thing on the clipboard.
using Item = std::variant<Text, Image, File>;

enum class Kind : std::uint8_t { text = 1, image = 2, file = 3 };

inline auto kind_of(const Item& item) -> Kind {
    return std::visit(overloaded{
                          [](const Text&) { return Kind::text; },
                          [](const Image&) { return Kind::image; },
                          [](const File&) { return Kind::file; },
                      },
                      item);
}

inline auto payload(const Item& item) -> ByteView {
    return std::visit(overloaded{
                          [](const Text& t) { return as_view(t.str()); },
                          [](const Image& i) { return ByteView(i.data); },
                          [](const File& f) { return ByteView(f.data); },
                      },
                      item);
}

inline auto mime_of(const Item& item) -> std::string_view {
    return std::visit(overloaded{
                          [](const Text&) { return std::string_view("text/plain;charset=utf-8"); },
                          [](const Image& i) { return i.mime.str(); },
                          [](const File& f) { return f.mime.str(); },
                      },
                      item);
}

inline auto name_of(const Item& item) -> std::string_view {
    if (auto* f = std::get_if<File>(&item)) return f->name.str();
    return {};
}

// Guess a MIME type from a file extension; application/octet-stream if unknown.
auto mime_for_name(std::string_view name) -> Mime;
// Preferred extension for a MIME type, without the dot ("bin" if unknown).
auto ext_for_mime(std::string_view mime) -> std::string_view;

auto human_size(std::uint64_t n) -> std::string;

auto base64_encode(ByteView data) -> std::string;
// Strict RFC 4648: standard alphabet, padding required, no whitespace.
auto base64_decode(std::string_view s) -> Result<Bytes>;

}  // namespace rp
