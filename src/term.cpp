#include "rp/term.hpp"

#include <memory>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace rp::term {

struct Raw::Saved {
    termios t;
};

Raw::Raw(int fd) : fd_(fd), saved_(std::make_unique<Saved>()) {}

Raw::Raw(Raw&& o) noexcept : fd_(std::exchange(o.fd_, -1)), saved_(std::move(o.saved_)) {}

auto Raw::enter(int fd) -> Result<Raw> {
    if (!::isatty(fd)) return fail("not a terminal");
    Raw r(fd);
    if (::tcgetattr(fd, &r.saved_->t) < 0) return fail("tcgetattr: {}", errno_str());
    termios t = r.saved_->t;
    // IXON off so Ctrl+S/Ctrl+Q reach us; IEXTEN off so Ctrl+V isn't eaten as "literal next".
    t.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    t.c_cflag |= CS8;
    t.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSAFLUSH, &t) < 0) return fail("tcsetattr: {}", errno_str());
    (void)write_all(fd, as_view("\x1b[?2004h"), Millis(500));
    return r;
}

Raw::~Raw() {
    if (fd_ < 0 || !saved_) return;
    (void)write_all(fd_, as_view("\x1b[?2004l"), Millis(500));
    ::tcsetattr(fd_, TCSAFLUSH, &saved_->t);
}

auto size(int fd) -> std::pair<int, int> {
    winsize ws{};
    if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return {ws.ws_col, ws.ws_row};
    return {80, 24};
}

void Parser::feed(ByteView bytes) { buf_ += as_string(bytes); }

namespace {
constexpr std::string_view paste_start = "\x1b[200~";
constexpr std::string_view paste_end = "\x1b[201~";
}  // namespace

auto Parser::next() -> std::optional<Key> {
    if (in_paste_) {
        auto end = buf_.find(paste_end);
        if (end == std::string::npos) {
            // Keep a tail that might be the start of the end marker.
            auto keep = std::min(buf_.size(), paste_end.size() - 1);
            paste_ += buf_.substr(0, buf_.size() - keep);
            buf_.erase(0, buf_.size() - keep);
            return std::nullopt;
        }
        paste_ += buf_.substr(0, end);
        buf_.erase(0, end + paste_end.size());
        in_paste_ = false;
        return key::Paste{std::exchange(paste_, {})};
    }
    if (buf_.empty()) return std::nullopt;

    auto take = [&](std::size_t n) { buf_.erase(0, n); };
    unsigned char c = static_cast<unsigned char>(buf_[0]);

    if (c == 0x1b) {
        if (buf_.size() == 1) return std::nullopt;  // could be the start of a sequence
        if (buf_.starts_with(paste_start)) {
            take(paste_start.size());
            in_paste_ = true;
            return next();
        }
        if (paste_start.starts_with(buf_)) return std::nullopt;
        if (buf_[1] == '\r' || buf_[1] == '\n') {
            take(2);
            return key::AltEnter{};
        }
        if (buf_[1] == '[' || buf_[1] == 'O') {
            // CSI / SS3: params then a final byte in 0x40..0x7E.
            std::size_t i = 2;
            while (i < buf_.size() && (static_cast<unsigned char>(buf_[i]) < 0x40 || static_cast<unsigned char>(buf_[i]) > 0x7E)) ++i;
            if (i >= buf_.size()) return std::nullopt;
            auto seq = buf_.substr(0, i + 1);
            take(i + 1);
            auto fin = seq.back();
            if (fin == 'D') return key::Left{};
            if (fin == 'C') return key::Right{};
            if (fin == 'H') return key::Home{};
            if (fin == 'F') return key::End{};
            if (seq == "\x1b[3~") return key::Delete{};
            if (seq == "\x1b[1~" || seq == "\x1b[7~") return key::Home{};
            if (seq == "\x1b[4~" || seq == "\x1b[8~") return key::End{};
            if (seq == "\x1b[13;2u" || seq == "\x1b[13;3u" || seq == "\x1b[27;2;13~") return key::AltEnter{};
            // kitty/xterm "CSI 118;5u" is Ctrl+V when the terminal sends key codes.
            if (seq == "\x1b[118;5u") return key::Ctrl{'v'};
            return next();  // ignore anything else
        }
        take(1);
        return key::Escape{};
    }
    if (c == '\r' || c == '\n') {
        take(1);
        return key::Enter{};
    }
    if (c == 0x7f || c == 0x08) {
        take(1);
        return key::Backspace{};
    }
    if (c == '\t') {
        take(1);
        return key::Char{"\t"};
    }
    if (c < 0x20) {
        take(1);
        return key::Ctrl{static_cast<char>('a' + c - 1)};
    }
    std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (buf_.size() < n) return std::nullopt;
    auto s = buf_.substr(0, n);
    take(n);
    return key::Char{repair_utf8(s)};
}

auto Parser::flush_escape() -> std::optional<Key> {
    if (buf_ == "\x1b") {
        buf_.clear();
        return key::Escape{};
    }
    return std::nullopt;
}

auto render(const Composer& c, const View& v, int prev_rows) -> std::pair<std::string, int> {
    const int width = std::max(v.width, 20);
    struct Piece {
        std::string text;
        int w;
        bool chip;
        bool newline;
    };
    std::vector<Piece> pieces;
    for (auto& cell : c.cells())
        pieces.push_back(std::visit(overloaded{
                                        [](const Glyph& g) -> Piece {
                                            if (g.utf8 == "\n") return {"", 0, false, true};
                                            if (g.utf8 == "\t") return {"    ", 4, false, false};
                                            return {g.utf8, cell_width(g.utf8), false, false};
                                        },
                                        [](const Chip& ch) -> Piece {
                                            auto label = "[" + ch.label + "]";
                                            int w = 0;
                                            for (auto& g : split_glyphs(label)) w += cell_width(g);
                                            return {label, w, true, false};
                                        },
                                    },
                                    cell));

    // Lay out into rows, tracking the cursor.
    std::vector<std::string> rows(1, v.prompt);
    int col = static_cast<int>(v.prompt.size());
    int cur_row = 0, cur_col = col;
    auto indent = std::string(v.prompt.size(), ' ');
    for (std::size_t i = 0; i <= pieces.size(); ++i) {
        if (i == c.cursor()) {
            cur_row = static_cast<int>(rows.size()) - 1;
            cur_col = col;
        }
        if (i == pieces.size()) break;
        auto& p = pieces[i];
        if (p.newline) {
            rows.push_back(indent);
            col = static_cast<int>(indent.size());
            continue;
        }
        if (col + p.w > width && col > static_cast<int>(indent.size())) {
            rows.push_back(indent);
            col = static_cast<int>(indent.size());
            if (i == c.cursor()) {
                cur_row = static_cast<int>(rows.size()) - 1;
                cur_col = col;
            }
        }
        rows.back() += p.chip ? "\x1b[7m" + p.text + "\x1b[27m" : p.text;
        col += p.w;
    }
    if (cur_col >= width) {
        rows.push_back(indent);
        cur_row = static_cast<int>(rows.size()) - 1;
        cur_col = static_cast<int>(indent.size());
    }
    if (!v.status.empty()) rows.push_back("\x1b[2m" + v.status + "\x1b[22m");

    std::string out = "\x1b[?25l";
    if (prev_rows > 0) out += std::format("\x1b[{}A", prev_rows);
    out += "\r\x1b[J";
    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (r) out += "\r\n";
        out += rows[r];
    }
    // Move from the last row back to the cursor.
    int last = static_cast<int>(rows.size()) - 1;
    if (last > cur_row) out += std::format("\x1b[{}A", last - cur_row);
    out += "\r";
    if (cur_col > 0) out += std::format("\x1b[{}C", cur_col);
    out += "\x1b[?25h";
    return {out, cur_row};
}

}  // namespace rp::term
