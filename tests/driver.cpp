// Test driver: the library's command line plus helpers the shell tests need.
// Not shipped; agentty is the real program (`agentty paste ...`).

#include "rp/agent.hpp"
#include "rp/cli.hpp"
#include "rp/io.hpp"

#include <print>
#include <string>
#include <vector>

using namespace rp;
namespace fs = std::filesystem;

namespace {

// Serve one fixed file as the clipboard on a unix socket.
auto serve_file(std::span<const std::string> args) -> int {
    if (args.size() < 3) return 2;
    fs::path socket = args[1];
    fs::path file = args[2];
    std::string kind = args.size() > 3 ? args[3] : "image";
    auto data = read_file(file, 1ull << 32);
    if (!data) {
        std::println(stderr, "rp_driver: {}", data.error().what);
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
    auto r = run_agent_at(socket, src, false);
    if (!r) {
        std::println(stderr, "rp_driver: {}", r.error().what);
        return 1;
    }
    return 0;
}

// Ask the local agent without its token (Windows same-user check).
auto fetch_notoken() -> int {
    auto e = local_endpoint();
    if (!e) return 1;
    if (auto* t = std::get_if<TcpEndpoint>(&*e)) t->token.clear();
    auto r = fetch(*e, 1 << 20);
    std::println("{}", r ? "served" : r.error().what);
    return r ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    platform_init();
    // Same as agentty's, except it re-runs itself as `rp_driver agent`, not `... paste agent`.
    set_program({.name = "rp_driver", .exe = {}, .args = {}, .default_command = "paste", .extra_usage = ""});
    // agentty feeds Config from its settings; the shell tests drive it with env.
    Config c;
    if (env("RICHPASTE_NO_DISPLAY")) c.display = false;
    if (env("RICHPASTE_NO_FORWARDS")) c.forwards = false;
    if (env("RICHPASTE_SERVER_CLIPBOARD")) c.server_display = true;
    if (env("RICHPASTE_NO_TERMINAL")) c.terminal = TerminalMode::off;
    if (auto t = env("RICHPASTE_TERMINAL")) c.terminal = *t == "5522" ? TerminalMode::osc5522 : TerminalMode::osc52;
    if (auto s = env("RICHPASTE_SOCK")) c.socket = *s;
    if (auto t = env("RICHPASTE_TOKEN")) c.token = *t;
    if (auto p = env("RICHPASTE_PORT")) c.agent_port = static_cast<std::uint16_t>(std::stoi(*p));
    set_config(std::move(c));
    if (!args.empty() && args[0] == "serve-file") return serve_file(args);
    if (!args.empty() && args[0] == "test-fetch-notoken") return fetch_notoken();
    return cli::run(args);
}
