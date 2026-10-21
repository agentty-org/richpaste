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

// Snippets that turn the clipboard into armored text using only what the OS ships.
auto mac_snippet() -> std::string;
// The same, but also pastes into the front window and restores the clipboard.
// Meant for a macOS Shortcut bound to a hotkey: copy, press the hotkey, done.
auto mac_hotkey_script() -> std::string;
auto windows_snippet() -> std::string;
// A self-contained web page that does the same in any browser.
auto html_page() -> std::string;

}  // namespace rp::armor
