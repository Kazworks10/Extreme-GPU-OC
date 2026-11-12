// Stability verdicts. Inconclusive never counts as stable.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpuoc {

enum class VerdictKind { Stable, Unstable, Inconclusive };

const char* to_string(VerdictKind k) noexcept;

struct Verdict {
  VerdictKind kind = VerdictKind::Inconclusive;
  std::string reason;

  static Verdict stable() { return {VerdictKind::Stable, {}}; }
  static Verdict unstable(std::string why) { return {VerdictKind::Unstable, std::move(why)}; }
  static Verdict inconclusive(std::string why) { return {VerdictKind::Inconclusive, std::move(why)}; }
  [[nodiscard]] bool is_stable() const noexcept { return kind == VerdictKind::Stable; }
  [[nodiscard]] std::string to_string() const;
};

// Everything a stress run observed.
struct StressSignals {
  // Unstable evidence.
  bool checksum_mismatch = false;
  bool crashed = false;
  bool exited_nonzero = false;
  bool device_lost = false;
  bool hang = false;
  bool driver_event = false;   // Windows Event Log: TDR / nvlddmkm
  bool ecc_increase = false;
  bool temp_exceeded = false;
  bool power_exceeded = false;
  // Inconclusive evidence.
  bool telemetry_missing = false;
  bool interrupted = false;
  bool start_failed = false;

  std::vector<std::string> details;
  int iterations = 0;
  double work = 0;
  double seconds = 0;
  int telemetry_samples = 0;
  int telemetry_failures = 0;
  double max_temp_c = 0;
  double max_power_w = 0;
  double avg_power_w = 0;
  std::uint64_t ecc_start = 0;
  std::uint64_t ecc_end = 0;
  std::uint32_t limit_flags = 0;
};

// Pure mapping from observations to a verdict. Any unstable evidence wins,
// even if the run was also interrupted or telemetry was missing.
Verdict evaluate_stress(const StressSignals& s);

}  // namespace gpuoc
