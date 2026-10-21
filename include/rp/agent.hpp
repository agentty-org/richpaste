#pragma once

#include "rp/clipboard.hpp"
#include "rp/io.hpp"

#include <variant>

namespace rp {

inline constexpr std::uint64_t default_max_bytes = 256ull << 20;

// How this program names and re-runs itself. An app that ships richpaste as a
// subcommand sets this once at startup, before any thread, e.g.
//   set_program({"agentty paste", {}, {"paste"}});
// so messages say `agentty paste setup` and the agent and ssh hook run
// `agentty paste agent` / `agentty paste ssh-hook`.
struct Program {
    std::string name = "richpaste";      // used in messages
    std::filesystem::path exe;            // empty: this executable
    std::vector<std::string> args;        // put before the subcommand
    std::string default_command;          // run when no subcommand is given; empty: usage
    std::filesystem::path state_dir;      // agent socket, log, saved pastes; empty: ~/.richpaste
};
void set_program(Program p);
auto program() -> const Program&;
// The command line that runs subcommand `sub` of this program.
auto self_command(std::string_view sub) -> std::vector<std::string>;

auto state_dir() -> std::filesystem::path;

// How to reach an agent. Unix socket on Linux/macOS; loopback TCP + token on Windows.
struct UnixEndpoint {
    std::filesystem::path path;
};
struct TcpEndpoint {
    std::uint16_t port;
    std::string token;
};
using Endpoint = std::variant<UnixEndpoint, TcpEndpoint>;

// Where this machine's agent listens (or will listen).
auto local_endpoint() -> Result<Endpoint>;
auto describe(const Endpoint& e) -> std::string;

// What the agent hands out. Normally agent_source(); tests swap in a fixed item.
using Source = std::function<Result<Clip>(std::uint64_t max_bytes, std::uint32_t hops)>;

// Reads this machine's display, or relays to the next ssh hop if there is none.
auto agent_source() -> Source;

// Serves clipboard requests until the process is killed. Only one agent runs per user.
auto run_agent(Source source, bool quiet) -> Result<void>;
// Same, on an explicit unix socket (tests and relays).
auto run_agent_at(const std::filesystem::path& socket, Source source, bool quiet) -> Result<void>;

// Answers one connection: read request, check token, read clipboard, write response.
void serve_one(int fd, const Source& source, std::string_view token, bool quiet);

// Starts the agent in the background if it isn't already answering.
auto ensure_agent() -> Result<void>;
auto agent_alive() -> bool;

// ---- remote side ----

// Forwarded sockets this user could reach, newest first.
auto find_forwarded_sockets() -> std::vector<std::filesystem::path>;

auto fetch(const Endpoint& e, std::uint64_t max_bytes, std::uint32_t hops = 0) -> Result<std::vector<Item>>;

// Where a paste may come from, in the order tried.
enum class Route { env_socket, forward, display, terminal };

struct PasteOptions {
    std::uint64_t max_bytes = default_max_bytes;
    // tty to ask the terminal on (OSC 5522 / 52). -1 = never ask the terminal.
    // The tty must be in raw mode, and nothing else may read it during the call.
    int tty_in = -1;
    int tty_out = -1;
};

// The one call an app needs: finds the right clipboard and returns what's on it.
// Order: $RICHPASTE_SOCK, ssh forwards (newest first), this machine's display,
// then the terminal itself. Inside ssh, forwards come before the display.
auto paste(const PasteOptions& opt = {}) -> Result<Clip>;

// ---- ssh glue ----

// Starts the agent if needed and returns a fresh {remote, local} pair for
// `ssh -R remote:local` (the remote is a unix socket path with %r in it).
auto remote_forward() -> Result<std::pair<std::string, std::string>>;

// Called from ssh_config `Match exec`. Writes the forward line ssh will Include.
auto ssh_hook() -> Result<void>;
auto ssh_forward_conf() -> std::filesystem::path;
auto ssh_config_snippet() -> std::string;

inline constexpr std::string_view forward_prefix = "richpaste-";

}  // namespace rp
