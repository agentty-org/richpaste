#pragma once

// The richpaste command line, as a library call, so another program can ship
// it as a subcommand (call set_program() first, see rp/agent.hpp).

#include <span>
#include <string>

namespace rp::cli {

// args are the words after the program, e.g. {"paste", "-o", "dir"}.
// Returns the process exit code.
auto run(std::span<const std::string> args) -> int;

}  // namespace rp::cli
