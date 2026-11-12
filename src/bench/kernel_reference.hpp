// Parameters and CPU reference of the deterministic stress kernel
// (src/bench/shaders/stress.comp).
#pragma once

#include <cstdint>
#include <optional>

#include "gpuoc/result.hpp"

namespace gpuoc {

struct KernelParams {
  std::uint32_t elements = 1u << 22;  // power of two, multiple of 256
  std::uint32_t iters = 64;           // ALU iterations per element per pass
  std::uint32_t passes = 16;          // dispatches per submission
  bool operator==(const KernelParams&) const = default;
};

Result<void> validate(const KernelParams& p);

// Bit-exact CPU implementation of the kernel. Cost: elements*iters*passes.
std::uint64_t reference_checksum(const KernelParams& p);

// Known reference checksum for the default parameters (verified against the
// CPU reference by the KernelReference.DefaultConstant test).
inline constexpr std::uint64_t kDefaultReferenceChecksum = 0x5e295ebc513b2968ull;
std::optional<std::uint64_t> known_checksum(const KernelParams& p);

// Work per submission in billions of kernel operations (one LCG/xorshift
// step counts as 4 operations).
double kernel_gop(const KernelParams& p);

}  // namespace gpuoc
