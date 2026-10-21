#pragma once

#include "rp/clipboard.hpp"
#include "rp/io.hpp"

namespace rp {

inline constexpr std::uint64_t default_max_bytes = 256ull << 20;

// Where the agent listens on the machine you sit at.
auto agent_socket_path() -> std::filesystem::path;
auto state_dir() -> std::filesystem::path;

// What the agent hands out. Normally agent_source(); tests swap in a fixed item.
using Source = std::function<Result<Clip>(std::uint64_t max_bytes, std::uint32_t hops)>;

// Reads this machine's display, or relays to the next ssh hop if there is none.
auto agent_source() -> Source;

// Serves clipboard requests until the process is killed. Takes a lock so only one runs.
auto run_agent(const std::filesystem::path& socket, Source source, bool quiet) -> Result<void>;

// Answers one connection: read request, read clipboard, write response.
void serve_one(int fd, const Source& source, bool quiet);

// Starts the agent in the background if it isn't already answering.
auto ensure_agent(const std::filesystem::path& self_exe) -> Result<void>;

// ---- remote side ----

// Forwarded sockets this user could reach, newest first. Removes dead ones.
auto find_forwarded_sockets() -> std::vector<std::filesystem::path>;

// Asks one socket for the clipboard.
auto fetch(const std::filesystem::path& socket, std::uint64_t max_bytes) -> Result<std::vector<Item>>;

// The one call an app needs: finds the right clipboard and returns what's on it.
// Order: $RICHPASTE_SOCK, forwarded sockets (newest first), local agent, local display.
auto paste(std::uint64_t max_bytes = default_max_bytes) -> Result<Clip>;

// ---- ssh glue ----

// Called from ssh_config `Match exec`. Writes the forward line ssh will Include.
auto ssh_hook(const std::filesystem::path& self_exe) -> Result<void>;
auto ssh_forward_conf() -> std::filesystem::path;
auto ssh_config_snippet(const std::filesystem::path& self_exe) -> std::string;

inline constexpr std::string_view forward_prefix = "richpaste-";

}  // namespace rp
