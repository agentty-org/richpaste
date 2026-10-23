// Windows side of the IO helpers. Sockets are Winsock; files are Win32.
#ifdef _WIN32

#include "rp/io.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rp {

namespace fs = std::filesystem;

namespace {

auto sock(int fd) -> SOCKET { return static_cast<SOCKET>(static_cast<unsigned>(fd)); }

auto win_error(DWORD e) -> std::string {
    char buf[512] = {};
    auto n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, sizeof(buf),
                              nullptr);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::format("error {}", e) : s;
}

auto wsa_str() -> std::string { return win_error(static_cast<DWORD>(::WSAGetLastError())); }

auto wait_for(int fd, short events, Millis timeout) -> Result<void> {
    WSAPOLLFD p{};
    p.fd = sock(fd);
    p.events = events;
    int r = ::WSAPoll(&p, 1, static_cast<INT>(std::min<long long>(timeout.count(), 1 << 30)));
    if (r > 0) return {};
    if (r == 0) return fail("timed out");
    return fail("poll: {}", wsa_str());
}

auto wide(const fs::path& p) -> std::wstring { return p.wstring(); }

}  // namespace

void Fd::reset() {
    if (fd_ >= 0) ::closesocket(sock(fd_));
    fd_ = -1;
}

auto errno_str() -> std::string { return win_error(::GetLastError()); }

void platform_init() {
    WSADATA d;
    ::WSAStartup(MAKEWORD(2, 2), &d);
}

auto wait_readable(int fd, Millis timeout) -> Result<void> { return wait_for(fd, POLLRDNORM, timeout); }
auto wait_writable(int fd, Millis timeout) -> Result<void> { return wait_for(fd, POLLWRNORM, timeout); }

auto read_some(int fd, std::span<std::byte> out, Millis timeout) -> Result<std::size_t> {
    if (auto w = wait_readable(fd, timeout); !w) return std::unexpected(w.error());
    int n = ::recv(sock(fd), reinterpret_cast<char*>(out.data()), static_cast<int>(std::min<std::size_t>(out.size(), 1 << 30)), 0);
    if (n >= 0) return static_cast<std::size_t>(n);
    return fail("recv: {}", wsa_str());
}

auto write_all(int fd, ByteView data, Millis timeout) -> Result<void> {
    while (!data.empty()) {
        if (auto w = wait_writable(fd, timeout); !w) return std::unexpected(w.error());
        int n = ::send(sock(fd), reinterpret_cast<const char*>(data.data()),
                       static_cast<int>(std::min<std::size_t>(data.size(), 1 << 30)), 0);
        if (n == SOCKET_ERROR) return fail("send: {}", wsa_str());
        data = data.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

auto unix_listen(const fs::path&) -> Result<Fd> { return fail("unix sockets are not used on Windows"); }
auto unix_connect(const fs::path&) -> Result<Fd> { return fail("unix sockets are not used on Windows"); }

auto tcp_listen_loopback(std::uint16_t port) -> Result<std::pair<Fd, std::uint16_t>> {
    SOCKET s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return fail("socket: {}", wsa_str());
    Fd fd(static_cast<int>(s));
    BOOL excl = TRUE;  // nobody else may bind the same port while we hold it
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR)
        return fail("bind 127.0.0.1:{}: {}", port, wsa_str());
    if (::listen(s, 16) == SOCKET_ERROR) return fail("listen: {}", wsa_str());
    int len = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    return std::pair{std::move(fd), ntohs(a.sin_port)};
}

auto tcp_connect_loopback(std::uint16_t port) -> Result<Fd> {
    SOCKET s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return fail("socket: {}", wsa_str());
    Fd fd(static_cast<int>(s));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR)
        return fail(Code::unreachable, "connect 127.0.0.1:{}: {}", port, wsa_str());
    return fd;
}

auto accept_one(int listener, Millis timeout) -> Result<Fd> {
    if (auto w = wait_readable(listener, timeout); !w) return std::unexpected(w.error());
    SOCKET c = ::accept(sock(listener), nullptr, nullptr);
    if (c == INVALID_SOCKET) return fail("accept: {}", wsa_str());
    return Fd(static_cast<int>(c));
}

auto peer_is_same_user(int fd) -> bool {
    sockaddr_in peer{}, self{};
    int pl = sizeof(peer), sl = sizeof(self);
    if (::getpeername(sock(fd), reinterpret_cast<sockaddr*>(&peer), &pl) != 0) return false;
    if (::getsockname(sock(fd), reinterpret_cast<sockaddr*>(&self), &sl) != 0) return false;
    if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return false;

    // Find the peer's side of this connection in the TCP table to get its process id.
    ULONG size = 0;
    ::GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    std::vector<std::byte> buf(size + 4096);
    size = static_cast<ULONG>(buf.size());
    if (::GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR)
        return false;
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
    DWORD pid = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        auto& r = table->table[i];
        if (static_cast<u_short>(r.dwLocalPort) == peer.sin_port && static_cast<u_short>(r.dwRemotePort) == self.sin_port &&
            r.dwLocalAddr == peer.sin_addr.s_addr) {
            pid = r.dwOwningPid;
            break;
        }
    }
    if (pid == 0) return false;

    auto user_sid = [](HANDLE process, std::vector<std::byte>& out) -> PSID {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(process, TOKEN_QUERY, &token)) return nullptr;
        DWORD n = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &n);
        out.resize(n);
        bool ok = n && ::GetTokenInformation(token, TokenUser, out.data(), n, &n);
        ::CloseHandle(token);
        return ok ? reinterpret_cast<TOKEN_USER*>(out.data())->User.Sid : nullptr;
    };
    HANDLE other = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!other) return false;
    std::vector<std::byte> a, b;
    PSID theirs = user_sid(other, a);
    PSID mine = user_sid(::GetCurrentProcess(), b);
    bool same = theirs && mine && ::EqualSid(theirs, mine);
    ::CloseHandle(other);
    return same;
}

auto read_file(const fs::path& path, std::size_t max) -> Result<Bytes> {
    HANDLE h = ::CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("open {}: {}", path.string(), errno_str());
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        ::CloseHandle(h);
        return fail("stat {}: {}", path.string(), errno_str());
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > max) {
        ::CloseHandle(h);
        return fail("{} is {} (limit {})", path.string(), human_size(static_cast<std::uint64_t>(size.QuadPart)), human_size(max));
    }
    Bytes out(static_cast<std::size_t>(size.QuadPart));
    std::size_t got = 0;
    while (got < out.size()) {
        DWORD n = 0;
        auto want = static_cast<DWORD>(std::min<std::size_t>(out.size() - got, 1 << 30));
        if (!::ReadFile(h, out.data() + got, want, &n, nullptr) || n == 0) break;
        got += n;
    }
    ::CloseHandle(h);
    out.resize(got);
    return out;
}

auto write_file_atomic(const fs::path& path, ByteView data) -> Result<void> {
    auto tmp = path;
    tmp += L".tmp" + fs::path(random_hex(4)).wstring();
    HANDLE h = ::CreateFileW(wide(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("create {}: {}", tmp.string(), errno_str());
    for (auto rest = data; !rest.empty();) {
        DWORD n = 0;
        auto want = static_cast<DWORD>(std::min<std::size_t>(rest.size(), 1 << 30));
        if (!::WriteFile(h, rest.data(), want, &n, nullptr)) {
            ::CloseHandle(h);
            ::DeleteFileW(wide(tmp).c_str());
            return fail("write {}: {}", tmp.string(), errno_str());
        }
        rest = rest.subspan(n);
    }
    ::CloseHandle(h);
    if (!::MoveFileExW(wide(tmp).c_str(), wide(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(wide(tmp).c_str());
        return fail("rename to {}: {}", path.string(), errno_str());
    }
    return {};
}

auto ensure_private_dir(const fs::path& p) -> Result<void> {
    // The user profile is already private to the user on Windows.
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) return fail("create {}: {}", p.string(), ec.message());
    return {};
}

auto FileLock::try_lock(const fs::path& path) -> Result<FileLock> {
    // No sharing: a second agent's CreateFile fails while we hold this.
    HANDLE h = ::CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (::GetLastError() == ERROR_SHARING_VIOLATION) return fail("another agent is already running");
        return fail("open {}: {}", path.string(), errno_str());
    }
    return FileLock(reinterpret_cast<std::intptr_t>(h));
}

FileLock::~FileLock() {
    if (h_ != -1) ::CloseHandle(reinterpret_cast<HANDLE>(h_));
}

auto env(const char* name) -> std::optional<std::string> {
    // Read the wide environment so non-ASCII values survive, then convert to UTF-8.
    std::wstring wname(name, name + std::strlen(name));
    DWORD n = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring buf(n, L'\0');
    n = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    buf.resize(n);
    if (buf.empty()) return std::nullopt;
    return fs::path(buf).string();
}

auto current_uid() -> std::optional<unsigned> { return std::nullopt; }

auto random_bytes(std::size_t n) -> Bytes {
    Bytes buf(n);
    if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        std::abort();
    return buf;
}

auto self_exe() -> fs::path {
    std::wstring buf(32768, L'\0');
    auto n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return buf;
}

namespace {

// Quotes one argument the way CommandLineToArgvW splits it.
auto quote_arg(const std::wstring& a) -> std::wstring {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

}  // namespace

auto spawn_detached(const fs::path& exe, std::span<const std::string> args, const fs::path& log) -> Result<void> {
    std::wstring cmd = quote_arg(exe.wstring());
    for (auto& a : args) cmd += L" " + quote_arg(fs::path(a).wstring());

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE lf = ::CreateFileW(wide(log).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = lf != INVALID_HANDLE_VALUE ? lf : nul;
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                               DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    auto err = ::GetLastError();
    if (lf != INVALID_HANDLE_VALUE) ::CloseHandle(lf);
    if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
    if (!ok) return fail("start agent: {}", win_error(err));
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return {};
}

auto run_capture(std::span<const std::string> args, Millis timeout) -> Result<std::string> {
    if (args.empty()) return fail("nothing to run");
    std::wstring cmd;
    for (auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + quote_arg(fs::path(a).wstring());

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return fail("pipe: {}", errno_str());
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                               &pi);
    auto err = ::GetLastError();
    ::CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
    if (!ok) {
        ::CloseHandle(rd);
        return fail("{}: {}", args[0], win_error(err));
    }
    ::CloseHandle(pi.hThread);

    // Small outputs (tmux queries); read until EOF or the deadline.
    std::string out;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!::PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // EOF
        if (avail) {
            DWORD n = 0;
            if (!::ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) || n == 0) break;
            out.append(buf, n);
            if (out.size() > (1u << 20)) break;
            continue;
        }
        if (::WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) {
            // Drain what's left after exit.
            DWORD n = 0;
            while (::PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
                   ::ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) && n)
                out.append(buf, n);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::TerminateProcess(pi.hProcess, 1);
            ::CloseHandle(pi.hProcess);
            ::CloseHandle(rd);
            return fail("{} timed out", args[0]);
        }
    }
    ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count()));
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(rd);
    if (code != 0) return fail("{} failed", args[0]);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}  // namespace rp

#endif
