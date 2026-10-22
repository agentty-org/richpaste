#pragma once

#include "rp/clipboard.hpp"
#include "rp/inband.hpp"
#include "rp/io.hpp"

#include <memory>
#include <optional>
#include <variant>

namespace rp {

inline constexpr std::uint64_t default_max_bytes = 256ull << 20;

// How the terminal route talks to the terminal.
enum class TerminalMode : std::uint8_t {
    automatic,  // ask the terminal what it supports (OSC 5522, else OSC 52)
    osc5522,    // assume OSC 5522 (images), for terminals that misreport it
    osc52,      // OSC 52 only (text)
    off,        // never ask the terminal
};

// What paste may do. agentty fills this from its settings (settings.json and
// AGENTTY_PASTE_*) before the first paste; the defaults are agentty's.
struct Config {
    bool forwards = true;            // ssh-forwarded agents (/tmp/richpaste-<you>-*.sock)
    bool display = true;             // this machine's own clipboard
    bool server_display = false;     // over ssh, also try the server's own clipboard
    TerminalMode terminal = TerminalMode::automatic;
    bool decode_text = true;         // turn richpaste:1: lines in a paste back into files
    std::uint64_t max_bytes = default_max_bytes;  // largest paste accepted
    std::filesystem::path socket;    // ask only this agent socket (relays, tests)
    std::filesystem::path save_dir;  // where pasted images/files are saved; empty: <state dir>/pasted
    std::string token;               // token for a Windows agent at the end of a forward
    std::uint16_t agent_port = 47233;  // Windows agent's loopback port (0 = any free one)
};
// The host may replace it at any time (a Settings change). Each call returns
// a copy, so a paste that already started keeps the one it read.
void set_config(Config c);
auto config() -> Config;

// How the host program names and re-runs itself. agentty sets this once at
// startup, before any thread, so messages say `agentty paste setup` and the
// agent and ssh hook run `agentty paste agent` / `agentty paste ssh-hook`.
// The defaults match agentty.
struct Program {
    std::string name = "agentty paste";   // used in messages
    std::filesystem::path exe;            // empty: this executable
    std::vector<std::string> args{"paste"};  // put before the subcommand
    std::string default_command = "paste";   // run when no subcommand is given; empty: usage
    // Lines the host adds to the usage text (its own subcommands).
    std::string extra_usage =
        "  agentty paste config       list and set the paste settings (Settings \xe2\x86\x92 Paste)\n";
    std::filesystem::path state_dir;      // agent socket, log, saved pastes; empty: ~/.agentty/paste
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
    std::uint64_t max_bytes = config().max_bytes;
    // How to ask the terminal (OSC 5522 / 52). Unset = never ask it.
    // Nothing else may read the terminal during the call.
    std::optional<inband::Session> terminal;
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
