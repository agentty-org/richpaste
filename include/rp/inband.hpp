#pragma once

// Getting the clipboard from the terminal itself, with nothing installed anywhere.
//
//   OSC 5522 (kitty and terminals that copy it): any mime type, images included.
//   OSC 52 read: text only, from most terminals that allow clipboard reads.
//   tmux: escapes are wrapped in passthrough, or tmux fetches OSC 52 for us.
//
// The caller owns the tty. We write queries and parse replies out of the input stream.

#include "rp/core.hpp"
#include "rp/io.hpp"

#include <optional>

namespace rp::inband {

struct Caps {
    bool osc5522 = false;   // terminal answered DECRQM ?5522 with a supported state
    bool tmux = false;      // we are inside tmux
    bool passthrough = false;  // tmux will pass our wrapped escapes on
    std::string terminal;   // best guess, for messages
};

// One OSC 5522 reply packet, parsed.
struct Packet {
    std::string type;    // "read"
    std::string status;  // OK, DATA, DONE, EPERM, ...
    std::string mime;    // decoded, DATA only
    std::string pw;      // decoded one-time password (paste events)
    Bytes data;          // decoded payload
};

auto parse_5522(std::string_view body) -> Result<Packet>;

// Collects the packets of one read reply into items.
class ReadReply {
public:
    // Feed one packet. Returns true once the reply is complete (DONE or an error).
    auto add(const Packet& p, std::uint64_t max_bytes) -> Result<bool>;
    [[nodiscard]] auto mimes() const -> std::vector<std::string>;  // for a "." listing
    [[nodiscard]] auto items() const -> Result<std::vector<Item>>;

private:
    std::vector<std::pair<std::string, Bytes>> parts_;
    std::uint64_t total_ = 0;
};

// Wraps an escape for tmux passthrough: ESC P tmux; <escape with ESC doubled> ESC \.
auto tmux_wrap(std::string_view esc) -> std::string;

auto query_5522_read(std::span<const std::string> mimes, std::string_view pw = {}) -> std::string;

// Picks which mime types to fetch from a listing. Same order as the native readers.
auto choose_mimes(std::span<const std::string> offered) -> std::vector<std::string>;

// Blocking, self-contained session on a tty fd in raw mode. Used by `richpaste paste`
// and by apps that don't want to run their own parser. Apps with an event loop should
// feed Events from their own parser instead (see term::Parser).
struct Session {
    int in_fd;
    int out_fd;
    Caps caps;
};

auto detect(int in_fd, int out_fd, Millis timeout) -> Caps;
auto read_clipboard(const Session& s, std::uint64_t max_bytes, Millis timeout) -> Result<std::vector<Item>>;

// Asks tmux to fetch the outer terminal's clipboard (OSC 52) and returns the text.
auto tmux_refresh_clipboard(Millis timeout) -> Result<std::string>;

}  // namespace rp::inband
