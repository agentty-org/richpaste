#pragma once

// Text armor: a whole file or image as one line of plain text, so it can travel
// through any terminal paste, ssh and tmux.
//
//   richpaste:1:<mime>:<name, base64>:<size>:<sha256 hex or empty>:<base64 data>
//
// One line, no newline, so pasting it into a shell by mistake runs nothing.
// The decoder skips whitespace inside the data and stops after <size> bytes,
// so wrapping or CR/LF changes by terminals don't break it.

#include "rp/core.hpp"

namespace rp::armor {

inline constexpr std::string_view magic = "richpaste:1:";

auto encode(const Item& item) -> std::string;
auto contains(std::string_view text) -> bool;

struct Split {
    std::vector<std::variant<std::string, Item>> parts;  // text and items, in order
    std::vector<std::string> errors;                      // one per bad block
};

// Pulls every armored block out of pasted text. Plain text around them is kept.
auto split(std::string_view text) -> Split;

// How much of the armored text has arrived, for text that comes in pieces
// (a paste read by read, a slow link). Cheap: no decoding.
struct Progress {
    enum class State : std::uint8_t {
        none,      // no armored block in the text
        partial,   // a block is still arriving; wait for more
        complete,  // every block has all its data (or is broken): split() now gives the final answer
    };
    State state = State::none;
    // The first unfinished block (arriving, or cut short): bytes of it here so
    // far, and its full size. need is 0 when there's none, or its header is cut.
    std::uint64_t have = 0;
    std::uint64_t need = 0;
};
auto progress(std::string_view text) -> Progress;

// Replaces any Text item that carries armored blocks with the items inside it.
// Every paste route runs this, so an image copied as text arrives as the image.
auto expand(std::vector<Item> items, std::vector<std::string>* errors = nullptr) -> std::vector<Item>;

// Snippets that turn the clipboard into armored text using only what the OS ships.
auto mac_snippet() -> std::string;
// The same, but also pastes into the front window and restores the clipboard.
// Meant for a macOS Shortcut bound to a hotkey: copy, press the hotkey, done.
auto mac_hotkey_script() -> std::string;
auto windows_snippet() -> std::string;
// A self-contained web page that does the same in any browser.
auto html_page() -> std::string;

}  // namespace rp::armor
