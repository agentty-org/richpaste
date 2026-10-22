#pragma once

// What the commands find out, as data. The library prints nothing here, so an
// app can draw it its own way; rp::cli prints it as plain text.

#include "rp/agent.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rp {

// Pastes the way a command-line program does: asks the terminal as a last
// resort, through /dev/tty, if the config allows it.
auto paste_here() -> Result<Clip>;

// Where pasted images and files are saved.
auto save_dir() -> std::filesystem::path;

// Everything `doctor` looks at, and the result of one real paste.
struct Diagnosis {
    Config config;
    std::filesystem::path state_dir;
    std::filesystem::path save_dir;
    std::string agent;  // where this machine's agent listens
    bool agent_alive = false;
    std::optional<std::string> wayland_display;
    std::optional<std::string> x_display;
    std::optional<std::string> ssh_connection;
    bool tmux = false;  // $TMUX is set
    struct Forward {
        std::filesystem::path socket;
        bool live = false;
    };
    std::vector<Forward> forwards;
    Situation situation;
    std::vector<Route> plan;
    Result<Clip> paste = fail("not tried");
};
auto diagnose() -> Diagnosis;

// `setup`: adds the ssh hook to ~/.ssh/config and starts the agent.
struct SetupReport {
    enum class Change : std::uint8_t { unchanged, updated, added };
    std::filesystem::path ssh_config;
    Change change = Change::unchanged;
    std::string snippet;  // the block that is in ssh_config now
};
auto setup() -> Result<SetupReport>;

// One line for a terminal mode, e.g. "auto".
auto terminal_mode_name(TerminalMode m) -> std::string_view;

}  // namespace rp
