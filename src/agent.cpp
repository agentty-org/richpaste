#include "rp/agent.hpp"

#include "rp/wire.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <print>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace rp {

namespace fs = std::filesystem;

namespace {

auto ensure_private_dir(const fs::path& p) -> Result<void> {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) return fail("create {}: {}", p.string(), ec.message());
    ::chmod(p.c_str(), 0700);
    return {};
}

// Only trust sockets that are real sockets owned by us (not symlinks, not planted by others).
auto ours(const fs::path& p) -> bool {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == ::getuid();
}

auto mtime(const fs::path& p) -> std::int64_t {
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0) return 0;
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

auto encode_clip(const Result<Clip>& clip, std::uint64_t max) -> Bytes {
    if (!clip) return wire::encode_error(clip.error().what);
    std::uint64_t total = 0;
    for (auto& i : clip->items) total += wire::encoded_size(i);
    if (total > max) return wire::encode_error(std::format("clipboard is {} (limit {})", human_size(total), human_size(max)));
    return wire::encode_items(clip->items);
}

auto summary(const std::vector<Item>& items) -> std::string {
    std::string s;
    for (auto& i : items) {
        if (!s.empty()) s += ", ";
        s += std::format("{} {}", mime_of(i).substr(0, mime_of(i).find(';')), human_size(payload(i).size()));
    }
    return s.empty() ? "nothing" : s;
}

auto fetch_hops(const fs::path& socket, std::uint64_t max, std::uint32_t hops) -> Result<std::vector<Item>> {
    auto fd = unix_connect(socket);
    if (!fd) return std::unexpected(fd.error());
    auto req = wire::encode_request({wire::Op::clipboard, max, hops});
    if (auto w = write_all(fd->get(), req, Millis(5000)); !w) return fail("sending request: {}", w.error().what);
    // Idle timeout covers the agent reading the clipboard and slow links.
    auto resp = read_all(fd->get(), static_cast<std::size_t>(max) + (1u << 20), Millis(30000));
    if (!resp) return fail("reading reply: {}", resp.error().what);
    if (resp->empty()) return fail("agent on your machine isn't running (forward closed with no reply)");
    return wire::decode_response(*resp);
}

// Are we inside an SSH session? Asks tmux too, since panes keep stale env.
auto in_ssh_session() -> bool {
    if (env("TMUX")) {
        if (FILE* p = ::popen("tmux show-environment SSH_CONNECTION 2>/dev/null", "r")) {
            char buf[256] = {};
            auto n = std::fread(buf, 1, sizeof(buf) - 1, p);
            ::pclose(p);
            std::string_view s(buf, n);
            if (s.starts_with("SSH_CONNECTION=")) return true;
            if (s.starts_with("-SSH_CONNECTION")) return false;
        }
    }
    return env("SSH_CONNECTION") || env("SSH_TTY");
}

struct Attempt {
    std::vector<std::string> errors;
};

auto from_forwards(std::uint64_t max, std::uint32_t hops, Attempt& a) -> std::optional<Result<Clip>> {
    for (auto& s : find_forwarded_sockets()) {
        auto r = fetch_hops(s, max, hops);
        if (r) return Clip{std::move(*r), "ssh " + s.filename().string()};
        // A dead forward leaves its socket file behind; clear it.
        if (r.error().what.contains("Connection refused")) {
            std::error_code ec;
            fs::remove(s, ec);
            continue;
        }
        // The agent answered with a real error (empty clipboard, too big). That's the answer.
        if (!r.error().what.starts_with("connect") && !r.error().what.starts_with("sending") &&
            !r.error().what.starts_with("reading reply") && !r.error().what.starts_with("agent on your machine"))
            return std::unexpected(r.error());
        a.errors.push_back(std::format("{}: {}", s.filename().string(), r.error().what));
    }
    return std::nullopt;
}

auto from_display(std::uint64_t max, Attempt& a) -> std::optional<Result<Clip>> {
    auto r = read_clipboard(max);
    if (r) return r;
    if (r.error().what.starts_with("no display server")) {
        a.errors.push_back(r.error().what);
        return std::nullopt;
    }
    return r;
}

auto paste_impl(std::uint64_t max, std::uint32_t hops, bool prefer_forwards) -> Result<Clip> {
    if (auto s = env("RICHPASTE_SOCK")) {
        auto r = fetch_hops(*s, max, hops);
        if (!r) return std::unexpected(r.error());
        return Clip{std::move(*r), "RICHPASTE_SOCK"};
    }
    Attempt a;
    if (prefer_forwards) {
        if (auto r = from_forwards(max, hops, a)) return std::move(*r);
        if (auto r = from_display(max, a)) return std::move(*r);
    } else {
        if (auto r = from_display(max, a)) return std::move(*r);
        if (auto r = from_forwards(max, hops, a)) return std::move(*r);
    }
    std::string why;
    for (auto& e : a.errors) why += "\n  " + e;
    return fail(
        "can't reach a clipboard.{}\n"
        "If you are on ssh: run `richpaste setup` on the machine you type on, then reconnect.",
        why);
}

std::atomic<bool> stop_flag = false;

}  // namespace

auto state_dir() -> fs::path { return home_dir() / ".richpaste"; }
auto agent_socket_path() -> fs::path { return state_dir() / "agent.sock"; }
auto ssh_forward_conf() -> fs::path { return state_dir() / "ssh"; }

void serve_one(int fd, const Source& source, bool quiet) {
    Bytes req(wire::request_size);
    if (!read_exact(fd, req, Millis(5000))) return;
    auto r = wire::decode_request(req);
    Bytes out;
    if (!r) {
        out = wire::encode_error(r.error().what);
    } else {
        auto clip = source(r->max_total_bytes, r->hops + 1);
        out = encode_clip(clip, r->max_total_bytes);
        if (!quiet) {
            if (clip)
                std::println(stderr, "richpaste: served {} from {}", summary(clip->items), clip->source);
            else
                std::println(stderr, "richpaste: {}", clip.error().what);
        }
    }
    (void)write_all(fd, out, Millis(30000));
}

auto run_agent(const fs::path& socket, Source source, bool quiet) -> Result<void> {
    if (auto d = ensure_private_dir(socket.parent_path()); !d) return d;
    auto lock_path = socket;
    lock_path += ".lock";
    Fd lock(::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!lock) return fail("open {}: {}", lock_path.string(), errno_str());
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) < 0) return fail("another agent is already running");

    auto listener = unix_listen(socket);
    if (!listener) return std::unexpected(listener.error());
    std::signal(SIGPIPE, SIG_IGN);
    if (!quiet) std::println(stderr, "richpaste: agent listening on {}", socket.string());

    while (!stop_flag) {
        if (!wait_readable(listener->get(), Millis(1000))) continue;
        int c = ::accept4(listener->get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) continue;
        // One thread per paste so a slow link can't block other sessions.
        std::thread([c, source, quiet] {
            Fd conn(c);
            serve_one(conn.get(), source, quiet);
        }).detach();
    }
    return {};
}

auto ensure_agent(const fs::path& self_exe) -> Result<void> {
    if (unix_connect(agent_socket_path())) return {};
    pid_t pid = ::fork();
    if (pid < 0) return fail("fork: {}", errno_str());
    if (pid == 0) {
        ::setsid();
        if (::fork() != 0) ::_exit(0);
        int null = ::open("/dev/null", O_RDWR);
        ::dup2(null, 0);
        ::dup2(null, 1);
        auto log = (state_dir() / "agent.log").string();
        int lf = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        ::dup2(lf >= 0 ? lf : null, 2);
        for (int fd = 3; fd < 256; ++fd) ::close(fd);
        ::execl(self_exe.c_str(), self_exe.c_str(), "agent", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    // Give it a moment so the very first paste doesn't race the bind.
    for (int i = 0; i < 50; ++i) {
        if (unix_connect(agent_socket_path())) return {};
        std::this_thread::sleep_for(Millis(10));
    }
    return {};
}

auto find_forwarded_sockets() -> std::vector<fs::path> {
    std::vector<fs::path> out;
    auto prefix = std::format("{}{}-", forward_prefix, user_name());
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/tmp", ec)) {
        auto name = e.path().filename().string();
        if (name.starts_with(prefix) && name.ends_with(".sock") && ours(e.path())) out.push_back(e.path());
    }
    std::ranges::sort(out, std::greater{}, mtime);
    return out;
}

auto fetch(const fs::path& socket, std::uint64_t max_bytes) -> Result<std::vector<Item>> {
    return fetch_hops(socket, max_bytes, 0);
}

auto paste(std::uint64_t max_bytes) -> Result<Clip> { return paste_impl(max_bytes, 0, in_ssh_session()); }

// The agent reads its own display first, and relays to the next hop if it has none.
auto agent_source() -> Source {
    return [](std::uint64_t max, std::uint32_t hops) { return paste_impl(max, hops, false); };
}

auto ssh_hook(const fs::path& self_exe) -> Result<void> {
    if (auto d = ensure_private_dir(ssh_forward_conf()); !d) return d;
    if (auto a = ensure_agent(self_exe); !a) return a;
    // A fresh name per connection, so old sockets left on the server never block the bind.
    auto line = std::format("RemoteForward /tmp/{}%r-{}.sock {}\n", forward_prefix, random_hex(8),
                            agent_socket_path().string());
    return write_file_atomic(ssh_forward_conf() / "forward.conf", as_view(line));
}

auto ssh_config_snippet(const fs::path& self_exe) -> std::string {
    return std::format(
        "# richpaste: forward your clipboard to every ssh session\n"
        "Match !host github.com,gitlab.com,bitbucket.org,ssh.dev.azure.com exec \"{} ssh-hook\"\n"
        "  Include {}\n"
        "# richpaste end\n",
        self_exe.string(), (ssh_forward_conf() / "forward.conf").string());
}

}  // namespace rp
