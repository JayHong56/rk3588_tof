#include "tof_net/zstd_codec.hpp"
#include <stdexcept>
#include <zstd.h>

namespace tof_net {

std::vector<uint8_t> zstd_compress(const uint8_t *data, size_t size, int level) {
    size_t bound = ZSTD_compressBound(size);
    std::vector<uint8_t> out(bound);
    size_t n = ZSTD_compress(out.data(), out.size(), data, size, level);
    if (ZSTD_isError(n)) {
        throw std::runtime_error(std::string("ZSTD_compress failed: ") + ZSTD_getErrorName(n));
    }
    out.resize(n);
    return out;
}

std::vector<uint8_t> zstd_decompress(const uint8_t *data, size_t compressed_size, size_t raw_size) {
    std::vector<uint8_t> out(raw_size);
    size_t n = ZSTD_decompress(out.data(), out.size(), data, compressed_size);
    if (ZSTD_isError(n)) {
        throw std::runtime_error(std::string("ZSTD_decompress failed: ") + ZSTD_getErrorName(n));
    }
    if (n != raw_size) {
        throw std::runtime_error("ZSTD_decompress size mismatch");
    }
    return out;
}

} // namespace tof_net
