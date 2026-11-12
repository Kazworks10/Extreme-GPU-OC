#include "bench/kernel_reference.hpp"

#include <format>
#include <vector>

namespace gpuoc {

namespace {

inline std::uint32_t mix32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

}  // namespace

Result<void> validate(const KernelParams& p) {
  if (p.elements < 256 || (p.elements & (p.elements - 1)) != 0 || p.elements > (1u << 26))
    return make_error(ErrorCode::InvalidArgument,
                      std::format("kernel elements {} must be a power of two in [256, 2^26]", p.elements));
  if (p.iters < 1 || p.iters > 4096)
    return make_error(ErrorCode::InvalidArgument, std::format("kernel iters {} not in [1, 4096]", p.iters));
  if (p.passes < 1 || p.passes > 256)
    return make_error(ErrorCode::InvalidArgument, std::format("kernel passes {} not in [1, 256]", p.passes));
  return success();
}

std::uint64_t reference_checksum(const KernelParams& p) {
  const std::uint32_t n = p.elements;
  const std::uint32_t mask = n - 1;
  std::vector<std::uint32_t> a(n), b(n);
  std::uint32_t sum_xor = 0, sum_add = 0;
  for (std::uint32_t pass = 0; pass < p.passes; ++pass) {
    const bool first = pass == 0;
    const bool last = pass + 1 == p.passes;
    const std::vector<std::uint32_t>& src = (pass % 2 == 0) ? a : b;
    std::vector<std::uint32_t>& dst = (pass % 2 == 0) ? b : a;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t j = mix32(i + pass * 0x632be5abu) & mask;
      std::uint32_t x = first ? mix32(i ^ 0x9e3779b9u) : src[i];
      std::uint32_t y = first ? mix32(j ^ 0x9e3779b9u) : src[j];
      for (std::uint32_t k = 0; k < p.iters; ++k) {
        x = x * 1664525u + 1013904223u;
        x ^= (x >> 13) ^ y;
        y = (y << 5) | (y >> 27);
      }
      dst[i] = x;
      if (last) {
        sum_xor ^= x;
        sum_add += x ^ i;
      }
    }
  }
  return (static_cast<std::uint64_t>(sum_xor) << 32) | sum_add;
}

std::optional<std::uint64_t> known_checksum(const KernelParams& p) {
  if (p == KernelParams{}) return kDefaultReferenceChecksum;
  return std::nullopt;
}

double kernel_gop(const KernelParams& p) {
  return static_cast<double>(p.elements) * p.iters * p.passes * 4.0 / 1e9;
}

}  // namespace gpuoc
