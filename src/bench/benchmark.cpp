#include "bench/benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <regex>

#include "core/heartbeat.hpp"
#include "core/log.hpp"
#include "platform/process.hpp"

namespace gpuoc {

double median_of(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double cv_pct_of(const std::vector<double>& v) {
  if (v.size() < 2) return 0;
  const double mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
  if (mean == 0) return 0;
  double ss = 0;
  for (double x : v) ss += (x - mean) * (x - mean);
  const double sd = std::sqrt(ss / static_cast<double>(v.size() - 1));
  return sd / std::fabs(mean) * 100.0;
}

// Tracks limits hit and power/temperature across a benchmark.
struct LimitTracker {
  EffectiveLimits limits;
  const SafetyPolicy* policy = nullptr;
  bool invalid_on_power_cap = false;
  double power_sum = 0;
  int samples = 0;
  double max_temp = 0;
  std::uint32_t flags = 0;
  std::string violation;

  void sample(Gpu& gpu) {
    auto t = gpu.telemetry();
    if (!t) return;
    const auto& v = t.value();
    power_sum += v.power_w;
    ++samples;
    max_temp = std::max(max_temp, v.temp_c);
    flags |= v.limit_flags;
    if (violation.empty() && policy) {
      if (auto why = policy->telemetry_violation(v, limits)) violation = *why;
    }
  }
  [[nodiscard]] std::string reason() const {
    if (!violation.empty()) return "safety limit: " + violation;
    if (flags & kLimitThermal) return "GPU hit a thermal limit";
    if (flags & kLimitHwSlowdown) return "GPU hit a hardware slowdown";
    if (invalid_on_power_cap && (flags & kLimitPower)) return "GPU hit its power limit";
    return {};
  }
};

namespace {

void finish(BenchmarkResult& r, const BenchmarkProtocol& p, const LimitTracker& lt, std::vector<double> iter_ms) {
  auto& s = r.stats;
  s.median = median_of(s.run_scores);
  s.mean = s.run_scores.empty() ? 0
                                : std::accumulate(s.run_scores.begin(), s.run_scores.end(), 0.0) /
                                      static_cast<double>(s.run_scores.size());
  s.cv_pct = cv_pct_of(s.run_scores);
  if (!s.run_scores.empty()) {
    s.min = *std::min_element(s.run_scores.begin(), s.run_scores.end());
    s.max = *std::max_element(s.run_scores.begin(), s.run_scores.end());
  }
  if (!iter_ms.empty()) {
    s.iter_ms_mean = std::accumulate(iter_ms.begin(), iter_ms.end(), 0.0) / static_cast<double>(iter_ms.size());
    std::sort(iter_ms.begin(), iter_ms.end());
    const auto idx = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(iter_ms.size()))) - 1;
    s.iter_ms_p99 = iter_ms[std::min(idx, iter_ms.size() - 1)];
  }
  r.score = s.median;
  r.avg_power_w = lt.samples ? lt.power_sum / lt.samples : 0;
  r.max_temp_c = lt.max_temp;
  r.limit_flags = lt.flags;
  r.perf_per_watt = r.avg_power_w > 0 ? r.score / r.avg_power_w : 0;
  r.valid = true;
  if (s.cv_pct > p.max_cv_pct) {
    r.valid = false;
    r.invalid_reason = std::format("CV {:.2f}% exceeds {:.2f}%", s.cv_pct, p.max_cv_pct);
  } else if (auto why = lt.reason(); !why.empty()) {
    r.valid = false;
    r.invalid_reason = why;
  }
}

Result<LimitTracker> make_tracker(Gpu& gpu, const BenchmarkProtocol& p) {
  LimitTracker lt;
  GPUOC_TRY_ASSIGN(lt.limits, gpu.limits());
  lt.policy = &gpu.policy();
  lt.invalid_on_power_cap = p.invalid_on_power_cap;
  return lt;
}

}  // namespace

// ---- WorkloadBenchmark -----------------------------------------------------------

WorkloadBenchmark::WorkloadBenchmark(WorkloadFactory factory, IClock& clock, BenchmarkProtocol protocol)
    : factory_(std::move(factory)), clock_(clock), p_(protocol) {}

Result<BenchmarkResult> WorkloadBenchmark::run(Gpu& gpu, const std::atomic<bool>* abort) {
  GPUOC_TRY_ASSIGN(auto wl, factory_(gpu.id()));
  GPUOC_TRY(wl->start());
  struct Stop {
    IWorkload* w;
    ~Stop() { w->stop(); }
  } stop{wl.get()};

  BenchmarkResult r;
  r.name = wl->name();
  r.unit = wl->unit();
  GPUOC_TRY_ASSIGN(LimitTracker lt, make_tracker(gpu, p_));
  auto aborted = [&] { return abort && abort->load(); };
  auto fail = [&](const IterationResult& it) {
    return make_error(ErrorCode::HardwareError,
                      std::format("workload failed during benchmark: {} {}", to_string(it.status), it.detail));
  };

  // Warm-up (not measured, limits not counted).
  const SteadyTime warm_end = clock_.now() + p_.warmup;
  while (clock_.now() < warm_end) {
    if (aborted()) return make_error(ErrorCode::Aborted, "benchmark aborted");
    auto it = wl->run_iteration();
    heartbeat::beat();
    if (it.status != IterationStatus::Ok) return fail(it);
  }

  std::vector<double> iter_ms;
  for (int run = 0; run < p_.runs; ++run) {
    double work = 0, seconds = 0;
    const SteadyTime end = clock_.now() + p_.run_duration;
    do {
      if (aborted()) return make_error(ErrorCode::Aborted, "benchmark aborted");
      auto it = wl->run_iteration();
      heartbeat::beat();
      if (it.status != IterationStatus::Ok) return fail(it);
      work += it.work;
      seconds += it.seconds;
      iter_ms.push_back(it.seconds * 1000.0);
      lt.sample(gpu);
    } while (clock_.now() < end);
    r.stats.run_scores.push_back(seconds > 0 ? work / seconds : 0);
  }
  finish(r, p_, lt, std::move(iter_ms));
  log::info("[{}] benchmark {}: {:.3f} {} (CV {:.2f}%){}", gpu.id().uuid, r.name, r.score, r.unit, r.stats.cv_pct,
            r.valid ? "" : " INVALID: " + r.invalid_reason);
  return r;
}

// ---- ExternalBenchmark -----------------------------------------------------------

ExternalBenchmark::ExternalBenchmark(std::string command, std::string score_regex, std::string unit, Sec timeout,
                                     IClock& clock, BenchmarkProtocol protocol)
    : command_(std::move(command)),
      regex_(std::move(score_regex)),
      unit_(std::move(unit)),
      timeout_(timeout),
      clock_(clock),
      p_(protocol) {}

Result<double> ExternalBenchmark::parse_score(const std::string& output, const std::string& score_regex) {
  std::regex re;
  try {
    re = std::regex(score_regex, std::regex::ECMAScript);
  } catch (const std::regex_error& e) {
    return make_error(ErrorCode::InvalidArgument, std::format("invalid score regex '{}': {}", score_regex, e.what()));
  }
  std::smatch m;
  if (!std::regex_search(output, m, re))
    return make_error(ErrorCode::ParseError, std::format("score regex '{}' did not match the output", score_regex));
  const std::string text = m.size() > 1 ? m[1].str() : m[0].str();
  try {
    std::size_t used = 0;
    const double v = std::stod(text, &used);
    if (used == 0 || !std::isfinite(v)) throw std::invalid_argument("not a number");
    return v;
  } catch (...) {
    return make_error(ErrorCode::ParseError, std::format("matched text '{}' is not a number", text));
  }
}

Result<ExternalBenchmark::RunOutput> ExternalBenchmark::run_once(Gpu& gpu, LimitTracker& tracker) {
  const SteadyTime t0 = clock_.now();
  GPUOC_TRY_ASSIGN(auto child, platform::ChildProcess::spawn_command_line(command_));
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  for (;;) {
    if (std::chrono::steady_clock::now() >= deadline) {
      child->kill();
      return make_error(ErrorCode::Timeout, std::format("external benchmark exceeded {} s", timeout_.count()));
    }
    auto line = child->read_line(Ms(1000));
    heartbeat::beat();
    tracker.sample(gpu);
    if (!line) continue;  // timeout: keep sampling telemetry
    if (!line.value()) break;
    output += *line.value();
    output += '\n';
  }
  auto e = child->wait(Ms(10000));
  if (!e) {
    child->kill();
    return make_error(ErrorCode::Timeout, "external benchmark did not exit");
  }
  if (e->code != 0 || e->crashed)
    return make_error(ErrorCode::HardwareError, std::format("external benchmark failed: {}", e->description));
  GPUOC_TRY_ASSIGN(double score, parse_score(output, regex_));
  return RunOutput{score, seconds_between(t0, clock_.now())};
}

Result<BenchmarkResult> ExternalBenchmark::run(Gpu& gpu, const std::atomic<bool>* abort) {
  BenchmarkResult r;
  r.name = "external";
  r.unit = unit_;
  GPUOC_TRY_ASSIGN(LimitTracker warm_tracker, make_tracker(gpu, p_));
  // Warm-up: repeat the command until the warm-up time has passed.
  const SteadyTime warm_end = clock_.now() + p_.warmup;
  while (clock_.now() < warm_end) {
    if (abort && abort->load()) return make_error(ErrorCode::Aborted, "benchmark aborted");
    GPUOC_TRY(run_once(gpu, warm_tracker));
  }
  GPUOC_TRY_ASSIGN(LimitTracker lt, make_tracker(gpu, p_));
  std::vector<double> iter_ms;
  for (int i = 0; i < p_.runs; ++i) {
    if (abort && abort->load()) return make_error(ErrorCode::Aborted, "benchmark aborted");
    GPUOC_TRY_ASSIGN(RunOutput out, run_once(gpu, lt));
    r.stats.run_scores.push_back(out.score);
    iter_ms.push_back(out.seconds * 1000.0);
  }
  finish(r, p_, lt, std::move(iter_ms));
  return r;
}

std::unique_ptr<IBenchmark> make_benchmark(const BenchmarkConfig& cfg, WorkloadFactory builtin, IClock& clock) {
  const auto p = BenchmarkProtocol::from(cfg);
  if (cfg.workload == "external")
    return std::make_unique<ExternalBenchmark>(cfg.external_command, cfg.external_score_regex, cfg.external_unit,
                                               cfg.external_timeout, clock, p);
  return std::make_unique<WorkloadBenchmark>(std::move(builtin), clock, p);
}

}  // namespace gpuoc
