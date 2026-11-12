#include "tuner/retune_policy.hpp"

#include <format>

namespace gpuoc {

std::optional<std::string> retune_reason(const RetuneInputs& in) {
  if (!in.lkg) return std::string("no validated profile for this GPU");
  const LkgEntry& e = *in.lkg;
  if (e.revoked) return "validated profile was revoked: " + e.revoke_reason;
  if (e.gpu_uuid != in.gpu_uuid) return std::format("GPU UUID changed ({} -> {})", e.gpu_uuid, in.gpu_uuid);
  if (e.driver_version != in.driver_version)
    return std::format("NVIDIA driver changed ({} -> {})", e.driver_version, in.driver_version);
  const auto age = in.now - e.validated_at;
  if (age > std::chrono::hours(24) * in.revalidate_days)
    return std::format("last validation is older than {} days", in.revalidate_days);
  if (in.repeated_instability) return std::string("runtime monitor recorded repeated instability");
  return std::nullopt;
}

}  // namespace gpuoc
