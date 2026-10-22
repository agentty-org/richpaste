#include "rp/agent.hpp"

#include "rp/armor.hpp"
#include "rp/inband.hpp"
#include "rp/sha256.hpp"
#include "rp/wire.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <mutex>
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
    if (!clip) return wire::encode_error(clip.error().what, clip.error().code);
    std::uint64_t total = 0;
    for (auto& i : clip->items) total += wire::encoded_size(i);
    if (total > max)
        return wire::encode_error(std::format("clipboard is {} (limit {})", human_size(total), human_size(max)), Code::too_big);
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
    return config().token;
}

// Sends one request on an open connection and reads the answer. Any failure
// to talk is `unreachable`; an answer keeps the agent's own code.
auto exchange(Fd fd, const Endpoint& e, std::uint64_t max, std::uint32_t hops) -> Result<std::vector<Item>> {
    auto req = wire::encode_request({wire::Op::clipboard, max, hops, token_of(e)});
    if (auto w = write_all(fd.get(), req, Millis(5000)); !w)
        return fail(Code::unreachable, "sending request: {}", w.error().what);
    // Idle timeout covers the agent reading the clipboard and slow links.
    auto resp = read_all(fd.get(), static_cast<std::size_t>(max) + (1u << 20), Millis(30000));
    if (!resp) {
        if (resp.error().code == Code::too_big) return std::unexpected(resp.error());
        return fail(Code::unreachable, "reading reply: {}", resp.error().what);
    }
    if (resp->empty()) return fail(Code::unreachable, "agent on your machine isn't running (forward closed with no reply)");
    return wire::decode_response(*resp);
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

// A live tmux pane we can ask things of. $TMUX alone can be stale.
auto in_tmux_pane() -> bool {
    if (!env("TMUX") || !env("TMUX_PANE")) return false;
    std::vector<std::string> q{"tmux", "display", "-p", "-t", *env("TMUX_PANE"), "#{pane_id}"};
    return run_capture(q, Millis(1000)).has_value();
}

auto tagged(Error e, Route r) -> Error {
    e.route = r;
    return e;
}

// A failure that means "this route has nothing to say", so the next one is
// tried. Anything else (empty, too big, refused, corrupt) is the answer.
auto moves_on(Route r, Code c) -> bool {
    // tmux only fetches text (OSC 52) and can fail for its own reasons; the
    // terminal may still answer, with an image even.
    if (r == Route::tmux_buffer) return c != Code::too_big;
    switch (c) {
        case Code::unreachable:
        case Code::no_display:
        case Code::no_answer:
        case Code::needs_terminal:
        case Code::no_route: return true;
        default: return false;
    }
}

auto from_socket(const PasteOptions& opt, std::uint32_t hops) -> Result<Clip> {
    auto s = config().socket;
    auto r = fetch(UnixEndpoint{s}, opt.max_bytes, hops);
    if (!r) return std::unexpected(r.error());
    return Clip{std::move(*r), "agent at " + s.string(), Route::socket};
}

auto from_forwards(const PasteOptions& opt, std::uint32_t hops) -> Result<Clip> {
    std::vector<Error> tried;
    for (auto& s : find_forwarded_sockets()) {
        Endpoint e = UnixEndpoint{s};
        auto fd = connect(e);
        if (!fd) {
            // A dead forward leaves its socket file behind; clear it.
            std::error_code ec;
            fs::remove(s, ec);
            continue;
        }
        auto r = exchange(std::move(*fd), e, opt.max_bytes, hops);
        if (r) return Clip{std::move(*r), "ssh " + s.filename().string(), Route::forward};
        if (r.error().code != Code::unreachable) return std::unexpected(r.error());
        tried.push_back(context(r.error(), s.filename().string()).error());
    }
    if (tried.empty()) return fail(Code::unreachable, "no ssh forward from your machine (run `{} setup` there)", program().name);
    Error e{Code::unreachable, tried.front().what, {}, {}};
    for (std::size_t i = 1; i < tried.size(); ++i) e.what += "; " + tried[i].what;
    return std::unexpected(std::move(e));
}

auto from_display(const PasteOptions& opt) -> Result<Clip> {
    auto r = read_clipboard(opt.max_bytes);
    if (r) r->route = Route::display;
    return r;
}

// tmux fetches the outer terminal's clipboard into a paste buffer and we read
// that buffer. The reply never passes through our input (where keyboard modes
// like extended-keys, slow links and stray terminal reports mangle a big
// reply), and no tty is needed. Text only; a richpaste:1: line in it becomes
// the image or file in paste().
auto from_tmux_buffer(const PasteOptions& opt) -> Result<Clip> {
    auto t = inband::tmux_refresh_clipboard(Millis(opt.tmux_timeout_ms));
    if (!t) return std::unexpected(t.error());
    if (t->empty()) return fail(Code::empty, "clipboard is empty");
    if (t->size() > opt.max_bytes) return fail(Code::too_big, "clipboard is larger than the {} limit", human_size(opt.max_bytes));
    return Clip{{Text::repair(*t)}, "terminal via tmux buffer", Route::tmux_buffer};
}

auto from_terminal(const PasteOptions& opt) -> Result<Clip> {
    if (!opt.terminal) return fail(Code::needs_terminal, "no terminal session to ask");
    inband::Session s = *opt.terminal;
    auto caps = inband::detect(s, Millis(1500));
    auto r = inband::read_clipboard(s, opt.max_bytes, Millis(3000));
    if (!r) return context(r.error(), caps.terminal.empty() ? "terminal" : "terminal " + caps.terminal);
    auto via = caps.osc5522 ? "terminal (OSC 5522)" : caps.tmux ? "terminal via tmux (OSC 52)" : "terminal (OSC 52)";
    return Clip{std::move(*r), via, Route::terminal};
}

auto try_route(Route r, const PasteOptions& opt, std::uint32_t hops) -> Result<Clip> {
    switch (r) {
        case Route::socket: return from_socket(opt, hops);
        case Route::forward: return from_forwards(opt, hops);
        case Route::display: return from_display(opt);
        case Route::tmux_buffer: return from_tmux_buffer(opt);
        case Route::terminal: return from_terminal(opt);
    }
    return fail("unknown route");
}

auto walk(const std::vector<Route>& routes, const PasteOptions& opt, std::uint32_t hops, bool remote) -> Result<Clip> {
    std::vector<Error> causes;
    for (auto route : routes) {
        auto r = try_route(route, opt, hops);
        if (r) return r;
        auto e = tagged(std::move(r.error()), route);
        if (!moves_on(route, e.code)) return std::unexpected(std::move(e));
        causes.push_back(std::move(e));
    }
    std::string why;
    for (auto& e : causes) why += std::format("\n  {}: {}", route_name(*e.route), e.what);
    // Only the terminal is left and we may not ask it: the caller can, through
    // its own input.
    const bool ask_terminal = std::ranges::any_of(causes, [](auto& e) { return e.code == Code::needs_terminal; });
    Error out{ask_terminal ? Code::needs_terminal : Code::no_route, "", std::move(causes), {}};
    out.what = remote ? std::format("can't reach the clipboard on the machine you're typing on.{}\n"
                                    "Run `{} setup` on that machine (your laptop), then reconnect.",
                                    why, program().name)
                      : std::format("can't reach a clipboard.{}\nRun `{} setup` on the machine you type on, then reconnect.",
                                    why, program().name);
    return std::unexpected(std::move(out));
}

auto paste_impl(const PasteOptions& opt, std::uint32_t hops, const Situation& s) -> Result<Clip> {
    return walk(plan(s, config()), opt, hops, s.ssh);
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
// A Config is small and swapped rarely; a mutex is portable everywhere
// (std::atomic<shared_ptr> is missing from libc++, and the free atomic_load
// overloads are deprecated in C++20).
struct ConfigSlot {
    std::mutex m;
    Config c;
};
auto config_slot() -> ConfigSlot& {
    static ConfigSlot s;
    return s;
}
}  // namespace

void set_program(Program p) { program_slot() = std::move(p); }
auto program() -> const Program& { return program_slot(); }
void set_config(Config c) {
    auto& s = config_slot();
    std::lock_guard g{s.m};
    s.c = std::move(c);
}
auto config() -> Config {
    auto& s = config_slot();
    std::lock_guard g{s.m};
    return s.c;
}

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
    if (!b) return fail(Code::unreachable, "agent is not running");
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
        out = wire::encode_error(r.error().what, Code::corrupt);
    } else if (!token.empty() && !same(r->token, token) && !peer_is_same_user(fd)) {
        // Windows: loopback TCP is open to every local user, so require the token
        // unless the caller is this same Windows user (ssh.exe forwards run as you).
        out = wire::encode_error("wrong agent token", Code::refused);
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
    std::uint16_t want = config().agent_port;
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
    return exchange(std::move(*fd), e, max, hops);
}

auto sense(const PasteOptions& opt) -> Situation {
    return {.ssh = in_ssh_session(), .tmux = opt.tmux_buffer && in_tmux_pane()};
}

auto plan(const Situation& s, const Config& c) -> std::vector<Route> {
    if (!c.socket.empty()) return {Route::socket};
    const bool term = c.terminal != TerminalMode::off;
    std::vector<Route> out;
    auto add = [&](bool on, Route r) {
        if (on) out.push_back(r);
    };
    if (s.ssh) {
        add(c.forwards, Route::forward);
        add(term && s.tmux, Route::tmux_buffer);
        add(term, Route::terminal);
        // This server's own clipboard isn't the one you copied into, so only
        // when asked for.
        add(c.display && c.server_display, Route::display);
    } else {
        add(c.display, Route::display);
        add(c.forwards, Route::forward);
        add(term && s.tmux, Route::tmux_buffer);
        add(term, Route::terminal);
    }
    return out;
}

auto paste(const PasteOptions& opt) -> Result<Clip> {
    auto clip = paste_impl(opt, 0, sense(opt));
    if (!clip) return clip;
    if (!config().decode_text) return clip;
    // Whatever route it came by, an image or file copied as text arrives as itself.
    std::vector<std::string> errors;
    clip->items = armor::expand(std::move(clip->items), &errors);
    if (!errors.empty() && clip->items.empty()) return fail(Code::corrupt, "{}", errors.front());
    return clip;
}

// The agent reads its own display first, and relays to the next hop if it has none.
auto agent_source() -> Source {
    return [](std::uint64_t max, std::uint32_t hops) {
        PasteOptions o;
        o.max_bytes = max;
        o.tmux_buffer = false;  // a background agent has no pane to ask
        auto routes = plan(Situation{}, config());
        std::erase(routes, Route::terminal);
        return walk(routes, o, hops, false);
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
