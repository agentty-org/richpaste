#include "rp/agent.hpp"
#include "rp/composer.hpp"
#include "rp/sha256.hpp"
#include "rp/term.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace rp;

namespace {

auto self_exe() -> fs::path {
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path("richpaste") : p;
}

auto usage() -> int {
    std::println(stderr,
                 "richpaste: paste anything into a terminal app, even over ssh and tmux\n"
                 "\n"
                 "on the machine you type on:\n"
                 "  richpaste setup              add the ssh hook to ~/.ssh/config (once)\n"
                 "  richpaste agent              run the clipboard agent in the foreground\n"
                 "\n"
                 "anywhere (local or over ssh):\n"
                 "  richpaste demo               an input box; Ctrl+V pastes text, images, files\n"
                 "  richpaste paste [-o DIR]     print text, or save images/files and print paths\n"
                 "  richpaste doctor             show what paste would use and why\n");
    return 2;
}

void print_items(const Clip& clip, const fs::path& dir) {
    auto pasted = store(clip.items, dir);
    if (!pasted) {
        std::println(stderr, "richpaste: {}", pasted.error().what);
        return;
    }
    for (auto& part : pasted->parts)
        std::visit(overloaded{
                       [](const std::string& t) { std::print("{}", t); },
                       [](const Chip& c) { std::println("{}", c.path.string()); },
                   },
                   part);
    std::fflush(stdout);
}

auto paste_dir() -> fs::path { return state_dir() / "pasted"; }

auto cmd_paste(int argc, char** argv) -> int {
    fs::path dir = paste_dir();
    for (int i = 2; i < argc; ++i) {
        std::string_view a = argv[i];
        if ((a == "-o" || a == "--out") && i + 1 < argc) dir = argv[++i];
        else return usage();
    }
    auto clip = paste();
    if (!clip) {
        std::println(stderr, "richpaste: {}", clip.error().what);
        return 1;
    }
    print_items(*clip, dir);
    return 0;
}

auto cmd_doctor() -> int {
    std::println("agent socket   {}  {}", agent_socket_path().string(),
                 unix_connect(agent_socket_path()) ? "(answering)" : "(not running)");
    std::println("display env    WAYLAND_DISPLAY={} DISPLAY={}", env("WAYLAND_DISPLAY").value_or("-"),
                 env("DISPLAY").value_or("-"));
    std::println("ssh session    SSH_CONNECTION={}  TMUX={}", env("SSH_CONNECTION").value_or("-"),
                 env("TMUX") ? "yes" : "no");
    auto socks = find_forwarded_sockets();
    std::println("forwards       {} found", socks.size());
    for (auto& s : socks) {
        auto ok = unix_connect(s);
        std::println("  {}  {}", s.string(), ok ? "(live)" : "(dead)");
    }
    auto clip = paste(64 << 20);
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
    auto exe = self_exe();
    auto cfg = home_dir() / ".ssh" / "config";
    auto snippet = ssh_config_snippet(exe);
    std::string current;
    if (auto b = read_file(cfg, 4 << 20)) current = as_string(*b);
    if (current.contains("# richpaste:")) {
        std::println("richpaste is already in {}", cfg.string());
    } else {
        std::error_code ec;
        fs::create_directories(cfg.parent_path(), ec);
        ::chmod(cfg.parent_path().c_str(), 0700);
        // Match blocks must not end up inside someone's Host block, so prepend.
        auto out = snippet + "\n" + current;
        if (auto w = write_file_atomic(cfg, as_view(out)); !w) {
            std::println(stderr, "richpaste: {}", w.error().what);
            return 1;
        }
        std::println("added to the top of {}:\n\n{}", cfg.string(), snippet);
    }
    if (auto a = ensure_agent(exe); !a) {
        std::println(stderr, "richpaste: starting agent: {}", a.error().what);
        return 1;
    }
    std::println("agent is running. new ssh sessions will carry your clipboard.");
    std::println("on the server, copy this same binary into PATH (it has no dependencies).");
    return 0;
}

// ---- demo input box ----

auto cmd_demo() -> int {
    auto raw = term::Raw::enter(STDIN_FILENO);
    if (!raw) {
        std::println(stderr, "richpaste: {}", raw.error().what);
        return 1;
    }
    std::signal(SIGPIPE, SIG_IGN);
    Composer box;
    term::Parser parser;
    term::View view;
    view.status = "Ctrl+V paste  Enter send  Alt+Enter newline  Ctrl+C quit";
    int rows_above = 0;
    auto out = [](std::string_view s) { (void)write_all(STDOUT_FILENO, as_view(s), Millis(1000)); };
    auto draw = [&] {
        view.width = term::size(STDOUT_FILENO).first;
        auto [s, r] = term::render(box, view, rows_above);
        out(s);
        rows_above = r;
    };
    auto set_status = [&](std::string s) {
        view.status = std::move(s);
        draw();
    };

    auto do_paste = [&] {
        set_status("pasting...");
        auto clip = paste();
        if (!clip) {
            auto msg = clip.error().what;
            std::ranges::replace(msg, '\n', ' ');
            set_status("paste failed: " + msg);
            return;
        }
        auto pasted = store(clip->items, paste_dir());
        if (!pasted) {
            set_status("paste failed: " + pasted.error().what);
            return;
        }
        for (auto& part : pasted->parts)
            std::visit(overloaded{
                           [&](const std::string& t) { box.insert_text(t); },
                           [&](const Chip& c) { box.insert_chip(c); },
                       },
                       part);
        set_status(std::format("pasted from {}", clip->source));
    };

    auto submit = [&] {
        // Move below the box, print what was sent, start a fresh box.
        view.status.clear();
        draw();
        out("\r\n");
        std::string sent = box.to_text();
        std::string shown;
        for (char ch : sent) shown += ch == '\n' ? std::string("\r\n  ") : std::string(1, ch);
        out(std::format("\x1b[2msent:\x1b[22m {}\r\n", shown));
        for (auto& c : box.chips())
            out(std::format("  \x1b[2m{} {} sha256:{}\x1b[22m\r\n", c.mime, human_size(c.size), c.sha256.substr(0, 16)));
        box.clear();
        rows_above = 0;
        set_status("Ctrl+V paste  Enter send  Alt+Enter newline  Ctrl+C quit");
    };

    draw();
    std::array<std::byte, 4096> buf;
    for (;;) {
        auto n = read_some(STDIN_FILENO, buf, parser.pending() ? Millis(30) : Millis(1 << 30));
        if (!n) {
            if (auto k = parser.flush_escape()) parser.feed({});
            continue;
        }
        if (*n == 0) break;
        parser.feed(std::span(buf).first(*n));
        bool quit = false;
        while (auto k = parser.next()) {
            std::visit(overloaded{
                           [&](const term::key::Char& c) { box.insert_text(c.utf8); },
                           [&](const term::key::Paste& p) {
                               // Some terminals send an empty paste when the clipboard holds an image.
                               if (p.text.empty()) do_paste();
                               else box.insert_text(p.text);
                           },
                           [&](const term::key::Ctrl& c) {
                               switch (c.letter) {
                                   case 'v': do_paste(); break;
                                   case 'c': case 'd': quit = true; break;
                                   case 'a': box.home(); break;
                                   case 'e': box.end(); break;
                                   case 'b': box.left(); break;
                                   case 'f': box.right(); break;
                                   case 'w': box.word_left(); break;
                                   case 'u': box.kill_line_before(); break;
                                   case 'j': box.insert_text("\n"); break;
                                   default: break;
                               }
                           },
                           [&](const term::key::Enter&) {
                               if (!box.empty()) submit();
                           },
                           [&](const term::key::AltEnter&) { box.insert_text("\n"); },
                           [&](const term::key::Backspace&) { box.backspace(); },
                           [&](const term::key::Delete&) { box.del(); },
                           [&](const term::key::Left&) { box.left(); },
                           [&](const term::key::Right&) { box.right(); },
                           [&](const term::key::Home&) { box.home(); },
                           [&](const term::key::End&) { box.end(); },
                           [&](const term::key::Escape&) {},
                       },
                       *k);
            if (quit) break;
        }
        if (quit) break;
        if (!parser.in_paste()) draw();
    }
    view.status.clear();
    draw();
    out("\r\n");
    return 0;
}

// Test helper: serve one fixed file as the clipboard.
auto cmd_serve_file(int argc, char** argv) -> int {
    if (argc < 4) return usage();
    fs::path socket = argv[2];
    fs::path file = argv[3];
    std::string kind = argc > 4 ? argv[4] : "image";
    auto data = read_file(file, 1ull << 32);
    if (!data) {
        std::println(stderr, "richpaste: {}", data.error().what);
        return 1;
    }
    Item item = [&]() -> Item {
        if (kind == "text") return Text::repair(as_string(*data));
        if (kind == "file") {
            auto name = FileName::sanitize(file.filename().string());
            auto mime = mime_for_name(name.str());
            return File{std::move(name), std::move(mime), *data};
        }
        auto mime = mime_for_name(file.filename().string());
        if (!mime.is_image()) mime = *Mime::parse("image/png");
        return Image{std::move(mime), *data};
    }();
    Source src = [item](std::uint64_t, std::uint32_t) -> Result<Clip> { return Clip{{item}, "file"}; };
    auto r = run_agent(socket, src, false);
    if (!r) {
        std::println(stderr, "richpaste: {}", r.error().what);
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string_view cmd = argv[1];
    if (cmd == "agent") {
        auto r = run_agent(agent_socket_path(), agent_source(), false);
        if (!r) {
            std::println(stderr, "richpaste: {}", r.error().what);
            return 1;
        }
        return 0;
    }
    if (cmd == "ssh-hook") {
        // ssh runs this on every connection; it must never print to stdout or fail loudly.
        if (auto r = ssh_hook(self_exe()); !r) std::println(stderr, "richpaste: {}", r.error().what);
        return 0;  // 0 = "match", so ssh includes the forward line
    }
    if (cmd == "setup") return cmd_setup();
    if (cmd == "paste") return cmd_paste(argc, argv);
    if (cmd == "doctor") return cmd_doctor();
    if (cmd == "demo") return cmd_demo();
    if (cmd == "serve-file") return cmd_serve_file(argc, argv);
    return usage();
}
