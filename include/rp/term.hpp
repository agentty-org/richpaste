#pragma once

#include "rp/core.hpp"
#include "rp/composer.hpp"
#include "rp/io.hpp"

#include <memory>
#include <optional>

namespace rp::term {

// The controlling terminal in raw mode, with bracketed paste on. Restores on destruction.
// POSIX: /dev/tty (so it works even when stdin/stdout are redirected).
// Windows: the console, with VT input/output enabled.
class Tty {
public:
    static auto open() -> Result<Tty>;
    Tty(Tty&& o) noexcept;
    Tty(const Tty&) = delete;
    auto operator=(Tty&&) -> Tty& = delete;
    ~Tty();

    auto read(std::span<std::byte> out, Millis timeout) -> Result<std::size_t>;
    auto write(std::string_view s) -> Result<void>;
    [[nodiscard]] auto size() const -> std::pair<int, int>;  // cols, rows
    // Raw fds for the in-band clipboard reader (POSIX only; -1 on Windows).
    [[nodiscard]] auto in_fd() const -> int;
    [[nodiscard]] auto out_fd() const -> int;

    struct State;

private:
    explicit Tty(std::unique_ptr<State> s);
    std::unique_ptr<State> s_;
};

namespace key {
struct Char { std::string utf8; };
struct Paste { std::string text; };  // bracketed paste body
struct Ctrl { char letter; };         // Ctrl+A..Z as 'a'..'z'
struct Alt { char letter; };          // Alt+letter as 'a'..'z'
struct Enter {};
struct AltEnter {};
struct Backspace {};
struct Delete {};
struct Left {};
struct Right {};
struct Home {};
struct End {};
struct Escape {};
}  // namespace key

using Key = std::variant<key::Char, key::Paste, key::Ctrl, key::Alt, key::Enter, key::AltEnter, key::Backspace,
                         key::Delete, key::Left, key::Right, key::Home, key::End, key::Escape>;

// Turns raw tty bytes into keys. Keeps partial sequences between feeds.
class Parser {
public:
    void feed(ByteView bytes);
    // Next complete key, if any. Call flush_escape() after a short idle to emit a lone Esc.
    auto next() -> std::optional<Key>;
    auto flush_escape() -> std::optional<Key>;
    [[nodiscard]] auto pending() const -> bool { return !buf_.empty(); }
    [[nodiscard]] auto in_paste() const -> bool { return in_paste_; }

private:
    std::string buf_;
    bool in_paste_ = false;
    std::string paste_;
};

struct View {
    std::string prompt = "> ";
    std::string status;
    int width = 80;
};

// Redraws the box. prev_rows is what the last call returned: rows above the cursor.
auto render(const Composer& c, const View& v, int prev_rows) -> std::pair<std::string, int>;

}  // namespace rp::term
