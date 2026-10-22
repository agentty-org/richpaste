#pragma once

#include "rp/core.hpp"

#include <functional>

namespace rp {

// What a clipboard owner is offering, plus a way to fetch one format.
struct Offer {
    std::vector<std::string> targets;  // mime types (or X11 atom names like UTF8_STRING)
    std::function<Result<Bytes>(std::string_view target, std::size_t max)> read;
};

// Picks the best formats from an offer and turns them into typed items.
// Order: copied files, then an image, then text.
auto choose(const Offer& offer, std::uint64_t max_bytes) -> Result<std::vector<Item>>;

struct Clip {
    std::vector<Item> items;
    std::string source;  // e.g. "wayland wayland-1", "x11 :0"
    Route route = Route::display;
};

// Reads this machine's clipboard directly (Wayland data-control, then X11).
auto read_clipboard(std::uint64_t max_bytes) -> Result<Clip>;

// Puts UTF-8 text on this machine's clipboard. Native on Windows (Win32);
// elsewhere it fails and the caller uses pbcopy / wl-copy / xclip or OSC 52.
auto write_clipboard_text(std::string_view utf8) -> Result<void>;

// True if this process can see a display server to read from.
auto has_display_env() -> bool;

namespace wayland {
auto read(const std::string& socket_path, std::uint64_t max_bytes) -> Result<std::vector<Item>>;
}
namespace x11 {
auto read(const std::string& display, std::uint64_t max_bytes) -> Result<std::vector<Item>>;
}
namespace win {
auto read(std::uint64_t max_bytes) -> Result<std::vector<Item>>;
// Puts UTF-8 text on the clipboard as CF_UNICODETEXT.
auto write_text(std::string_view utf8) -> Result<void>;
}
namespace mac {
auto read(std::uint64_t max_bytes) -> Result<std::vector<Item>>;
}

// file:// URI list -> local paths. Skips anything that isn't a local file URI.
auto parse_uri_list(std::string_view list) -> std::vector<std::string>;

}  // namespace rp
