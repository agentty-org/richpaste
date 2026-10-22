#include "rp/agent.hpp"

#include "rp/armor.hpp"
#include "rp/inband.hpp"
#include "rp/sha256.hpp"
#include "rp/wire.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <print>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rp {

namespace fs = std::filesystem;

namespace {

[[maybe_unused]] auto tcp_info_path() -> fs::path { return state_dir() / "agent.tcp"; }

// Only trust sockets that are real sockets owned by us (not symlinks, not planted by others).
[[maybe_unused]] auto ours(const fs::path& p) -> bool {
#ifdef _WIN32
    (void)p;
    return false;
#else
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == ::getuid();
#endif
}

[[maybe_unused]] auto mtime(const fs::path& p) -> std::int64_t {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
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

auto connect(const Endpoint& e) -> Result<Fd> {
    return std::visit(overloaded{
                          [](const UnixEndpoint& u) { return unix_connect(u.path); },
                          [](const TcpEndpoint& t) { return tcp_connect_loopback(t.port); },
                      },
                      e);
}

auto token_of(const Endpoint& e) -> std::string {
    if (auto* t = std::get_if<TcpEndpoint>(&e)) return t->token;
    // A forwarded unix socket may end at a Windows agent that wants a token.
    return env("RICHPASTE_TOKEN").value_or("");
}

// Errors that mean "nobody is listening there", as opposed to a real answer.
auto unreachable(const Error& e) -> bool {
    auto& w = e.what;
    return w.starts_with("connect") || w.starts_with("sending") || w.starts_with("reading reply") ||
           w.starts_with("agent on your machine");
}

// Inside ssh? Asks tmux too, since an old pane keeps stale env.
auto in_ssh_session() -> bool {
    if (env("TMUX")) {
        std::vector<std::string> cmd{"tmux", "show-environment", "SSH_CONNECTION"};
        if (auto s = run_capture(cmd, Millis(1000))) {
            if (s->starts_with("SSH_CONNECTION=")) return true;
            if (s->starts_with("-SSH_CONNECTION")) return false;
        }
    }
    return env("SSH_CONNECTION") || env("SSH_TTY");
}

struct Attempt {
    std::vector<std::string> errors;
};

auto from_forwards(std::uint64_t max, std::uint32_t hops, Attempt& a) -> std::optional<Result<Clip>> {
    for (auto& s : find_forwarded_sockets()) {
        auto r = fetch(UnixEndpoint{s}, max, hops);
        if (r) return Clip{std::move(*r), "ssh " + s.filename().string()};
        // A dead forward leaves its socket file behind; clear it.
        if (r.error().what.contains("Connection refused")) {
            std::error_code ec;
            fs::remove(s, ec);
            continue;
        }
        // The agent answered with a real error (empty clipboard, too big). That's the answer.
        if (!unreachable(r.error())) return std::unexpected(r.error());
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

auto from_terminal(const PasteOptions& opt, Attempt& a) -> std::optional<Result<Clip>> {
    if (!opt.terminal) return std::nullopt;
    if (env("RICHPASTE_NO_TERMINAL")) return std::nullopt;
    inband::Session s = *opt.terminal;
    auto caps = inband::detect(s, Millis(1500));
    auto r = inband::read_clipboard(s, opt.max_bytes, Millis(3000));
    auto via = caps.osc5522 ? "terminal (OSC 5522)" : caps.tmux ? "terminal via tmux (OSC 52)" : "terminal (OSC 52)";
    if (r) return Clip{std::move(*r), via};
    // "Empty" and "refused" are answers; "didn't answer" means try nothing else here.
    if (r.error().what.contains("didn't answer") || r.error().what.contains("did not answer")) {
        a.errors.push_back(std::format("terminal{}: {}", caps.terminal.empty() ? "" : " " + caps.terminal, r.error().what));
        return std::nullopt;
    }
    return std::unexpected(r.error());
}

auto paste_impl(const PasteOptions& opt, std::uint32_t hops, bool prefer_forwards) -> Result<Clip> {
    if (auto s = env("RICHPASTE_SOCK")) {
        auto r = fetch(UnixEndpoint{*s}, opt.max_bytes, hops);
        if (!r) return std::unexpected(r.error());
        return Clip{std::move(*r), "RICHPASTE_SOCK"};
    }
    Attempt a;
    if (prefer_forwards) {
        if (auto r = from_forwards(opt.max_bytes, hops, a)) return std::move(*r);
        // Over ssh, the terminal sits on the machine you type on, so ask it.
        if (auto r = from_terminal(opt, a)) return std::move(*r);
        // Don't fall back to this server's own display: that's not the clipboard you
        // copied into, and pasting it silently gives stale text.
        if (env("RICHPASTE_SERVER_CLIPBOARD")) {
            if (auto r = from_display(opt.max_bytes, a)) return std::move(*r);
        }
        std::string why;
        for (auto& e : a.errors) why += "\n  " + e;
        return fail(
            "can't reach the clipboard on the machine you're typing on.{}\n"
            "Run `{} setup` on that machine (your laptop), then reconnect.",
            why, program().name);
    } else {
        if (auto r = from_display(opt.max_bytes, a)) return std::move(*r);
        if (auto r = from_forwards(opt.max_bytes, hops, a)) return std::move(*r);
        if (auto r = from_terminal(opt, a)) return std::move(*r);
    }
    std::string why;
    for (auto& e : a.errors) why += "\n  " + e;
    return fail(
        "can't reach a clipboard.{}\n"
        "Run `{} setup` on the machine you type on, then reconnect.",
        why, program().name);
}

[[maybe_unused]] auto parse_tcp_info(std::string_view s) -> Result<TcpEndpoint> {
    // "port token\n"
    auto sp = s.find(' ');
    if (sp == std::string_view::npos) return fail("bad agent.tcp");
    unsigned port = 0;
    auto ps = s.substr(0, sp);
    auto [p, ec] = std::from_chars(ps.data(), ps.data() + ps.size(), port);
    if (ec != std::errc{} || port == 0 || port > 65535) return fail("bad agent.tcp port");
    auto tok = s.substr(sp + 1);
    while (!tok.empty() && (tok.back() == '\n' || tok.back() == '\r' || tok.back() == ' ')) tok.remove_suffix(1);
    if (tok.size() < 32) return fail("bad agent.tcp token");
    return TcpEndpoint{static_cast<std::uint16_t>(port), std::string(tok)};
}

// Constant-time compare so the token can't be guessed byte by byte.
auto same(std::string_view a, std::string_view b) -> bool {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

auto serve_loop(Fd listener, Source source, std::string token, bool quiet) -> Result<void> {
    for (;;) {
        auto c = accept_one(listener.get(), Millis(1000));
        if (!c) continue;
        // One thread per paste so a slow link can't block other sessions.
        std::thread([conn = std::move(*c), source, token, quiet]() mutable {
            serve_one(conn.get(), source, token, quiet);
        }).detach();
    }
}

}  // namespace

namespace {
auto program_slot() -> Program& {
    static Program p;
    return p;
}
}  // namespace

void set_program(Program p) { program_slot() = std::move(p); }
auto program() -> const Program& { return program_slot(); }

auto self_command(std::string_view sub) -> std::vector<std::string> {
    auto& p = program();
    std::vector<std::string> out{(p.exe.empty() ? self_exe() : p.exe).string()};
    out.insert(out.end(), p.args.begin(), p.args.end());
    out.emplace_back(sub);
    return out;
}

auto state_dir() -> fs::path {
    if (!program().state_dir.empty()) return program().state_dir;
    if (auto h = env("AGENTTY_HOME"); h && !h->empty()) return fs::path(*h) / "paste";
    return home_dir() / ".agentty" / "paste";
}
auto ssh_forward_conf() -> fs::path { return state_dir() / "ssh"; }

auto local_endpoint() -> Result<Endpoint> {
#ifdef _WIN32
    auto b = read_file(tcp_info_path(), 4096);
    if (!b) return fail("agent is not running");
    auto t = parse_tcp_info(as_string(*b));
    if (!t) return std::unexpected(t.error());
    return Endpoint{*t};
#else
    return Endpoint{UnixEndpoint{state_dir() / "agent.sock"}};
#endif
}

auto describe(const Endpoint& e) -> std::string {
    return std::visit(overloaded{
                          [](const UnixEndpoint& u) { return u.path.string(); },
                          [](const TcpEndpoint& t) { return std::format("127.0.0.1:{}", t.port); },
                      },
                      e);
}

void serve_one(int fd, const Source& source, std::string_view token, bool quiet) {
    Bytes head(wire::request_head);
    if (!read_exact(fd, head, Millis(5000))) return;
    auto tlen = std::to_integer<std::size_t>(head[head.size() - 2]) | std::to_integer<std::size_t>(head.back()) << 8;
    if (tlen > wire::max_token) return;
    Bytes req = head;
    req.resize(head.size() + tlen);
    if (tlen && !read_exact(fd, std::span(req).subspan(head.size()), Millis(5000))) return;
    auto r = wire::decode_request(req);
    Bytes out;
    if (!r) {
        out = wire::encode_error(r.error().what);
    } else if (!token.empty() && !same(r->token, token) && !peer_is_same_user(fd)) {
        // Windows: loopback TCP is open to every local user, so require the token
        // unless the caller is this same Windows user (ssh.exe forwards run as you).
        out = wire::encode_error("wrong agent token");
        if (!quiet) std::println(stderr, "{}: refused a request with a wrong token", program().name);
    } else {
        auto clip = source(r->max_total_bytes, r->hops + 1);
        out = encode_clip(clip, r->max_total_bytes);
        if (!quiet) {
            if (clip)
                std::println(stderr, "{}: served {} from {}", program().name, summary(clip->items), clip->source);
            else
                std::println(stderr, "{}: {}", program().name, clip.error().what);
        }
    }
    (void)write_all(fd, out, Millis(30000));
}

auto run_agent_at(const fs::path& socket, Source source, bool quiet) -> Result<void> {
    if (auto d = ensure_private_dir(socket.parent_path()); !d) return d;
    auto lock_path = socket;
    lock_path += ".lock";
    auto lock = FileLock::try_lock(lock_path);
    if (!lock) return std::unexpected(lock.error());
    auto listener = unix_listen(socket);
    if (!listener) return std::unexpected(listener.error());
    if (!quiet) std::println(stderr, "{}: agent listening on {}", program().name, socket.string());
    return serve_loop(std::move(*listener), std::move(source), {}, quiet);
}

auto run_agent(Source source, bool quiet) -> Result<void> {
#ifdef _WIN32
    if (auto d = ensure_private_dir(state_dir()); !d) return d;
    auto lock = FileLock::try_lock(state_dir() / "agent.lock");
    if (!lock) return std::unexpected(lock.error());
    // A fixed port lets ssh's RemoteForward line stay the same; fall back to any port.
    std::uint16_t want = 47233;
    if (auto p = env("RICHPASTE_PORT")) want = static_cast<std::uint16_t>(std::stoi(*p));
    auto l = tcp_listen_loopback(want);
    if (!l) l = tcp_listen_loopback(0);
    if (!l) return std::unexpected(l.error());
    auto token = random_hex(24);
    auto info = std::format("{} {}\n", l->second, token);
    if (auto w = write_file_atomic(tcp_info_path(), as_view(info)); !w) return w;
    if (!quiet) std::println(stderr, "{}: agent listening on 127.0.0.1:{}", program().name, l->second);
    return serve_loop(std::move(l->first), std::move(source), token, quiet);
#else
    return run_agent_at(state_dir() / "agent.sock", std::move(source), quiet);
#endif
}

auto agent_alive() -> bool {
    auto e = local_endpoint();
    return e && connect(*e).has_value();
}

auto ensure_agent() -> Result<void> {
    if (agent_alive()) return {};
    if (auto d = ensure_private_dir(state_dir()); !d) return d;
    auto cmd = self_command("agent");
    std::span<const std::string> args(cmd.begin() + 1, cmd.end());
    if (auto s = spawn_detached(cmd.front(), args, state_dir() / "agent.log"); !s) return s;
    // Give it a moment so the very first paste doesn't race the bind.
    for (int i = 0; i < 100; ++i) {
        if (agent_alive()) return {};
        std::this_thread::sleep_for(Millis(10));
    }
    return {};
}

auto find_forwarded_sockets() -> std::vector<fs::path> {
    std::vector<fs::path> out;
#ifndef _WIN32
    auto prefix = std::format("{}{}-", forward_prefix, user_name());
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/tmp", ec)) {
        auto name = e.path().filename().string();
        if (name.starts_with(prefix) && name.ends_with(".sock") && ours(e.path())) out.push_back(e.path());
    }
    std::ranges::sort(out, std::greater{}, mtime);
#endif
    return out;
}

auto fetch(const Endpoint& e, std::uint64_t max, std::uint32_t hops) -> Result<std::vector<Item>> {
    auto fd = connect(e);
    if (!fd) return std::unexpected(fd.error());
    auto req = wire::encode_request({wire::Op::clipboard, max, hops, token_of(e)});
    if (auto w = write_all(fd->get(), req, Millis(5000)); !w) return fail("sending request: {}", w.error().what);
    // Idle timeout covers the agent reading the clipboard and slow links.
    auto resp = read_all(fd->get(), static_cast<std::size_t>(max) + (1u << 20), Millis(30000));
    if (!resp) return fail("reading reply: {}", resp.error().what);
    if (resp->empty()) return fail("agent on your machine isn't running (forward closed with no reply)");
    return wire::decode_response(*resp);
}

auto paste(const PasteOptions& opt) -> Result<Clip> {
    auto clip = paste_impl(opt, 0, in_ssh_session());
    if (!clip) return clip;
    // Whatever route it came by, an image or file copied as text arrives as itself.
    std::vector<std::string> errors;
    clip->items = armor::expand(std::move(clip->items), &errors);
    if (!errors.empty() && clip->items.empty()) return fail("{}", errors.front());
    return clip;
}

// The agent reads its own display first, and relays to the next hop if it has none.
auto agent_source() -> Source {
    return [](std::uint64_t max, std::uint32_t hops) {
        PasteOptions o;
        o.max_bytes = max;
        return paste_impl(o, hops, false);
    };
}

auto remote_forward() -> Result<std::pair<std::string, std::string>> {
    if (auto a = ensure_agent(); !a) return std::unexpected(a.error());
    auto e = local_endpoint();
    if (!e) return std::unexpected(e.error());
    // A fresh name per connection, so old sockets left on the server never block the bind.
    // The remote end is always a unix socket in /tmp; the local end is ours.
    auto remote = std::format("/tmp/{}%r-{}.sock", forward_prefix, random_hex(8));
    auto local = std::visit(overloaded{
                                [](const UnixEndpoint& u) { return u.path.string(); },
                                [](const TcpEndpoint& t) { return std::format("127.0.0.1:{}", t.port); },
                            },
                            *e);
    return std::pair{remote, local};
}

auto ssh_hook() -> Result<void> {
    if (auto d = ensure_private_dir(ssh_forward_conf()); !d) return d;
    auto f = remote_forward();
    if (!f) return std::unexpected(f.error());
    auto line = std::format("RemoteForward {} {}\n", f->first, f->second);
    return write_file_atomic(ssh_forward_conf() / "forward.conf", as_view(line));
}

auto ssh_config_snippet() -> std::string {
    auto cmd = self_command("ssh-hook");
    auto exe = cmd.front();
    auto conf = (ssh_forward_conf() / "forward.conf").string();
#ifdef _WIN32
    // OpenSSH for Windows runs Match exec through cmd.exe and wants forward slashes.
    std::ranges::replace(exe, '\\', '/');
    std::ranges::replace(conf, '\\', '/');
#endif
    // The exe is quoted; the rest are plain words like "paste" and "ssh-hook".
    std::string line = "\\\"" + exe + "\\\"";
    for (std::size_t i = 1; i < cmd.size(); ++i) line += " " + cmd[i];
    return std::format(
        "# richpaste: forward your clipboard to every ssh session\n"
        "Match !host github.com,gitlab.com,bitbucket.org,ssh.dev.azure.com exec \"{}\"\n"
        "  Include \"{}\"\n"
        "# richpaste end\n",
        line, conf);
}

}  // namespace rp
