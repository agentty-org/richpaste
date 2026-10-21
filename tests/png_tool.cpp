// Test-only: convert a BMP to PNG with our encoder.  png_tool in.bmp out.png
#include "rp/image.hpp"
#include "rp/io.hpp"

#include <print>

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    auto bmp = rp::read_file(argv[1], 1u << 30);
    if (!bmp) return 1;
    auto img = rp::image::decode_bmp(*bmp);
    if (!img) {
        std::println(stderr, "{}", img.error().what);
        return 1;
    }
    auto png = rp::image::encode_png(*img);
    return rp::write_file_atomic(argv[2], png) ? 0 : 1;
}
