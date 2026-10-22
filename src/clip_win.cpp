// Windows clipboard: reads files (CF_HDROP), PNG, bitmaps (CF_DIBV5/CF_DIB -> PNG),
// Unicode text; writes Unicode text.
#ifdef _WIN32

#include "rp/clipboard.hpp"
#include "rp/image.hpp"
#include "rp/io.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <thread>

namespace rp::win {

namespace fs = std::filesystem;

namespace {

auto utf8(std::wstring_view w) -> std::string {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Opens the clipboard, retrying while another app holds it.
class Clipboard {
public:
    static auto open() -> Result<Clipboard> {
        for (int i = 0; i < 40; ++i) {
            if (::OpenClipboard(nullptr)) return Clipboard();
            std::this_thread::sleep_for(Millis(25));
        }
        return fail("the clipboard is busy (another app has it open)");
    }
    Clipboard(Clipboard&& o) noexcept : open_(std::exchange(o.open_, false)) {}
    Clipboard(const Clipboard&) = delete;
    auto operator=(Clipboard&&) -> Clipboard& = delete;
    ~Clipboard() {
        if (open_) ::CloseClipboard();
    }

private:
    Clipboard() = default;
    bool open_ = true;
};

// Copies the bytes of a global memory clipboard handle.
auto global_bytes(HANDLE h, std::size_t max) -> Result<Bytes> {
    if (!h) return fail("no data");
    auto size = ::GlobalSize(h);
    if (size > max) return fail("clipboard data is larger than the {} limit", human_size(max));
    auto* p = static_cast<const std::byte*>(::GlobalLock(h));
    if (!p) return fail("could not lock clipboard data");
    Bytes out(p, p + size);
    ::GlobalUnlock(h);
    return out;
}

auto format_name(UINT f) -> std::string {
    switch (f) {
        case CF_TEXT: return "CF_TEXT";
        case CF_UNICODETEXT: return "CF_UNICODETEXT";
        case CF_DIB: return "CF_DIB";
        case CF_DIBV5: return "CF_DIBV5";
        case CF_BITMAP: return "CF_BITMAP";
        case CF_HDROP: return "CF_HDROP";
        default: break;
    }
    wchar_t buf[256];
    int n = ::GetClipboardFormatNameW(f, buf, 256);
    return n > 0 ? utf8({buf, static_cast<std::size_t>(n)}) : std::format("#{}", f);
}

}  // namespace

auto read(std::uint64_t max_bytes) -> Result<std::vector<Item>> {
    auto cb = Clipboard::open();
    if (!cb) return std::unexpected(cb.error());

    std::vector<UINT> formats;
    for (UINT f = ::EnumClipboardFormats(0); f; f = ::EnumClipboardFormats(f)) formats.push_back(f);
    if (formats.empty()) return fail("clipboard is empty");
    auto has = [&](UINT f) { return std::ranges::contains(formats, f); };
    UINT png = ::RegisterClipboardFormatW(L"PNG");
    UINT png_mime = ::RegisterClipboardFormatW(L"image/png");

    // Copied files from Explorer.
    if (has(CF_HDROP)) {
        auto drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP));
        if (drop) {
            UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<Item> items;
            std::uint64_t total = 0;
            for (UINT i = 0; i < count; ++i) {
                UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(len + 1, L'\0');
                ::DragQueryFileW(drop, i, path.data(), len + 1);
                path.resize(len);
                std::error_code ec;
                if (!fs::is_regular_file(path, ec)) continue;
                auto data = read_file(path, max_bytes > total ? max_bytes - total : 0);
                if (!data) return std::unexpected(data.error());
                total += data->size();
                auto name = FileName::sanitize(fs::path(path).filename().string());
                auto mime = mime_for_name(name.str());
                items.push_back(File{std::move(name), std::move(mime), std::move(*data)});
            }
            if (!items.empty()) return items;
            if (count > 0) return fail("copied item is not a regular file (folders aren't supported)");
        }
    }

    // PNG as published by browsers, Office, Snipping Tool.
    for (UINT f : {png, png_mime}) {
        if (!f || !has(f)) continue;
        auto data = global_bytes(::GetClipboardData(f), max_bytes);
        if (data && data->size() > 8) return std::vector<Item>{Image{*Mime::parse("image/png"), std::move(*data)}};
    }

    // Bitmaps: Print Screen and most apps. Windows synthesizes CF_DIB from CF_BITMAP for us.
    for (UINT f : {static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
        if (!has(f) && !(f == CF_DIB && has(CF_BITMAP))) continue;
        auto dib = global_bytes(::GetClipboardData(f), max_bytes);
        if (!dib) continue;
        auto img = image::decode_dib(*dib);
        if (!img) continue;
        auto out = image::encode_png(*img);
        if (out.size() > max_bytes) return fail("image is larger than the {} limit", human_size(max_bytes));
        return std::vector<Item>{Image{*Mime::parse("image/png"), std::move(out)}};
    }

    // Text.
    if (has(CF_UNICODETEXT)) {
        auto data = global_bytes(::GetClipboardData(CF_UNICODETEXT), max_bytes * 2);
        if (!data) return std::unexpected(data.error());
        std::wstring_view w(reinterpret_cast<const wchar_t*>(data->data()), data->size() / sizeof(wchar_t));
        if (auto z = w.find(L'\0'); z != std::wstring_view::npos) w = w.substr(0, z);
        return std::vector<Item>{Text::repair(utf8(w))};
    }

    std::string list;
    for (auto f : formats) {
        if (!list.empty()) list += ", ";
        list += format_name(f);
        if (list.size() > 200) break;
    }
    return fail("nothing pasteable on the clipboard (offered: {})", list);
}

auto write_text(std::string_view text) -> Result<void> {
    // UTF-16 with a trailing NUL, in movable global memory the clipboard then owns.
    int n = text.empty()
                ? 0
                : ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!text.empty() && n == 0) return fail("text is not valid UTF-8");
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, (static_cast<std::size_t>(n) + 1) * sizeof(wchar_t));
    if (!mem) return fail("out of memory");
    auto* w = static_cast<wchar_t*>(::GlobalLock(mem));
    if (n) ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), w, n);
    w[n] = L'\0';
    ::GlobalUnlock(mem);

    auto cb = Clipboard::open();
    if (!cb) {
        ::GlobalFree(mem);
        return std::unexpected(cb.error());
    }
    ::EmptyClipboard();
    if (!::SetClipboardData(CF_UNICODETEXT, mem)) {
        ::GlobalFree(mem);
        return fail("SetClipboardData failed");
    }
    return {};
}

}  // namespace rp::win

#endif
