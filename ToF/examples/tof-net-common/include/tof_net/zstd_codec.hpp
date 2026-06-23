#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tof_net {

std::vector<uint8_t> zstd_compress(const uint8_t *data, size_t size, int level);
std::vector<uint8_t> zstd_decompress(const uint8_t *data, size_t compressed_size, size_t raw_size);

} // namespace tof_net
