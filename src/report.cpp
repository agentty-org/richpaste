// The facts behind `doctor` and `setup`, as data (see rp/report.hpp).

#include "rp/report.hpp"

#include "rp/io.hpp"
#include "rp/term.hpp"

namespace fs = std::filesystem;

namespace rp {

auto terminal_mode_name(TerminalMode m) -> std::string_view {
    switch (m) {
        case TerminalMode::automatic: return "auto";
        case TerminalMode::osc5522: return "osc5522";
        case TerminalMode::osc52: return "osc52";
        case TerminalMode::off: return "off";
    }
    return "?";
}

auto save_dir() -> fs::path {
    if (auto d = config().save_dir; !d.empty()) return d;
    return state_dir() / "pasted";
}

auto paste_here() -> Result<Clip> {
    PasteOptions o;
    auto tty = config().terminal == TerminalMode::off ? Result<term::Tty>(fail("off")) : term::Tty::open();
    if (tty) o.terminal = tty->session();
    return paste(o);
}

auto diagnose() -> Diagnosis {
    Diagnosis d;
    d.config = config();
    d.state_dir = state_dir();
    d.save_dir = save_dir();
    auto ep = local_endpoint();
    d.agent = ep ? describe(*ep) : "-";
    d.agent_alive = agent_alive();
    d.wayland_display = env("WAYLAND_DISPLAY");
    d.x_display = env("DISPLAY");
    d.ssh_connection = env("SSH_CONNECTION");
    d.tmux = env("TMUX").has_value();
    for (auto& s : find_forwarded_sockets()) d.forwards.push_back({s, unix_connect(s).has_value()});
    d.situation = sense(PasteOptions{});
    d.plan = plan(d.situation, d.config);
    d.paste = paste_here();
    return d;
}

auto setup() -> Result<SetupReport> {
    SetupReport out;
    out.ssh_config = home_dir() / ".ssh" / "config";
    out.snippet = ssh_config_snippet();
    std::string current;
    if (auto b = read_file(out.ssh_config, 4 << 20)) current = as_string(*b);
    // An older block may point at another binary (richpaste vs agentty paste),
    // so swap it for ours instead of leaving it.
    constexpr std::string_view end_mark = "# richpaste end\n";
    auto a = current.find("# richpaste:");
    auto b = a == std::string::npos ? a : current.find(end_mark, a);
    if (b == std::string::npos || current.substr(a, b + end_mark.size() - a) != out.snippet) {
        if (auto dir = ensure_private_dir(out.ssh_config.parent_path()); !dir) return std::unexpected(dir.error());
        std::string next;
        if (b != std::string::npos)
            next = current.substr(0, a) + out.snippet + current.substr(b + end_mark.size());
        else  // Match blocks must not end up inside someone's Host block, so prepend.
            next = out.snippet + "\n" + current;
        if (auto w = write_file_atomic(out.ssh_config, as_view(next)); !w) return std::unexpected(w.error());
        out.change = b != std::string::npos ? SetupReport::Change::updated : SetupReport::Change::added;
    }
    if (auto r = ensure_agent(); !r) return context(r.error(), "starting agent");
    return out;
}

}  // namespace rp
