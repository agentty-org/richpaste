#include "rp/agent.hpp"
#include "rp/armor.hpp"
#include "rp/cli.hpp"
#include "rp/report.hpp"
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
                 "{2}"
                 "\n"
                 "when nothing reaches your clipboard (e.g. Terminal.app over ssh):\n"
                 "  {0} howto        one-time Mac/Windows setup to paste images as text\n"
                 "  {0} page         a web page that turns a pasted image into text\n"
                 "  {0} encode FILE  FILE as one line of text; paste it anywhere\n"
                 "  {0} decode [DIR] turn such text on stdin back into files\n",
                 n, paste_line, program().extra_usage);
    return 2;
}

auto paste_dir() -> fs::path { return save_dir(); }

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

auto cmd_paste(Args args) -> int {
    fs::path dir = paste_dir();
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string_view a = args[i];
        if ((a == "-o" || a == "--out") && i + 1 < args.size()) dir = args[++i];
        else return usage();
    }
    auto clip = paste_here();
    if (!clip) {
        std::println(stderr, "{}: {}", program().name, clip.error().what);
        return 1;
    }
    print_items(*clip, dir);
    return 0;
}

auto cmd_doctor() -> int {
    const auto d = diagnose();
    const auto& c = d.config;
    std::println("settings       forwards={} display={} server_display={} terminal={} max={}",
                 c.forwards ? "on" : "off", c.display ? "on" : "off", c.server_display ? "on" : "off",
                 terminal_mode_name(c.terminal), human_size(c.max_bytes));
    if (!c.socket.empty()) std::println("socket         {} (only this one is asked)", c.socket.string());
    std::println("state dir      {}", d.state_dir.string());
    std::println("saves to       {}", d.save_dir.string());
    std::println("agent          {}  {}", d.agent, d.agent_alive ? "(answering)" : "(not running)");
    std::println("display env    WAYLAND_DISPLAY={} DISPLAY={}", d.wayland_display.value_or("-"),
                 d.x_display.value_or("-"));
    std::println("ssh session    SSH_CONNECTION={}  TMUX={}", d.ssh_connection.value_or("-"), d.tmux ? "yes" : "no");
    std::println("forwards       {} found", d.forwards.size());
    for (auto& f : d.forwards) std::println("  {}  {}", f.socket.string(), f.live ? "(live)" : "(dead)");
    std::string routes;
    for (auto r : d.plan) routes += (routes.empty() ? "" : " -> ") + std::string{route_name(r)};
    std::println("plan           {}{}{}", routes.empty() ? "(nothing enabled)" : routes, d.situation.ssh ? "  [ssh]" : "",
                 d.situation.tmux ? "  [tmux]" : "");
    if (!d.paste) {
        auto& e = d.paste.error();
        std::println("paste          FAILED ({}): {}", code_name(e.code), e.what);
        for (auto& cause : e.causes)
            std::println("  {:<12} {}", cause.route ? route_name(*cause.route) : "?", code_name(cause.code));
        return 1;
    }
    std::println("paste          ok via {}", d.paste->source);
    for (auto& i : d.paste->items)
        std::println("  {} {} sha256 {}", mime_of(i), human_size(payload(i).size()), hex(sha256(payload(i))).substr(0, 16));
    return 0;
}

auto cmd_setup() -> int {
    auto r = setup();
    if (!r) {
        std::println(stderr, "{}: {}", program().name, r.error().what);
        return 1;
    }
    if (r->change == SetupReport::Change::unchanged)
        std::println("{} is already in {}", program().name, r->ssh_config.string());
    else
        std::println("{} {}:\n\n{}", r->change == SetupReport::Change::updated ? "updated" : "added to the top of",
                     r->ssh_config.string(), r->snippet);
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
