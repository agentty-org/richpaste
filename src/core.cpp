#include "rp/core.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace rp {

namespace {

// Decodes one UTF-8 sequence at s[i]. Returns length, or 0 if malformed.
auto utf8_len(std::string_view s, std::size_t i) -> std::size_t {
    auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[i + k]); };
    auto cont = [&](std::size_t k) { return i + k < s.size() && (b(k) & 0xC0) == 0x80; };
    unsigned char c = b(0);
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(1) || !cont(2)) return 0;
        if (c == 0xE0 && b(1) < 0xA0) return 0;  // overlong
        if (c == 0xED && b(1) > 0x9F) return 0;  // surrogate
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(1) || !cont(2) || !cont(3)) return 0;
        if (c == 0xF0 && b(1) < 0x90) return 0;  // overlong
        if (c == 0xF4 && b(1) > 0x8F) return 0;  // > U+10FFFF
        return 4;
    }
    return 0;
}

auto is_token_char(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) || std::string_view("!#$&^_.+-").contains(c);
}

auto lower(std::string_view s) -> std::string {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

struct MimeExt {
    std::string_view ext;
    std::string_view mime;
};

// First entry for a mime is its preferred extension.
constexpr std::array mime_table{
    MimeExt{"png", "image/png"},        MimeExt{"jpg", "image/jpeg"},      MimeExt{"jpeg", "image/jpeg"},
    MimeExt{"gif", "image/gif"},        MimeExt{"webp", "image/webp"},     MimeExt{"avif", "image/avif"},
    MimeExt{"bmp", "image/bmp"},        MimeExt{"tiff", "image/tiff"},     MimeExt{"tif", "image/tiff"},
    MimeExt{"svg", "image/svg+xml"},    MimeExt{"ico", "image/x-icon"},    MimeExt{"heic", "image/heic"},
    MimeExt{"pdf", "application/pdf"},  MimeExt{"zip", "application/zip"}, MimeExt{"gz", "application/gzip"},
    MimeExt{"tar", "application/x-tar"}, MimeExt{"json", "application/json"},
    MimeExt{"mp3", "audio/mpeg"},       MimeExt{"wav", "audio/wav"},       MimeExt{"ogg", "audio/ogg"},
    MimeExt{"flac", "audio/flac"},      MimeExt{"m4a", "audio/mp4"},       MimeExt{"mp4", "video/mp4"},
    MimeExt{"webm", "video/webm"},      MimeExt{"mkv", "video/x-matroska"}, MimeExt{"mov", "video/quicktime"},
    MimeExt{"txt", "text/plain"},       MimeExt{"md", "text/markdown"},    MimeExt{"html", "text/html"},
    MimeExt{"csv", "text/csv"},
};

}  // namespace

auto valid_utf8(std::string_view s) -> bool {
    for (std::size_t i = 0; i < s.size();) {
        auto n = utf8_len(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

auto repair_utf8(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        auto n = utf8_len(s, i);
        if (n == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s.substr(i, n));
            i += n;
        }
    }
    return out;
}

auto latin1_to_utf8(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

auto Mime::parse(std::string_view s) -> Result<Mime> {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty() || s.size() > 255) return fail("bad mime type length");
    auto semi = s.find(';');
    auto essence = s.substr(0, semi);
    auto slash = essence.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == essence.size())
        return fail("mime type needs type/subtype: '{}'", s);
    for (std::size_t i = 0; i < essence.size(); ++i) {
        if (i == slash) continue;
        if (!is_token_char(essence[i])) return fail("bad character in mime type '{}'", s);
    }
    std::string out = lower(essence);
    if (semi != std::string_view::npos) {
        auto params = s.substr(semi);
        for (char c : params)
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
                return fail("bad character in mime parameters");
        out += params;
    }
    return Mime{std::move(out)};
}

auto FileName::sanitize(std::string_view s) -> FileName {
    if (auto cut = s.find_last_of("/\\"); cut != std::string_view::npos) s = s.substr(cut + 1);
    std::string out = repair_utf8(s);
    std::erase_if(out, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
    for (char& c : out)
        if (std::string_view("<>:\"|?*").contains(c)) c = '_';
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (!out.empty() && out.front() == '.') out.front() = '_';
    if (out.size() > 200) {
        std::size_t n = 200;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
        out.resize(n);
    }
    if (out.empty()) out = "file";
    return FileName{std::move(out)};
}

auto FileName::parse(std::string_view s) -> Result<FileName> {
    auto clean = sanitize(s);
    if (clean.str() != s) return fail("unsafe file name '{}'", repair_utf8(s));
    return clean;
}

auto Text::parse(std::string s) -> Result<Text> {
    if (!valid_utf8(s)) return fail("text is not valid UTF-8");
    return Text{std::move(s)};
}

auto mime_for_name(std::string_view name) -> Mime {
    auto dot = name.rfind('.');
    if (dot != std::string_view::npos) {
        auto ext = lower(name.substr(dot + 1));
        for (auto& e : mime_table)
            if (e.ext == ext) return *Mime::parse(e.mime);
    }
    return *Mime::parse("application/octet-stream");
}

auto ext_for_mime(std::string_view mime) -> std::string_view {
    auto essence = mime.substr(0, mime.find(';'));
    for (auto& e : mime_table)
        if (e.mime == essence) return e.ext;
    return "bin";
}

auto human_size(std::uint64_t n) -> std::string {
    constexpr std::array units{"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    std::size_t u = 0;
    while (v >= 1024 && u + 1 < units.size()) {
        v /= 1024;
        ++u;
    }
    return u == 0 ? std::format("{} B", n) : std::format("{:.1f} {}", v, units[u]);
}

auto base64_encode(ByteView data) -> std::string {
    constexpr char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    auto at = [&](std::size_t k) { return std::to_integer<std::uint32_t>(data[k]); };
    for (; i + 3 <= data.size(); i += 3) {
        auto v = (at(i) << 16) | (at(i + 1) << 8) | at(i + 2);
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += abc[(v >> 6) & 63];
        out += abc[v & 63];
    }
    if (auto rest = data.size() - i; rest > 0) {
        auto v = at(i) << 16 | (rest == 2 ? at(i + 1) << 8 : 0);
        out += abc[v >> 18];
        out += abc[(v >> 12) & 63];
        out += rest == 2 ? abc[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

auto base64_decode(std::string_view s) -> Result<Bytes> {
    if (s.size() % 4 != 0) return fail("base64 length is not a multiple of 4");
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    Bytes out;
    out.reserve(s.size() / 4 * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        bool last = i + 4 == s.size();
        int a = val(s[i]), b = val(s[i + 1]);
        int c = s[i + 2] == '=' && last ? -2 : val(s[i + 2]);
        int d = s[i + 3] == '=' && last ? -2 : val(s[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2)) return fail("invalid base64");
        auto v = static_cast<std::uint32_t>(a << 18 | b << 12 | (c < 0 ? 0 : c) << 6 | (d < 0 ? 0 : d));
        out.push_back(static_cast<std::byte>(v >> 16));
        if (c >= 0) out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
        if (d >= 0) out.push_back(static_cast<std::byte>(v & 0xFF));
    }
    return out;
}

}  // namespace rp
