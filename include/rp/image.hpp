#pragma once

#include "rp/core.hpp"

namespace rp::image {

// 8-bit RGBA pixels, row-major, top row first.
struct Rgba {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Bytes pixels;  // width * height * 4
};

auto encode_png(const Rgba& img) -> Bytes;

// Windows packed DIB (BITMAPINFOHEADER or V4/V5 header, then optional masks/palette, then pixels).
// Handles 1/4/8-bit palette, 16/24/32-bit, BI_RGB and BI_BITFIELDS, bottom-up and top-down.
auto decode_dib(ByteView dib) -> Result<Rgba>;

// A .bmp file is a 14-byte file header followed by a DIB.
auto decode_bmp(ByteView bmp) -> Result<Rgba>;

auto crc32(ByteView data, std::uint32_t crc = 0) -> std::uint32_t;
auto adler32(ByteView data) -> std::uint32_t;
// zlib stream (RFC 1950) with fixed-Huffman deflate (RFC 1951).
auto zlib_compress(ByteView data) -> Bytes;

}  // namespace rp::image
