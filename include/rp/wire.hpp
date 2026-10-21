#pragma once

// Wire format between the remote app and the clipboard agent on your machine.
// Everything is little-endian. One request, one response, then the connection closes.
//
//   request:  "RPQ1" u32 op  u64 max_total_bytes  u32 hops
//   response: "RPA1" u8 status
//             status 0: u32 count, then count items:
//                       u8 kind  str16 mime  str16 name  u64 size  bytes[size]  sha256[32]
//             status 1: str16 message
//
// str16 is a u16 length followed by that many bytes.

#include "rp/core.hpp"

#include <concepts>
#include <cstring>

namespace rp::wire {

inline constexpr std::string_view request_magic = "RPQ1";
inline constexpr std::string_view response_magic = "RPA1";
inline constexpr std::uint32_t max_items = 4096;
inline constexpr std::uint32_t max_hops = 8;
inline constexpr std::size_t request_size = 4 + 4 + 8 + 4;

enum class Op : std::uint32_t { clipboard = 1 };

struct Request {
    Op op = Op::clipboard;
    std::uint64_t max_total_bytes = 0;
    std::uint32_t hops = 0;  // how many agents already relayed this
};

class Writer {
public:
    template <std::unsigned_integral T>
    void put(T v) {
        for (std::size_t i = 0; i < sizeof(T); ++i) out_.push_back(static_cast<std::byte>(v >> (8 * i)));
    }
    void put(ByteView b) { out_.insert(out_.end(), b.begin(), b.end()); }
    void put_magic(std::string_view m) { put(as_view(m)); }
    void put_str16(std::string_view s) {
        put(static_cast<std::uint16_t>(s.size()));
        put(as_view(s));
    }
    [[nodiscard]] auto bytes() && -> Bytes { return std::move(out_); }
    [[nodiscard]] auto size() const -> std::size_t { return out_.size(); }

private:
    Bytes out_;
};

// Reads from a buffer that is already fully in memory.
class Reader {
public:
    explicit Reader(ByteView in) : in_(in) {}

    template <std::unsigned_integral T>
    auto get() -> Result<T> {
        if (in_.size() < sizeof(T)) return fail("truncated message");
        T v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(std::to_integer<T>(in_[i]) << (8 * i));
        in_ = in_.subspan(sizeof(T));
        return v;
    }
    auto take(std::size_t n) -> Result<ByteView> {
        if (in_.size() < n) return fail("truncated message");
        auto b = in_.first(n);
        in_ = in_.subspan(n);
        return b;
    }
    auto expect_magic(std::string_view m) -> Result<void> {
        auto b = take(m.size());
        if (!b) return std::unexpected(b.error());
        if (as_string(*b) != m) return fail("not a richpaste message (bad magic)");
        return {};
    }
    auto str16() -> Result<std::string> {
        auto n = get<std::uint16_t>();
        if (!n) return std::unexpected(n.error());
        auto b = take(*n);
        if (!b) return std::unexpected(b.error());
        return as_string(*b);
    }
    [[nodiscard]] auto remaining() const -> std::size_t { return in_.size(); }

private:
    ByteView in_;
};

auto encode_request(const Request& r) -> Bytes;
auto decode_request(ByteView b) -> Result<Request>;

auto encode_items(std::span<const Item> items) -> Bytes;
auto encode_error(std::string_view message) -> Bytes;
// Parses a whole response. Checks every hash and turns raw fields into typed Items.
auto decode_response(ByteView b) -> Result<std::vector<Item>>;

// Bytes needed on the wire for one item, to enforce limits before encoding.
auto encoded_size(const Item& item) -> std::uint64_t;

}  // namespace rp::wire
