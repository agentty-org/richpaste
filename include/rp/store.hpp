#pragma once

// Saving pasted images and files to disk, named by content hash.

#include "rp/core.hpp"

#include <filesystem>
#include <span>

namespace rp {

// One saved item.
struct Saved {
    Kind kind;
    std::string mime;
    std::string label;             // e.g. "image png 120 KB", "report.pdf 2 MB"
    std::filesystem::path path;    // where the bytes are
    std::uint64_t size;
    std::string sha256;
};

// Saves images and files under dir. Text items come back as their text,
// in order. Same bytes, same name: a second save of the same thing is free.
struct Pasted {
    std::vector<std::variant<std::string, Saved>> parts;
};
auto store(std::span<const Item> items, const std::filesystem::path& dir) -> Result<Pasted>;

}  // namespace rp
