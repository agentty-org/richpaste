#include "rp/composer.hpp"

#include "rp/io.hpp"
#include "rp/sha256.hpp"

#include <algorithm>

namespace rp {

namespace fs = std::filesystem;

namespace {

auto decode(std::string_view g) -> char32_t {
    if (g.empty()) return 0;
    auto b = [&](std::size_t i) { return static_cast<unsigned char>(g[i]); };
    if (b(0) < 0x80) return b(0);
    if (g.size() >= 2 && b(0) < 0xE0) return ((b(0) & 0x1F) << 6) | (b(1) & 0x3F);
    if (g.size() >= 3 && b(0) < 0xF0) return ((b(0) & 0x0F) << 12) | ((b(1) & 0x3F) << 6) | (b(2) & 0x3F);
    if (g.size() >= 4) return ((b(0) & 0x07) << 18) | ((b(1) & 0x3F) << 12) | ((b(2) & 0x3F) << 6) | (b(3) & 0x3F);
    return 0xFFFD;
}

auto in(char32_t c, std::initializer_list<std::pair<char32_t, char32_t>> ranges) -> bool {
    return std::ranges::any_of(ranges, [c](auto r) { return c >= r.first && c <= r.second; });
}

auto is_combining(char32_t c) -> bool {
    return in(c, {{0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F},
                  {0x0900, 0x0903}, {0x093A, 0x094F}, {0x0951, 0x0957}, {0x0962, 0x0963}, {0x1AB0, 0x1AFF},
                  {0x1DC0, 0x1DFF}, {0x200C, 0x200D}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
                  {0x1F3FB, 0x1F3FF}, {0xE0100, 0xE01EF}});
}

auto is_wide(char32_t c) -> bool {
    return in(c, {{0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
                  {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x26A1, 0x26A1},
                  {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
                  {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
                  {0x2728, 0x2728}, {0x274C, 0x274C}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
                  {0x27B0, 0x27B0}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2E80, 0x303E}, {0x3041, 0x33FF},
                  {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
                  {0xFE30, 0xFE4F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
                  {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD}});
}

}  // namespace

auto cell_width(std::string_view glyph) -> int {
    auto c = decode(glyph);
    if (c == '\t') return 4;
    if (c < 0x20 || c == 0x7F) return 0;
    return is_wide(c) ? 2 : 1;
}

auto split_glyphs(std::string_view utf8) -> std::vector<std::string> {
    std::vector<std::string> out;
    // CRLF and lone CR both become LF.
    std::string s;
    {
        auto src = repair_utf8(utf8);
        s.reserve(src.size());
        for (std::size_t i = 0; i < src.size(); ++i) {
            if (src[i] == '\r') {
                s += '\n';
                if (i + 1 < src.size() && src[i + 1] == '\n') ++i;
            } else {
                s += src[i];
            }
        }
    }
    bool join_next = false;
    for (std::size_t i = 0; i < s.size();) {
        auto c0 = static_cast<unsigned char>(s[i]);
        std::size_t n = c0 < 0x80 ? 1 : c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : 4;
        auto g = std::string_view(s).substr(i, n);
        auto cp = decode(g);
        i += n;
        if (!out.empty() && (is_combining(cp) || join_next) && out.back() != "\n") {
            out.back() += g;
        } else {
            out.emplace_back(g);
        }
        join_next = cp == 0x200D;  // zero-width joiner glues emoji sequences
    }
    return out;
}

void Composer::insert_text(std::string_view utf8) {
    auto glyphs = split_glyphs(utf8);
    std::vector<Cell> add;
    add.reserve(glyphs.size());
    for (auto& g : glyphs) {
        auto cp = decode(g);
        // Drop control characters except newline and tab; they'd corrupt the screen.
        if ((cp < 0x20 && cp != '\n' && cp != '\t') || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) continue;
        add.push_back(Glyph{std::move(g)});
    }
    cells_.insert(cells_.begin() + static_cast<std::ptrdiff_t>(cursor_), add.begin(), add.end());
    cursor_ += add.size();
}

void Composer::insert_chip(Chip chip) {
    cells_.insert(cells_.begin() + static_cast<std::ptrdiff_t>(cursor_), std::move(chip));
    ++cursor_;
}

void Composer::backspace() {
    if (cursor_ == 0) return;
    cells_.erase(cells_.begin() + static_cast<std::ptrdiff_t>(--cursor_));
}

void Composer::del() {
    if (cursor_ < cells_.size()) cells_.erase(cells_.begin() + static_cast<std::ptrdiff_t>(cursor_));
}

namespace {
auto is_newline(const Cell& c) -> bool {
    auto* g = std::get_if<Glyph>(&c);
    return g && g->utf8 == "\n";
}
auto is_space(const Cell& c) -> bool {
    auto* g = std::get_if<Glyph>(&c);
    return g && (g->utf8 == " " || g->utf8 == "\n" || g->utf8 == "\t");
}
}  // namespace

void Composer::home() {
    while (cursor_ > 0 && !is_newline(cells_[cursor_ - 1])) --cursor_;
}

void Composer::end() {
    while (cursor_ < cells_.size() && !is_newline(cells_[cursor_])) ++cursor_;
}

void Composer::word_left() {
    auto start = cursor_;
    while (start > 0 && is_space(cells_[start - 1])) --start;
    while (start > 0 && !is_space(cells_[start - 1])) --start;
    cells_.erase(cells_.begin() + static_cast<std::ptrdiff_t>(start), cells_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ = start;
}

void Composer::kill_line_before() {
    auto start = cursor_;
    while (start > 0 && !is_newline(cells_[start - 1])) --start;
    cells_.erase(cells_.begin() + static_cast<std::ptrdiff_t>(start), cells_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ = start;
}

void Composer::clear() {
    cells_.clear();
    cursor_ = 0;
}

auto Composer::chips() const -> std::vector<Chip> {
    std::vector<Chip> out;
    for (auto& c : cells_)
        if (auto* chip = std::get_if<Chip>(&c)) out.push_back(*chip);
    return out;
}

auto Composer::to_text() const -> std::string {
    std::string out;
    for (auto& c : cells_)
        std::visit(overloaded{
                       [&](const Glyph& g) { out += g.utf8; },
                       [&](const Chip& chip) { out += chip.path.string(); },
                   },
                   c);
    return out;
}

auto store(std::span<const Item> items, const fs::path& dir) -> Result<Pasted> {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return fail("create {}: {}", dir.string(), ec.message());
    Pasted out;
    for (auto& item : items) {
        if (auto* t = std::get_if<Text>(&item)) {
            out.parts.emplace_back(t->str());
            continue;
        }
        auto data = payload(item);
        auto digest = hex(sha256(data));
        auto mime = std::string(mime_of(item));
        auto sub = std::string_view(mime).substr(mime.find('/') + 1);
        sub = sub.substr(0, sub.find(';'));
        std::string name;
        std::string label;
        if (auto* f = std::get_if<File>(&item)) {
            name = digest.substr(0, 12) + "-" + std::string(f->name.str());
            label = std::format("{} {}", f->name.str(), human_size(data.size()));
        } else {
            name = std::format("{}.{}", digest.substr(0, 16), ext_for_mime(mime));
            label = std::format("image {} {}", sub, human_size(data.size()));
        }
        auto path = dir / name;
        // Same hash means same bytes: skip the write.
        if (!fs::exists(path, ec))
            if (auto w = write_file_atomic(path, data); !w) return std::unexpected(w.error());
        out.parts.emplace_back(Chip{kind_of(item), mime, label, path, data.size(), digest});
    }
    return out;
}

}  // namespace rp
