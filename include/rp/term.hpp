#pragma once

#include "rp/core.hpp"
#include "rp/inband.hpp"
#include "rp/io.hpp"

#include <memory>

namespace rp::term {

// The controlling terminal in raw mode, for asking it for the clipboard
// (OSC 5522 / 52). Restores on destruction.
// POSIX: /dev/tty, so it works even when stdin/stdout are redirected.
// Windows: the console (CONIN$/CONOUT$) with VT input and output on, which
// Windows Terminal, WezTerm and conhost pass through as plain bytes.
class Tty {
public:
    static auto open() -> Result<Tty>;
    Tty(Tty&& o) noexcept;
    Tty(const Tty&) = delete;
    auto operator=(Tty&&) -> Tty& = delete;
    ~Tty();

    auto read(std::span<std::byte> out, Millis timeout) -> Result<std::size_t>;
    auto write(std::string_view s) -> Result<void>;

    // A session that talks through this tty. The Tty must outlive it.
    auto session() -> inband::Session;

    struct State;

private:
    explicit Tty(std::unique_ptr<State> s);
    std::unique_ptr<State> s_;
};

}  // namespace rp::term
