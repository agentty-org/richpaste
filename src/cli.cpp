#include "rp/agent.hpp"
#include "rp/armor.hpp"
#include "rp/cli.hpp"
#include "rp/sha256.hpp"
#include "rp/store.hpp"
#include "rp/term.hpp"

#include <algorithm>
#include <cstdio>
#include <print>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace rp;

namespace {

using Args = std::span<const std::string>;

auto usage() -> int {
    auto& n = program().name;
    // When paste is the default, `<name> [-o DIR]` is the short form.
    auto paste_line = program().default_command == "paste" ? "" : " paste";
    std::println(stderr,
                 "{0}: paste anything into a terminal app, even over ssh and tmux\n"
                 "\n"
                 "on the machine you type on:\n"
                 "  {0} setup        add the ssh hook to ~/.ssh/config and start the agent (once)\n"
                 "  {0} agent        run the clipboard agent in the foreground\n"
                 "\n"
                 "anywhere (local or over ssh):\n"
                 "  {0}{1} [-o DIR]  print text, or save images/files and print paths\n"
                 "  {0} doctor       show what paste would use and why\n"
                 "\n"
                 "when nothing reaches your clipboard (e.g. Terminal.app over ssh):\n"
                 "  {0} howto        one-time Mac/Windows setup to paste images as text\n"
                 "  {0} page         a web page that turns a pasted image into text\n"
                 "  {0} encode FILE  FILE as one line of text; paste it anywhere\n"
                 "  {0} decode [DIR] turn such text on stdin back into files\n",
                 n, paste_line);
    return 2;
}

auto paste_dir() -> fs::path { return state_dir() / "pasted"; }

void print_items(const Clip& clip, const fs::path& dir) {
    auto pasted = store(clip.items, dir);
    if (!pasted) {
        std::println(stderr, "{}: {}", program().name, pasted.error().what);
        return;
    }
    for (auto& part : pasted->parts)
        std::visit(overloaded{
                       [](const std::string& t) { std::print("{}", t); },
                       [](const Saved& s) { std::println("{}", s.path.string()); },
                   },
                   part);
    std::fflush(stdout);
}

// Pastes, asking the terminal as a last resort when we have one.
auto paste_with_terminal(std::uint64_t max) -> Result<Clip> {
    PasteOptions o;
    o.max_bytes = max;
    auto tty = term::Tty::open();
    if (tty) o.terminal = tty->session();
    return paste(o);
}

auto cmd_paste(Args args) -> int {
    fs::path dir = paste_dir();
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string_view a = args[i];
        if ((a == "-o" || a == "--out") && i + 1 < args.size()) dir = args[++i];
        else return usage();
    }
    auto clip = paste_with_terminal(default_max_bytes);
    if (!clip) {
        std::println(stderr, "{}: {}", program().name, clip.error().what);
        return 1;
    }
    print_items(*clip, dir);
    return 0;
}

auto cmd_doctor() -> int {
    auto ep = local_endpoint();
    std::println("agent          {}  {}", ep ? describe(*ep) : "-", agent_alive() ? "(answering)" : "(not running)");
    std::println("display env    WAYLAND_DISPLAY={} DISPLAY={}", env("WAYLAND_DISPLAY").value_or("-"),
                 env("DISPLAY").value_or("-"));
    std::println("ssh session    SSH_CONNECTION={}  TMUX={}", env("SSH_CONNECTION").value_or("-"),
                 env("TMUX") ? "yes" : "no");
    auto socks = find_forwarded_sockets();
    std::println("forwards       {} found", socks.size());
    for (auto& s : socks) std::println("  {}  {}", s.string(), unix_connect(s) ? "(live)" : "(dead)");
    auto clip = paste_with_terminal(64 << 20);
    if (!clip) {
        std::println("paste          FAILED: {}", clip.error().what);
        return 1;
    }
    std::println("paste          ok via {}", clip->source);
    for (auto& i : clip->items)
        std::println("  {} {} sha256 {}", mime_of(i), human_size(payload(i).size()), hex(sha256(payload(i))).substr(0, 16));
    return 0;
}

auto cmd_setup() -> int {
    auto cfg = home_dir() / ".ssh" / "config";
    auto snippet = ssh_config_snippet();
    std::string current;
    if (auto b = read_file(cfg, 4 << 20)) current = as_string(*b);
    // An older block may point at another binary (richpaste vs agentty paste),
    // so swap it for ours instead of leaving it.
    constexpr std::string_view end_mark = "# richpaste end\n";
    auto a = current.find("# richpaste:");
    auto b = a == std::string::npos ? a : current.find(end_mark, a);
    if (b != std::string::npos && current.substr(a, b + end_mark.size() - a) == snippet) {
        std::println("{} is already in {}", program().name, cfg.string());
    } else {
        if (auto d = ensure_private_dir(cfg.parent_path()); !d) {
            std::println(stderr, "{}: {}", program().name, d.error().what);
            return 1;
        }
        std::string out;
        if (b != std::string::npos)
            out = current.substr(0, a) + snippet + current.substr(b + end_mark.size());
        else  // Match blocks must not end up inside someone's Host block, so prepend.
            out = snippet + "\n" + current;
        if (auto w = write_file_atomic(cfg, as_view(out)); !w) {
            std::println(stderr, "{}: {}", program().name, w.error().what);
            return 1;
        }
        std::println("{} {}:\n\n{}", b != std::string::npos ? "updated" : "added to the top of", cfg.string(), snippet);
    }
    if (auto r = ensure_agent(); !r) {
        std::println(stderr, "{}: starting agent: {}", program().name, r.error().what);
        return 1;
    }
    std::println("agent is running. new ssh sessions will carry your clipboard.");
    std::println("the server needs {} too; nothing else to set up there.", program().name);
    return 0;
}

auto cmd_howto() -> int {
    std::println(
        "Ctrl+V in a terminal app runs on this server. It can't read your Mac's clipboard:\n"
        "Terminal.app only ever sends text, and never answers programs that ask for the clipboard.\n"
        "So the image has to be turned into text on the Mac, before it's pasted. That needs one\n"
        "keyboard shortcut on the Mac, made from built-in parts. Nothing to install.\n"
        "\n"
        "One-time setup (about a minute):\n"
        "  1. Open the Shortcuts app, make a new shortcut called \"Rich paste\".\n"
        "  2. Add the action \"Run Shell Script\" and paste the script below into it.\n"
        "  3. Shortcut details (i) > Add Keyboard Shortcut > press Ctrl+Cmd+V.\n"
        "  4. First run only: allow Shortcuts in System Settings > Privacy > Accessibility\n"
        "     (it needs that to press Cmd+V for you).\n"
        "\n"
        "Then: copy a screenshot or a file, click the ssh window, press Ctrl+Cmd+V.\n"
        "It pastes the image as text, the app turns it back into the image, and your\n"
        "clipboard is put back the way it was. Plain text just pastes normally.\n"
        "\n"
        "---- script ----\n{}\n---- end ----\n"
        "\n"
        "Windows laptop (PowerShell), run after copying, then paste with Ctrl+Shift+V:\n{}\n"
        "\n"
        "Any OS, nothing at all: `{} page > rp.html`, open it in a browser, paste the\n"
        "image there, then paste the text it copies into the terminal.",
        armor::mac_hotkey_script(), armor::windows_snippet(), program().name);
    return 0;
}

}  // namespace

namespace rp::cli {

auto run(std::span<const std::string> in) -> int {
    platform_init();
    if (!in.empty() && (in[0] == "help" || in[0] == "-h" || in[0] == "--help")) return usage();
    // No subcommand (or only flags): run the default one, if there is one.
    std::vector<std::string> owned;
    auto& def = program().default_command;
    if (!def.empty() && (in.empty() || in[0].starts_with('-'))) {
        owned.push_back(def);
        owned.insert(owned.end(), in.begin(), in.end());
    }
    Args args = owned.empty() ? in : Args(owned);
    if (args.empty()) return usage();
    std::string_view cmd = args[0];
    if (cmd == "agent") {
        auto r = run_agent(agent_source(), false);
        if (!r) {
            std::println(stderr, "{}: {}", program().name, r.error().what);
            return 1;
        }
        return 0;
    }
    if (cmd == "ssh-hook") {
        // ssh runs this on every connection; it must never print to stdout or fail loudly.
        if (auto r = ssh_hook(); !r) std::println(stderr, "{}: {}", program().name, r.error().what);
        return 0;  // 0 = "match", so ssh includes the forward line
    }
    if (cmd == "setup") return cmd_setup();
    if (cmd == "howto") return cmd_howto();
    if (cmd == "page") {
        std::print("{}", armor::html_page());
        return 0;
    }
    if (cmd == "mac-snippet") {
        std::println("{}", armor::mac_snippet());
        return 0;
    }
    if (cmd == "mac-hotkey") {
        std::println("{}", armor::mac_hotkey_script());
        return 0;
    }
    if (cmd == "win-snippet") {
        std::println("{}", armor::windows_snippet());
        return 0;
    }
    if (cmd == "decode") {
        // Turns armored text on stdin back into files (for scripts and tests).
        std::string text;
        char buf[65536];
        while (auto n = std::fread(buf, 1, sizeof(buf), stdin)) text.append(buf, n);
        auto sp = armor::split(text);
        std::vector<Item> items;
        for (auto& part : sp.parts)
            if (auto* it = std::get_if<Item>(&part)) items.push_back(*it);
        for (auto& e : sp.errors) std::println(stderr, "{}: {}", program().name, e);
        print_items(Clip{items, "text"}, args.size() > 1 ? fs::path(args[1]) : paste_dir());
        return sp.errors.empty() && !items.empty() ? 0 : 1;
    }
    if (cmd == "encode") {
        // A file as one line of text, to paste through a text-only terminal.
        if (args.size() < 2) return usage();
        auto data = read_file(args[1], 1u << 30);
        if (!data) return 1;
        auto mime = mime_for_name(fs::path(args[1]).filename().string());
        Item it = mime.is_image() ? Item{Image{mime, *data}}
                                  : Item{File{FileName::sanitize(fs::path(args[1]).filename().string()), mime, *data}};
        std::print("{}", armor::encode(it));
        return 0;
    }
    if (cmd == "paste") return cmd_paste(args);
    if (cmd == "doctor") return cmd_doctor();
    return usage();
}

}  // namespace rp::cli
