#pragma once

#include "rp/core.hpp"
#include "rp/composer.hpp"
#include "rp/io.hpp"

#include <optional>

namespace rp::term {

// Puts the tty in raw mode and turns on bracketed paste. Restores on destruction.
class Raw {
public:
    static auto enter(int fd) -> Result<Raw>;
    Raw(Raw&& o) noexcept;
    Raw(const Raw&) = delete;
    auto operator=(Raw&&) -> Raw& = delete;
    ~Raw();

private:
    explicit Raw(int fd);
    int fd_ = -1;
    struct Saved;
    std::unique_ptr<Saved> saved_;
};

auto size(int fd) -> std::pair<int, int>;  // cols, rows

namespace key {
struct Char { std::string utf8; };
struct Paste { std::string text; };  // bracketed paste body
struct Ctrl { char letter; };         // Ctrl+A..Z as 'a'..'z'
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

using Key = std::variant<key::Char, key::Paste, key::Ctrl, key::Enter, key::AltEnter, key::Backspace, key::Delete,
                         key::Left, key::Right, key::Home, key::End, key::Escape>;

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
