#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace rp {

using Digest = std::array<std::byte, 32>;

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::byte> data);
    [[nodiscard]] auto finish() -> Digest;

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

auto sha256(std::span<const std::byte> data) -> Digest;
auto hex(std::span<const std::byte> data) -> std::string;

}  // namespace rp
