#pragma once

#include "rp/core.hpp"

#include <filesystem>

namespace rp {

// A pasted thing that lives in the box as one unit.
struct Chip {
    Kind kind;
    std::string mime;
    std::string label;             // what the user sees, e.g. "image png 120 KB"
    std::filesystem::path path;    // where the bytes were saved
    std::uint64_t size;
    std::string sha256;
};

// One grapheme-ish unit of text (a code point plus any combining marks).
struct Glyph {
    std::string utf8;
};

using Cell = std::variant<Glyph, Chip>;

// The input box contents. No terminal code in here.
class Composer {
public:
    void insert_text(std::string_view utf8);
    void insert_chip(Chip chip);
    void backspace();
    void del();
    void left() { if (cursor_ > 0) --cursor_; }
    void right() { if (cursor_ < cells_.size()) ++cursor_; }
    void home();
    void end();
    void word_left();
    void kill_line_before();
    void clear();

    [[nodiscard]] auto cells() const -> const std::vector<Cell>& { return cells_; }
    [[nodiscard]] auto cursor() const -> std::size_t { return cursor_; }
    [[nodiscard]] auto empty() const -> bool { return cells_.empty(); }
    [[nodiscard]] auto chips() const -> std::vector<Chip>;

    // Plain text with each chip written as its saved path.
    [[nodiscard]] auto to_text() const -> std::string;

private:
    std::vector<Cell> cells_;
    std::size_t cursor_ = 0;
};

// Columns a code point takes in a terminal (0, 1 or 2).
auto cell_width(std::string_view glyph) -> int;
// Splits UTF-8 into glyphs, attaching combining marks to the base.
auto split_glyphs(std::string_view utf8) -> std::vector<std::string>;

// Saves items under dir and turns them into chips. Text items come back as text.
struct Pasted {
    std::vector<std::variant<std::string, Chip>> parts;
};
auto store(std::span<const Item> items, const std::filesystem::path& dir) -> Result<Pasted>;

}  // namespace rp
