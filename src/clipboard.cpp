#include "rp/clipboard.hpp"
#include "rp/agent.hpp"   // config()
#include "rp/io.hpp"

#include <algorithm>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace rp {

namespace fs = std::filesystem;

namespace {

auto has(const Offer& o, std::string_view t) -> bool { return std::ranges::contains(o.targets, t); }

auto hex_val(char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

auto percent_decode(std::string_view s) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

auto local_hostname() -> std::string {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return {};
    return buf;
}

auto read_files(const Offer& offer, std::string_view target, std::uint64_t max_bytes) -> Result<std::vector<Item>> {
    auto raw = offer.read(target, 1 << 20);
    if (!raw) return std::unexpected(raw.error());
    auto paths = parse_uri_list(as_string(*raw));
    std::vector<Item> items;
    std::uint64_t total = 0;
    std::vector<std::string> skipped;
    for (auto& p : paths) {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) {
            skipped.push_back(p);
            continue;
        }
        auto left = max_bytes > total ? max_bytes - total : 0;
        auto data = read_file(p, left);
        if (!data) return std::unexpected(data.error());
        total += data->size();
        auto name = FileName::sanitize(fs::path(p).filename().string());
        auto mime = mime_for_name(name.str());
        items.push_back(File{std::move(name), std::move(mime), std::move(*data)});
    }
    if (items.empty() && !skipped.empty())
        return fail("copied item is not a regular file (folders aren't supported): {}", skipped.front());
    return items;
}

}  // namespace

auto parse_uri_list(std::string_view list) -> std::vector<std::string> {
    std::vector<std::string> out;
    auto host = local_hostname();
    while (!list.empty()) {
        auto nl = list.find('\n');
        auto line = list.substr(0, nl);
        list = nl == std::string_view::npos ? std::string_view{} : list.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        if (!line.starts_with("file://")) continue;
        auto rest = line.substr(7);
        auto slash = rest.find('/');
        if (slash == std::string_view::npos) continue;
        auto authority = rest.substr(0, slash);
        if (!authority.empty() && authority != "localhost" && authority != host) continue;
        out.push_back(percent_decode(rest.substr(slash)));
    }
    return out;
}

auto choose(const Offer& offer, std::uint64_t max_bytes) -> Result<std::vector<Item>> {
    // Copied files (file managers). GNOME's format starts with "copy"/"cut".
    for (auto t : {"text/uri-list", "x-special/gnome-copied-files"}) {
        if (!has(offer, t)) continue;
        auto files = read_files(offer, t, max_bytes);
        if (!files) return files;
        if (!files->empty()) return files;
    }

    // Images: prefer lossless/common formats, then anything image/*.
    std::vector<std::string_view> images;
    for (auto t : {"image/png", "image/jpeg", "image/webp", "image/gif", "image/avif", "image/bmp", "image/tiff"})
        if (has(offer, t)) images.push_back(t);
    for (auto& t : offer.targets)
        if (t.starts_with("image/") && !std::ranges::contains(images, t)) images.push_back(t);
    for (auto t : images) {
        auto data = offer.read(t, max_bytes);
        if (!data) return std::unexpected(data.error());
        if (data->empty()) continue;
        auto mime = Mime::parse(t);
        if (!mime) continue;
        return std::vector<Item>{Image{std::move(*mime), std::move(*data)}};
    }

    // Text. STRING is Latin-1 by X11 rules; the rest we treat as UTF-8.
    for (auto t : {"text/plain;charset=utf-8", "text/plain;charset=UTF-8", "UTF8_STRING", "text/plain", "TEXT",
                   "STRING"}) {
        if (!has(offer, t)) continue;
        auto data = offer.read(t, max_bytes);
        if (!data) return std::unexpected(data.error());
        auto s = as_string(*data);
        auto text = std::string_view(t) == "STRING" ? Text::repair(latin1_to_utf8(s)) : Text::repair(s);
        return std::vector<Item>{std::move(text)};
    }

    if (offer.targets.empty()) return fail(Code::empty, "clipboard is empty");
    std::string list;
    for (auto& t : offer.targets) {
        if (!list.empty()) list += ", ";
        list += t;
        if (list.size() > 200) break;
    }
    return fail(Code::empty, "nothing pasteable on the clipboard (offered: {})", list);
}

auto has_display_env() -> bool { return env("WAYLAND_DISPLAY") || env("DISPLAY"); }

auto write_clipboard_text(std::string_view utf8) -> Result<void> {
#if defined(_WIN32)
    return win::write_text(utf8);
#else
    (void)utf8;
    return fail("no native clipboard write on this platform");
#endif
}

auto read_clipboard(std::uint64_t max_bytes) -> Result<Clip> {
    if (!config().display) return fail(Code::no_display, "no display server found (reading this machine's clipboard is turned off)");
#if defined(_WIN32)
    auto r = win::read(max_bytes);
    if (!r) return std::unexpected(r.error());
    return Clip{std::move(*r), "windows"};
#elif defined(__APPLE__)
    // Always the pasteboard: DISPLAY on a Mac only means XQuartz is installed.
    {
        auto r = mac::read(max_bytes);
        if (!r) return std::unexpected(r.error());
        return Clip{std::move(*r), "macos"};
    }
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
    std::vector<std::string> errors;

    // Wayland sockets: $WAYLAND_DISPLAY first, then any wayland-* in the runtime dir.
    std::vector<std::string> wl;
    auto runtime = env("XDG_RUNTIME_DIR");
    if (auto d = env("WAYLAND_DISPLAY")) wl.push_back(d->starts_with('/') || !runtime ? *d : *runtime + "/" + *d);
    if (runtime) {
        std::error_code ec;
        std::vector<std::string> found;
        for (auto& e : fs::directory_iterator(*runtime, ec)) {
            auto name = e.path().filename().string();
            if (name.starts_with("wayland-") && !name.ends_with(".lock") && e.is_socket(ec))
                found.push_back(e.path().string());
        }
        std::ranges::sort(found);
        for (auto& f : found)
            if (!std::ranges::contains(wl, f)) wl.push_back(f);
    }
    for (auto& sock : wl) {
        auto r = wayland::read(sock, max_bytes);
        if (r) return Clip{std::move(*r), "wayland " + fs::path(sock).filename().string()};
        errors.push_back(std::format("wayland {}: {}", sock, r.error().what));
        // An empty clipboard is an answer, not a backend failure.
        if (r.error().code == Code::empty) return std::unexpected(r.error());
    }

    // X11: $DISPLAY first, then any local X server socket.
    std::vector<std::string> xs;
    if (auto d = env("DISPLAY")) xs.push_back(*d);
    std::error_code ec;
    std::vector<std::string> found;
    for (auto& e : fs::directory_iterator("/tmp/.X11-unix", ec)) {
        auto name = e.path().filename().string();
        if (name.size() > 1 && name[0] == 'X' && std::ranges::all_of(name.substr(1), ::isdigit))
            found.push_back(":" + name.substr(1));
    }
    std::ranges::sort(found, {}, [](const std::string& s) { return std::stoi(s.substr(1)); });
    for (auto& f : found)
        if (!std::ranges::contains(xs, f)) xs.push_back(f);
    for (auto& d : xs) {
        auto r = x11::read(d, max_bytes);
        if (r) return Clip{std::move(*r), "x11 " + d};
        errors.push_back(std::format("x11 {}: {}", d, r.error().what));
        if (r.error().code == Code::empty) return std::unexpected(r.error());
    }

    if (errors.empty()) return fail(Code::no_display, "no display server found (no Wayland or X11 session on this machine)");
    std::string all;
    for (auto& e : errors) all += (all.empty() ? "" : "; ") + e;
    // Every display we found failed to answer: as good as none.
    return fail(Code::no_display, "{}", all);
#endif
}

}  // namespace rp
