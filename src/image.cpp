#include "rp/image.hpp"

#include <array>
#include <bit>
#include <cstring>

namespace rp::image {

namespace {

constexpr auto crc_table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[n] = c;
    }
    return t;
}();

class BitWriter {
public:
    // Deflate packs bits LSB-first.
    void bits(std::uint32_t v, int n) {
        acc_ |= static_cast<std::uint64_t>(v) << used_;
        used_ += n;
        while (used_ >= 8) {
            out_.push_back(static_cast<std::byte>(acc_ & 0xFF));
            acc_ >>= 8;
            used_ -= 8;
        }
    }
    // Huffman codes are sent MSB-first, so reverse them.
    void code(std::uint32_t c, int n) {
        std::uint32_t r = 0;
        for (int i = 0; i < n; ++i) r |= ((c >> i) & 1) << (n - 1 - i);
        bits(r, n);
    }
    void flush() {
        if (used_ > 0) bits(0, 8 - used_);
    }
    auto take() -> Bytes { return std::move(out_); }

private:
    Bytes out_;
    std::uint64_t acc_ = 0;
    int used_ = 0;
};

// Fixed literal/length code from RFC 1951 3.2.6.
void put_literal(BitWriter& w, unsigned v) {
    if (v < 144) w.code(0x30 + v, 8);
    else if (v < 256) w.code(0x190 + (v - 144), 9);
    else if (v < 280) w.code(v - 256, 7);
    else w.code(0xC0 + (v - 280), 8);
}

constexpr std::array<std::uint16_t, 29> len_base{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> len_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> dist_base{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                  33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> dist_extra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void put_match(BitWriter& w, unsigned len, unsigned dist) {
    unsigned li = 28;
    while (len_base[li] > len) --li;
    put_literal(w, 257 + li);
    if (len_extra[li]) w.bits(len - len_base[li], len_extra[li]);
    unsigned di = 29;
    while (dist_base[di] > dist) --di;
    w.code(di, 5);
    if (dist_extra[di]) w.bits(dist - dist_base[di], dist_extra[di]);
}

void put_u32be(Bytes& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::byte>(v >> (8 * i)));
}

void put_chunk(Bytes& out, std::string_view type, ByteView data) {
    put_u32be(out, static_cast<std::uint32_t>(data.size()));
    auto start = out.size();
    out.insert(out.end(), reinterpret_cast<const std::byte*>(type.data()),
               reinterpret_cast<const std::byte*>(type.data()) + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_u32be(out, crc32(ByteView(out).subspan(start)));
}

auto le16(ByteView b, std::size_t o) -> std::uint32_t {
    return std::to_integer<std::uint32_t>(b[o]) | std::to_integer<std::uint32_t>(b[o + 1]) << 8;
}
auto le32(ByteView b, std::size_t o) -> std::uint32_t { return le16(b, o) | le16(b, o + 2) << 16; }

// Scales a masked channel value to 0..255.
auto channel(std::uint32_t px, std::uint32_t mask) -> std::uint8_t {
    if (mask == 0) return 0;
    auto shift = std::countr_zero(mask);
    auto bits = std::popcount(mask);
    auto v = (px & mask) >> shift;
    auto maxv = (1u << bits) - 1;
    return static_cast<std::uint8_t>(bits >= 8 ? v >> (bits - 8) : (v * 255 + maxv / 2) / maxv);
}

}  // namespace

auto crc32(ByteView data, std::uint32_t crc) -> std::uint32_t {
    crc = ~crc;
    for (auto b : data) crc = crc_table[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

auto adler32(ByteView data) -> std::uint32_t {
    std::uint32_t a = 1, b = 0;
    std::size_t i = 0;
    while (i < data.size()) {
        auto n = std::min<std::size_t>(data.size() - i, 5552);
        for (std::size_t k = 0; k < n; ++k) {
            a += std::to_integer<std::uint32_t>(data[i + k]);
            b += a;
        }
        a %= 65521;
        b %= 65521;
        i += n;
    }
    return b << 16 | a;
}

auto zlib_compress(ByteView data) -> Bytes {
    BitWriter w;
    w.bits(0x78, 8);
    w.bits(0x01, 8);  // CMF/FLG: deflate, 32K window, fastest
    w.bits(1, 1);     // final block
    w.bits(1, 2);     // fixed Huffman

    // LZ77 with one hash head per 3-byte prefix plus a short chain.
    constexpr std::size_t window = 32768, hash_size = 1 << 15, max_chain = 16;
    std::vector<std::int64_t> head(hash_size, -1);
    std::vector<std::int64_t> prev(window, -1);
    auto at = [&](std::size_t i) { return std::to_integer<std::uint32_t>(data[i]); };
    auto hash = [&](std::size_t i) { return ((at(i) << 10) ^ (at(i + 1) << 5) ^ at(i + 2)) & (hash_size - 1); };
    auto insert = [&](std::size_t i) {
        if (i + 2 >= data.size()) return;
        auto h = hash(i);
        prev[i % window] = head[h];
        head[h] = static_cast<std::int64_t>(i);
    };

    std::size_t i = 0;
    while (i < data.size()) {
        unsigned best_len = 0, best_dist = 0;
        if (i + 2 < data.size()) {
            auto cand = head[hash(i)];
            for (std::size_t chain = 0; cand >= 0 && chain < max_chain; ++chain) {
                auto c = static_cast<std::size_t>(cand);
                if (i - c > window - 1) break;
                unsigned len = 0;
                auto limit = std::min<std::size_t>(258, data.size() - i);
                while (len < limit && data[c + len] == data[i + len]) ++len;
                if (len > best_len) {
                    best_len = len;
                    best_dist = static_cast<unsigned>(i - c);
                    if (len == 258) break;
                }
                auto p = prev[c % window];
                if (p >= cand) break;  // stale entry from an older lap of the ring
                cand = p;
            }
        }
        if (best_len >= 3) {
            put_match(w, best_len, best_dist);
            for (unsigned k = 0; k < best_len; ++k) insert(i + k);
            i += best_len;
        } else {
            put_literal(w, at(i));
            insert(i);
            ++i;
        }
    }
    put_literal(w, 256);  // end of block
    w.flush();
    auto out = w.take();
    put_u32be(out, adler32(data));
    return out;
}

auto encode_png(const Rgba& img) -> Bytes {
    const std::size_t stride = static_cast<std::size_t>(img.width) * 4;
    // Filter each row with "Up" or "Sub", whichever has smaller absolute sum. Cheap and good on screenshots.
    Bytes raw;
    raw.reserve((stride + 1) * img.height);
    Bytes sub(stride), up(stride);
    for (std::uint32_t y = 0; y < img.height; ++y) {
        auto row = ByteView(img.pixels).subspan(y * stride, stride);
        std::uint64_t sum_sub = 0, sum_up = 0;
        for (std::size_t x = 0; x < stride; ++x) {
            auto cur = std::to_integer<std::uint8_t>(row[x]);
            auto left = x >= 4 ? std::to_integer<std::uint8_t>(row[x - 4]) : 0;
            auto above = y > 0 ? std::to_integer<std::uint8_t>(img.pixels[(y - 1) * stride + x]) : 0;
            auto s = static_cast<std::uint8_t>(cur - left);
            auto u = static_cast<std::uint8_t>(cur - above);
            sub[x] = std::byte{s};
            up[x] = std::byte{u};
            sum_sub += s < 128 ? s : 256 - s;
            sum_up += u < 128 ? u : 256 - u;
        }
        bool use_up = y > 0 && sum_up < sum_sub;
        raw.push_back(std::byte{static_cast<std::uint8_t>(use_up ? 2 : 1)});
        auto& f = use_up ? up : sub;
        raw.insert(raw.end(), f.begin(), f.end());
    }

    Bytes out = {std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
                 std::byte{'\r'}, std::byte{'\n'}, std::byte{0x1A}, std::byte{'\n'}};
    Bytes ihdr;
    put_u32be(ihdr, img.width);
    put_u32be(ihdr, img.height);
    for (int b : {8, 6, 0, 0, 0}) ihdr.push_back(static_cast<std::byte>(b));  // 8-bit RGBA
    put_chunk(out, "IHDR", ihdr);
    put_chunk(out, "IDAT", zlib_compress(raw));
    put_chunk(out, "IEND", {});
    return out;
}

auto decode_dib(ByteView b) -> Result<Rgba> {
    if (b.size() < 40) return fail("bitmap too small");
    auto hsize = le32(b, 0);
    if (hsize < 40 || hsize > b.size()) return fail("unsupported bitmap header");
    auto w = static_cast<std::int32_t>(le32(b, 4));
    auto h = static_cast<std::int32_t>(le32(b, 8));
    auto bpp = le16(b, 14);
    auto compression = le32(b, 16);
    auto colors_used = le32(b, 32);
    if (w <= 0 || h == 0 || w > 65535 || h > 65535 || h < -65535) return fail("bad bitmap size {}x{}", w, h);
    bool top_down = h < 0;
    auto height = static_cast<std::uint32_t>(top_down ? -h : h);
    auto width = static_cast<std::uint32_t>(w);
    constexpr std::uint32_t BI_RGB = 0, BI_BITFIELDS = 3, BI_ALPHABITFIELDS = 6;
    if (compression != BI_RGB && compression != BI_BITFIELDS && compression != BI_ALPHABITFIELDS)
        return fail("compressed bitmaps are not supported");

    std::size_t off = hsize;
    std::uint32_t rm = 0, gm = 0, bm = 0, am = 0;
    if (bpp == 16) rm = 0x7C00, gm = 0x03E0, bm = 0x001F;
    if (bpp == 32) rm = 0x00FF0000, gm = 0x0000FF00, bm = 0x000000FF;
    if (compression == BI_BITFIELDS || compression == BI_ALPHABITFIELDS) {
        if (hsize >= 52) {
            // V4/V5 headers carry the masks inside the header.
            rm = le32(b, 40), gm = le32(b, 44), bm = le32(b, 48);
            if (hsize >= 56) am = le32(b, 52);
        } else {
            auto n = compression == BI_ALPHABITFIELDS ? 16u : 12u;
            if (b.size() < off + n) return fail("bitmap masks missing");
            rm = le32(b, off), gm = le32(b, off + 4), bm = le32(b, off + 8);
            if (n == 16) am = le32(b, off + 12);
            off += n;
        }
    } else if (bpp == 32 && hsize >= 56) {
        am = le32(b, 52);  // V5 BI_RGB may still declare alpha
    }

    std::vector<std::array<std::uint8_t, 4>> palette;
    if (bpp <= 8) {
        if (bpp != 1 && bpp != 4 && bpp != 8) return fail("unsupported bitmap depth {}", bpp);
        auto n = colors_used ? colors_used : 1u << bpp;
        if (n > 256 || b.size() < off + n * 4) return fail("bitmap palette missing");
        for (std::uint32_t i = 0; i < n; ++i) {
            auto p = off + i * 4;
            palette.push_back({std::to_integer<std::uint8_t>(b[p + 2]), std::to_integer<std::uint8_t>(b[p + 1]),
                               std::to_integer<std::uint8_t>(b[p]), 255});
        }
        off += n * 4;
    } else if (bpp != 16 && bpp != 24 && bpp != 32) {
        return fail("unsupported bitmap depth {}", bpp);
    }

    std::size_t stride = ((static_cast<std::size_t>(width) * bpp + 31) / 32) * 4;
    // Some writers (and CF_DIB from some apps) put pixels right after the header even with BI_BITFIELDS.
    if (b.size() < off + stride * height && b.size() >= hsize + stride * height) off = hsize;
    if (b.size() < off + stride * height) return fail("bitmap pixel data is truncated");

    Rgba img{width, height, Bytes(static_cast<std::size_t>(width) * height * 4)};
    bool any_alpha = false;
    for (std::uint32_t y = 0; y < height; ++y) {
        auto src_y = top_down ? y : height - 1 - y;
        auto row = b.subspan(off + src_y * stride, stride);
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t r, g, bl, a = 255;
            if (bpp <= 8) {
                auto bit = static_cast<std::size_t>(x) * bpp;
                auto byte = std::to_integer<std::uint32_t>(row[bit / 8]);
                auto idx = (byte >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                auto& c = idx < palette.size() ? palette[idx] : palette[0];
                r = c[0], g = c[1], bl = c[2];
            } else if (bpp == 24) {
                auto p = x * 3u;
                bl = std::to_integer<std::uint8_t>(row[p]);
                g = std::to_integer<std::uint8_t>(row[p + 1]);
                r = std::to_integer<std::uint8_t>(row[p + 2]);
            } else {
                std::uint32_t px = bpp == 16 ? le16(row, x * 2u) : le32(row, x * 4u);
                r = channel(px, rm), g = channel(px, gm), bl = channel(px, bm);
                if (am) {
                    a = channel(px, am);
                    any_alpha |= a != 0;
                } else if (bpp == 32) {
                    a = std::to_integer<std::uint8_t>(row[x * 4u + 3]);
                    any_alpha |= a != 0;
                }
            }
            auto d = (static_cast<std::size_t>(y) * width + x) * 4;
            img.pixels[d] = std::byte{r};
            img.pixels[d + 1] = std::byte{g};
            img.pixels[d + 2] = std::byte{bl};
            img.pixels[d + 3] = std::byte{a};
        }
    }
    // 32-bit bitmaps very often leave alpha at 0 everywhere. That means opaque, not invisible.
    if (bpp == 32 && !any_alpha)
        for (std::size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = std::byte{255};
    return img;
}

auto decode_bmp(ByteView bmp) -> Result<Rgba> {
    if (bmp.size() < 14 || bmp[0] != std::byte{'B'} || bmp[1] != std::byte{'M'}) return fail("not a BMP file");
    return decode_dib(bmp.subspan(14));
}

}  // namespace rp::image
