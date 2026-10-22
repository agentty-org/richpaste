#include "rp/store.hpp"

#include "rp/io.hpp"
#include "rp/sha256.hpp"

namespace rp {

namespace fs = std::filesystem;

auto store(std::span<const Item> items, const fs::path& dir) -> Result<Pasted> {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return fail("create {}: {}", dir.string(), ec.message());
    Pasted out;
    for (auto& item : items) {
        if (auto* t = std::get_if<Text>(&item)) {
            out.parts.emplace_back(t->str());
            continue;
        }
        auto data = payload(item);
        auto digest = hex(sha256(data));
        auto mime = std::string(mime_of(item));
        auto sub = std::string_view(mime).substr(mime.find('/') + 1);
        sub = sub.substr(0, sub.find(';'));
        std::string name;
        std::string label;
        if (auto* f = std::get_if<File>(&item)) {
            name = digest.substr(0, 12) + "-" + std::string(f->name.str());
            label = std::format("{} {}", f->name.str(), human_size(data.size()));
        } else {
            name = std::format("{}.{}", digest.substr(0, 16), ext_for_mime(mime));
            label = std::format("image {} {}", sub, human_size(data.size()));
        }
        auto path = dir / name;
        // Same hash means same bytes: skip the write.
        if (!fs::exists(path, ec))
            if (auto w = write_file_atomic(path, data); !w) return std::unexpected(w.error());
        out.parts.emplace_back(Saved{kind_of(item), mime, label, path, data.size(), digest});
    }
    return out;
}

}  // namespace rp
