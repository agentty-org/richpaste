#include "rp/wire.hpp"

#include "rp/sha256.hpp"

#include <algorithm>

namespace rp::wire {

namespace {

auto make_item(Kind kind, std::string_view mime_s, std::string_view name_s, ByteView data) -> Result<Item> {
    auto mime = Mime::parse(mime_s);
    if (!mime) return std::unexpected(mime.error());
    switch (kind) {
        case Kind::text: {
            auto t = Text::parse(as_string(data));
            if (!t) return std::unexpected(t.error());
            return Item{std::move(*t)};
        }
        case Kind::image:
            if (!mime->is_image()) return fail("image item has non-image mime '{}'", mime->str());
            return Item{Image{std::move(*mime), Bytes(data.begin(), data.end())}};
        case Kind::file: {
            auto name = FileName::parse(name_s);
            if (!name) return std::unexpected(name.error());
            return Item{File{std::move(*name), std::move(*mime), Bytes(data.begin(), data.end())}};
        }
    }
    return fail("unknown item kind {}", std::to_underlying(kind));
}

}  // namespace

auto encode_request(const Request& r) -> Bytes {
    Writer w;
    w.put_magic(request_magic);
    w.put(std::to_underlying(r.op));
    w.put(r.max_total_bytes);
    w.put(r.hops);
    w.put_str16(r.token.substr(0, max_token));
    return std::move(w).bytes();
}

auto decode_request(ByteView b) -> Result<Request> {
    Reader r(b);
    if (auto m = r.expect_magic(request_magic); !m) return std::unexpected(m.error());
    auto op = r.get<std::uint32_t>();
    auto max = r.get<std::uint64_t>();
    auto hops = r.get<std::uint32_t>();
    if (!op || !max || !hops) return fail("truncated request");
    auto token = r.str16();
    if (!token) return fail("truncated request");
    if (token->size() > max_token) return fail("token too long");
    if (*op != std::to_underlying(Op::clipboard)) return fail("unknown request op {}", *op);
    if (*hops > max_hops) return fail("request relayed through too many machines (loop?)");
    return Request{Op::clipboard, *max, *hops, std::move(*token)};
}

auto encoded_size(const Item& item) -> std::uint64_t {
    return 1 + 2 + mime_of(item).size() + 2 + name_of(item).size() + 8 + payload(item).size() + 32;
}

auto encode_items(std::span<const Item> items) -> Bytes {
    Writer w;
    w.put_magic(response_magic);
    w.put(std::uint8_t{0});
    w.put(static_cast<std::uint32_t>(items.size()));
    for (auto& item : items) {
        auto data = payload(item);
        w.put(std::to_underlying(kind_of(item)));
        w.put_str16(mime_of(item));
        w.put_str16(name_of(item));
        w.put(static_cast<std::uint64_t>(data.size()));
        w.put(data);
        w.put(ByteView(sha256(data)));
    }
    return std::move(w).bytes();
}

auto encode_error(std::string_view message, Code code) -> Bytes {
    Writer w;
    w.put_magic(response_magic);
    w.put(std::uint8_t{1});
    w.put_str16(message.substr(0, 4000));
    w.put(std::to_underlying(code));
    return std::move(w).bytes();
}

auto decode_response(ByteView b) -> Result<std::vector<Item>> {
    Reader r(b);
    if (auto m = r.expect_magic(response_magic); !m) return std::unexpected(m.error());
    auto status = r.get<std::uint8_t>();
    if (!status) return std::unexpected(status.error());
    if (*status == 1) {
        auto msg = r.str16();
        if (!msg) return std::unexpected(msg.error());
        // Older agents send no code byte.
        auto code = r.remaining() ? r.get<std::uint8_t>() : Result<std::uint8_t>{0};
        auto c = code && *code <= std::to_underlying(Code::needs_terminal) ? static_cast<Code>(*code) : Code::other;
        return fail(c, "{}", repair_utf8(*msg));
    }
    if (*status != 0) return fail("unknown response status {}", *status);

    auto count = r.get<std::uint32_t>();
    if (!count) return std::unexpected(count.error());
    if (*count > max_items) return fail("too many items ({})", *count);

    std::vector<Item> items;
    items.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        auto kind = r.get<std::uint8_t>();
        auto mime = r.str16();
        auto name = r.str16();
        auto size = r.get<std::uint64_t>();
        if (!kind || !mime || !name || !size) return fail("truncated item {}", i);
        if (*size > r.remaining()) return fail("item {} claims {} bytes but message is shorter", i, *size);
        auto data = r.take(static_cast<std::size_t>(*size));
        auto digest = r.take(32);
        if (!data || !digest) return fail("truncated item {}", i);
        auto actual = sha256(*data);
        if (!std::ranges::equal(*digest, actual)) return fail(Code::corrupt, "item {} is corrupted (sha256 mismatch)", i);
        auto item = make_item(static_cast<Kind>(*kind), *mime, *name, *data);
        if (!item) return fail("item {}: {}", i, item.error().what);
        items.push_back(std::move(*item));
    }
    if (r.remaining() != 0) return fail(Code::corrupt, "{} trailing bytes after response", r.remaining());
    return items;
}

}  // namespace rp::wire
