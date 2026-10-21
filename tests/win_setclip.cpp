// Test-only (Windows): put something on the Win32 clipboard.
//   win_setclip text file.txt     (UTF-8 file -> CF_UNICODETEXT)
//   win_setclip dib  W H          (24-bit gradient, CF_DIB)
//   win_setclip png  file.png     (registered "PNG" format)
//   win_setclip files a.txt b.png (CF_HDROP)
#include "rp/io.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cstring>
#include <print>
#include <string>

namespace {

auto put(UINT format, const void* data, std::size_t size) -> bool {
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, size);
    if (!h) return false;
    std::memcpy(::GlobalLock(h), data, size);
    ::GlobalUnlock(h);
    return ::SetClipboardData(format, h) != nullptr;
}

auto widen(const char* s) -> std::wstring {
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    w.resize(static_cast<std::size_t>(n - 1));
    return w;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string kind = argv[1];
    if (!::OpenClipboard(nullptr)) return 3;
    ::EmptyClipboard();
    bool ok = false;
    if (kind == "text") {
        // From a UTF-8 file: argv is in the ANSI codepage and would mangle the test text.
        auto data = rp::read_file(argv[2], 1u << 20);
        if (!data) return 4;
        auto s = rp::as_string(*data);
        auto w = widen(s.c_str());
        ok = put(CF_UNICODETEXT, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    } else if (kind == "dib" && argc >= 4) {
        int W = std::stoi(argv[2]), H = std::stoi(argv[3]);
        int stride = ((W * 24 + 31) / 32) * 4;
        std::vector<unsigned char> buf(sizeof(BITMAPINFOHEADER) + static_cast<std::size_t>(stride) * H);
        auto* bi = reinterpret_cast<BITMAPINFOHEADER*>(buf.data());
        bi->biSize = sizeof(BITMAPINFOHEADER);
        bi->biWidth = W;
        bi->biHeight = H;  // bottom-up
        bi->biPlanes = 1;
        bi->biBitCount = 24;
        bi->biCompression = BI_RGB;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                // Row y of the image (top = 0) lives at bottom-up row H-1-y.
                auto* p = buf.data() + sizeof(BITMAPINFOHEADER) + (H - 1 - y) * stride + x * 3;
                p[0] = static_cast<unsigned char>((x * 3) & 255);       // B
                p[1] = static_cast<unsigned char>((y * 5) & 255);       // G
                p[2] = static_cast<unsigned char>((x + y) & 255);       // R
            }
        ok = put(CF_DIB, buf.data(), buf.size());
    } else if (kind == "png") {
        auto data = rp::read_file(argv[2], 1u << 30);
        if (data) ok = put(::RegisterClipboardFormatW(L"PNG"), data->data(), data->size());
    } else if (kind == "files") {
        std::wstring list;
        for (int i = 2; i < argc; ++i) {
            wchar_t full[MAX_PATH];
            ::GetFullPathNameW(widen(argv[i]).c_str(), MAX_PATH, full, nullptr);
            list += full;
            list += L'\0';
        }
        list += L'\0';
        std::vector<unsigned char> buf(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
        auto* df = reinterpret_cast<DROPFILES*>(buf.data());
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        std::memcpy(buf.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        ok = put(CF_HDROP, buf.data(), buf.size());
    }
    ::CloseClipboard();
    return ok ? 0 : 1;
}
