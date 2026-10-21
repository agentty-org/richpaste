// macOS clipboard reader. NSPasteboard through JavaScript for Automation, which ships
// with every macOS. No frameworks to link, so the same C++ builds with plain clang.
#if defined(__APPLE__)

#include "rp/clipboard.hpp"
#include "rp/io.hpp"

#include <algorithm>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rp::mac {

namespace fs = std::filesystem;

namespace {

// Prints "type\n" lines for what's on the pasteboard, or writes one type's data to a file.
constexpr std::string_view script = R"JS(
ObjC.import('AppKit');
function run(argv) {
  const pb = $.NSPasteboard.generalPasteboard;
  if (argv[0] === 'types') {
    const t = pb.types; const out = [];
    for (let i = 0; i < t.count; i++) out.push(ObjC.unwrap(t.objectAtIndex(i)));
    return out.join('\n');
  }
  if (argv[0] === 'files') {
    const urls = pb.readObjectsForClassesOptions($([$.NSURL]), $({}));
    const out = [];
    if (urls) for (let i = 0; i < urls.count; i++) {
      const u = urls.objectAtIndex(i);
      if (u.isFileURL) out.push(ObjC.unwrap(u.path));
    }
    return out.join('\n');
  }
  if (argv[0] === 'data') {
    const d = pb.dataForType(argv[1]);
    if (!d || d.isNil()) return 'none';
    d.writeToFileAtomically(argv[2], true);
    return 'ok';
  }
  return 'bad';
}
)JS";

auto osascript(std::vector<std::string> args) -> Result<std::string> {
    std::vector<std::string> argv{"/usr/bin/osascript", "-l", "JavaScript", "-e", std::string(script)};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_capture(argv, Millis(10000));
}

struct Type {
    std::string uti;
    std::string mime;
};

// Pasteboard types (UTIs) we know how to hand out, best first within each group.
const std::vector<Type> image_types = {
    {"public.png", "image/png"},   {"public.jpeg", "image/jpeg"}, {"org.webmproject.webp", "image/webp"},
    {"com.compuserve.gif", "image/gif"}, {"public.heic", "image/heic"}, {"public.tiff", "image/tiff"},
};

}  // namespace

auto read(std::uint64_t max_bytes) -> Result<std::vector<Item>> {
    auto types_s = osascript({"types"});
    if (!types_s) return fail("reading the macOS pasteboard: {}", types_s.error().what);
    std::vector<std::string> types;
    for (std::size_t i = 0; i < types_s->size();) {
        auto j = types_s->find('\n', i);
        types.push_back(types_s->substr(i, j == std::string::npos ? j : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (types.empty() || (types.size() == 1 && types[0].empty())) return fail("clipboard is empty");
    auto has = [&](std::string_view t) { return std::ranges::contains(types, t); };

    auto tmp_dir = fs::temp_directory_path() / ("richpaste-" + random_hex(8));
    if (auto d = ensure_private_dir(tmp_dir); !d) return std::unexpected(d.error());
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } cleanup{tmp_dir};
    auto fetch = [&](std::string_view uti) -> Result<Bytes> {
        auto out = tmp_dir / "data";
        auto r = osascript({"data", std::string(uti), out.string()});
        if (!r) return std::unexpected(r.error());
        if (*r != "ok") return fail("pasteboard has no {}", uti);
        return read_file(out, max_bytes);
    };

    if (has("public.file-url")) {
        auto list = osascript({"files"});
        if (list && !list->empty()) {
            Offer o{{"text/uri-list"}, [&](std::string_view, std::size_t) -> Result<Bytes> {
                        std::string uris;
                        std::size_t i = 0;
                        while (i < list->size()) {
                            auto j = list->find('\n', i);
                            uris += "file://" + list->substr(i, j == std::string::npos ? j : j - i) + "\n";
                            if (j == std::string::npos) break;
                            i = j + 1;
                        }
                        return to_bytes(uris);
                    }};
            auto r = choose(o, max_bytes);
            if (r) return r;
        }
    }
    for (auto& t : image_types) {
        if (!has(t.uti)) continue;
        auto data = fetch(t.uti);
        if (!data) return std::unexpected(data.error());
        return std::vector<Item>{Image{*Mime::parse(t.mime), std::move(*data)}};
    }
    for (auto t : {"public.utf8-plain-text", "NSStringPboardType", "public.plain-text"}) {
        if (!has(t)) continue;
        auto data = fetch(t);
        if (!data) continue;
        return std::vector<Item>{Text::repair(as_string(*data))};
    }
    std::string list;
    for (auto& t : types) list += (list.empty() ? "" : ", ") + t;
    return fail("nothing pasteable on the clipboard (offered: {})", list.substr(0, 200));
}

}  // namespace rp::mac

#endif
