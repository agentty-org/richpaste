#include "rp/term.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <chrono>

namespace rp::term {

#ifdef _WIN32

struct Tty::State {
    HANDLE in = INVALID_HANDLE_VALUE;
    HANDLE out = INVALID_HANDLE_VALUE;
    DWORD in_mode = 0, out_mode = 0;
    UINT in_cp = 0, out_cp = 0;
};

auto Tty::open() -> Result<Tty> {
    auto s = std::make_unique<State>();
    s->in = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    s->out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    auto close_both = [&] {
        if (s->in != INVALID_HANDLE_VALUE) ::CloseHandle(s->in);
        if (s->out != INVALID_HANDLE_VALUE) ::CloseHandle(s->out);
    };
    if (s->in == INVALID_HANDLE_VALUE || s->out == INVALID_HANDLE_VALUE) {
        close_both();
        return fail("no console");
    }
    if (!::GetConsoleMode(s->in, &s->in_mode) || !::GetConsoleMode(s->out, &s->out_mode)) {
        close_both();
        return fail("not a console");
    }
    s->in_cp = ::GetConsoleCP();
    s->out_cp = ::GetConsoleOutputCP();
    ::SetConsoleCP(CP_UTF8);
    ::SetConsoleOutputCP(CP_UTF8);
    // VT input: the terminal's replies arrive as the same bytes a Unix tty gives.
    DWORD in = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
    DWORD out = s->out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
    if (!::SetConsoleMode(s->in, in) || !::SetConsoleMode(s->out, out)) {
        ::SetConsoleCP(s->in_cp);
        ::SetConsoleOutputCP(s->out_cp);
        close_both();
        return fail("this console has no VT mode");
    }
    return Tty(std::move(s));
}

Tty::~Tty() {
    if (!s_) return;
    ::SetConsoleMode(s_->in, s_->in_mode);
    ::SetConsoleMode(s_->out, s_->out_mode);
    ::SetConsoleCP(s_->in_cp);
    ::SetConsoleOutputCP(s_->out_cp);
    ::CloseHandle(s_->in);
    ::CloseHandle(s_->out);
}

auto Tty::read(std::span<std::byte> out, Millis timeout) -> Result<std::size_t> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto left = std::chrono::duration_cast<Millis>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return fail("timed out");
        if (::WaitForSingleObject(s_->in, static_cast<DWORD>(left.count())) != WAIT_OBJECT_0)
            return fail("timed out");
        // The handle also signals for focus and mouse records, which ReadFile
        // would block on. Skip non-key records; only read when a key is waiting.
        INPUT_RECORD rec;
        DWORD got = 0;
        if (!::PeekConsoleInputW(s_->in, &rec, 1, &got) || got == 0) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
            ::ReadConsoleInputW(s_->in, &rec, 1, &got);
            continue;
        }
        DWORD n = 0;
        if (!::ReadFile(s_->in, out.data(), static_cast<DWORD>(out.size()), &n, nullptr))
            return fail("reading the console: {}", errno_str());
        return static_cast<std::size_t>(n);
    }
}

auto Tty::write(std::string_view str) -> Result<void> {
    while (!str.empty()) {
        DWORD n = 0;
        if (!::WriteFile(s_->out, str.data(), static_cast<DWORD>(str.size()), &n, nullptr))
            return fail("writing the console: {}", errno_str());
        str.remove_prefix(n);
    }
    return {};
}

#else

struct Tty::State {
    int fd = -1;
    termios saved{};
};

auto Tty::open() -> Result<Tty> {
    auto s = std::make_unique<State>();
    s->fd = ::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (s->fd < 0) return fail("no terminal: {}", errno_str());
    if (::tcgetattr(s->fd, &s->saved) < 0) {
        ::close(s->fd);
        return fail("tcgetattr: {}", errno_str());
    }
    // Raw, so the terminal's replies come through byte for byte.
    termios t = s->saved;
    t.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    t.c_cflag |= CS8;
    t.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (::tcsetattr(s->fd, TCSAFLUSH, &t) < 0) {
        ::close(s->fd);
        return fail("tcsetattr: {}", errno_str());
    }
    return Tty(std::move(s));
}

Tty::~Tty() {
    if (!s_) return;
    ::tcsetattr(s_->fd, TCSAFLUSH, &s_->saved);
    ::close(s_->fd);
}

auto Tty::read(std::span<std::byte> out, Millis timeout) -> Result<std::size_t> {
    return read_some(s_->fd, out, timeout);
}

auto Tty::write(std::string_view str) -> Result<void> {
    return write_all(s_->fd, as_view(str), Millis(1000));
}

#endif

auto Tty::session() -> inband::Session {
    return inband::Session{
        [this](std::span<std::byte> out, Millis t) { return read(out, t); },
        [this](std::string_view str) { return write(str); },
        {},
    };
}

Tty::Tty(std::unique_ptr<State> s) : s_(std::move(s)) {}
Tty::Tty(Tty&& o) noexcept : s_(std::move(o.s_)) {}

}  // namespace rp::term
