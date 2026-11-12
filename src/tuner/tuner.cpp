#include "tuner/tuner.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/json_codec.hpp"
#include "core/log.hpp"
#include "platform/paths.hpp"

namespace gpuoc {

using nlohmann::json;

double objective_metric(Objective o, double score, double power_w) {
  if (power_w <= 0) return score;
  switch (o) {
    case Objective::MaxPerformance: return score;
    case Objective::MaxPerfPerWatt: return score / power_w;
    case Objective::Balanced: return score / std::sqrt(power_w);
  }
  return score;
}

// ---- StressTrialRunner ------------------------------------------------------------

StressTrialRunner::StressTrialRunner(std::shared_ptr<StressRunner> stress, std::shared_ptr<IBenchmark> bench, Ms poll)
    : stress_(std::move(stress)), bench_(std::move(bench)), poll_(poll) {}

namespace {
TrialResult apply_failure(const Error& e) {
  TrialResult r;
  // A refusal means the tuner asked for something outside the limits: that
  // is a bug, never evidence of stability or instability.
  if (e.code == ErrorCode::SafetyRefusal || e.code == ErrorCode::OutOfRange || e.code == ErrorCode::Unsupported ||
      e.code == ErrorCode::Busy) {
    r.verdict = Verdict::inconclusive("apply refused: " + e.message);
  } else {
    r.verdict = Verdict::unstable("apply failed: " + e.message);
  }
  return r;
}
}  // namespace

TrialResult StressTrialRunner::stress(Gpu& gpu, const Settings& settings, Sec duration, const std::atomic<bool>* abort) {
  if (auto ap = gpu.apply(settings, ClampMode::Reject); !ap) return apply_failure(ap.error());
  StressOptions o;
  o.duration = duration;
  o.poll = poll_;
  const StressReport rep = stress_->run(gpu, o, abort);
  TrialResult r;
  r.verdict = rep.verdict;
  r.score = rep.score;
  r.avg_power_w = rep.signals.avg_power_w;
  r.max_temp_c = rep.signals.max_temp_c;
  r.limit_flags = rep.signals.limit_flags;
  r.safety_violation = rep.signals.temp_exceeded || rep.signals.power_exceeded;
  return r;
}

TrialResult StressTrialRunner::benchmark(Gpu& gpu, const Settings& settings, const std::atomic<bool>* abort) {
  if (auto ap = gpu.apply(settings, ClampMode::Reject); !ap) return apply_failure(ap.error());
  TrialResult r;
  auto b = bench_->run(gpu, abort);
  if (!b) {
    r.verdict = b.error().code == ErrorCode::Aborted ? Verdict::inconclusive("aborted")
                                                     : Verdict::unstable("benchmark failed: " + b.error().message);
    return r;
  }
  r.verdict = Verdict::stable();
  r.score = b.value().score;
  r.avg_power_w = b.value().avg_power_w;
  r.max_temp_c = b.value().max_temp_c;
  r.limit_flags = b.value().limit_flags;
  r.bench_valid = b.value().valid;
  r.unit = b.value().unit;
  if (!b.value().valid) r.verdict.reason = b.value().invalid_reason;
  return r;
}

// ---- Tuner ---------------------------------------------------------------------------

Tuner::Tuner(Gpu& gpu, ITrialRunner& runner, TuningLog& log, IClock& clock, TunerOptions opts)
    : gpu_(gpu), runner_(runner), log_(log), clock_(clock), opts_(std::move(opts)) {}

double Tuner::elapsed_s() const { return replayed_elapsed_s_ + seconds_between(live_start_, clock_.now()); }

double Tuner::budget_left_s() const { return static_cast<double>(opts_.budget.count()) - elapsed_s(); }

bool Tuner::budget_allows(Sec next, Sec reserve) const {
  return elapsed_s() + static_cast<double>((next + reserve).count()) <= static_cast<double>(opts_.budget.count());
}

Result<void> Tuner::begin() {
  GPUOC_TRY_ASSIGN(caps_, gpu_.capabilities());
  GPUOC_TRY_ASSIGN(lim_, gpu_.limits());
  driver_ = caps_.driver_version;
  if (!(caps_.core_offset && lim_.core_offset_allowed) && !(caps_.mem_offset && lim_.mem_offset_allowed))
    return make_error(ErrorCode::Unsupported, "this GPU exposes neither core nor memory clock offsets");
  if (opts_.tuning.core_step_mhz <= 0 || opts_.tuning.mem_step_mhz <= 0)
    return make_error(ErrorCode::InvalidArgument, "tuning steps must be positive");

  const std::string params = tuning_params_hash(opts_.tuning, opts_.objective, gpu_.policy().config());
  GPUOC_TRY_ASSIGN(auto resume, log_.find_resumable(gpu_.id().uuid, driver_, params));
  TuneSessionInfo info;
  if (resume) {
    info = resume->info;
    replay_ = resume->trials;
    out_.resumed = true;
    GPUOC_TRY(log_.resume_session(info));
    log::info("[{}] resuming tuning session {} ({} logged trials)", gpu_.id().uuid, info.session_id, replay_.size());
  } else {
    info.session_id = std::format("{}-{}", to_epoch_ms(clock_.wall_now()), platform::safe_file_component(gpu_.id().uuid));
    info.gpu_uuid = gpu_.id().uuid;
    info.driver_version = driver_;
    info.objective = opts_.objective;
    info.params_hash = params;
    info.started = clock_.wall_now();
    GPUOC_TRY(log_.begin_session(info));
    log::info("[{}] tuning session {} started (objective {})", gpu_.id().uuid, info.session_id,
              to_string(opts_.objective));
  }
  live_start_ = clock_.now();
  seq_ = 0;
  replay_pos_ = 0;
  replayed_elapsed_s_ = 0;
  return success();
}

Settings Tuner::base_settings() const {
  Settings s;
  if (caps_.core_offset && lim_.core_offset_allowed) s.core_offset_mhz = 0;
  if (caps_.mem_offset && lim_.mem_offset_allowed) s.mem_offset_mhz = 0;
  if (caps_.power_limit && lim_.power_allowed) s.power_limit_w = power_ ? *power_ : caps_.stock_power_limit_w;
  if (caps_.fan_control) s.fan_curve = fan_ ? *fan_ : FanCurve::driver_auto();
  if (lim_.voltage_allowed && caps_.stock_voltage_mv > 0) s.voltage_mv = voltage_ ? *voltage_ : caps_.stock_voltage_mv;
  return s;
}

Result<TrialResult> Tuner::run_logged(const std::string& stage, const std::string& kind, const Settings& s,
                                      Sec duration, const std::atomic<bool>* abort) {
  const int seq = seq_++;
  if (replay_pos_ < replay_.size()) {
    const TrialRecord& r = replay_[replay_pos_];
    if (r.seq == seq && r.stage == stage && r.kind == kind && r.settings == s && r.duration == duration) {
      ++replay_pos_;
      ++out_.replayed_trials;
      TrialResult res;
      if (r.completed) {
        replayed_elapsed_s_ += r.elapsed_s;
        res.verdict = Verdict{r.verdict, r.reason};
        res.score = r.score;
        res.avg_power_w = r.avg_power_w;
        res.max_temp_c = r.max_temp_c;
        res.limit_flags = r.limit_flags;
        res.bench_valid = r.bench_valid;
        return res;
      }
      // Started but never finished: the process or the system died during
      // this trial. Conservatively unstable.
      replayed_elapsed_s_ += static_cast<double>(r.duration.count());
      TrialRecord done = r;
      done.completed = true;
      done.verdict = VerdictKind::Unstable;
      done.reason = "process or system died during this trial";
      GPUOC_TRY(log_.trial_result(done));
      GPUOC_TRY(log_.event("crash_recovered", json{{"seq", seq}, {"stage", stage}, {"settings", to_json(s)}}));
      log::warn("[{}] trial {} ({} {}) was interrupted by a crash or reboot; treating it as unstable", gpu_.id().uuid,
                seq, stage, s.to_string());
      out_.notes.push_back(std::format("trial {} ({}) interrupted by crash/reboot: counted as unstable", seq, stage));
      res.verdict = Verdict::unstable(done.reason);
      return res;
    }
    log::warn("[{}] tuning log diverges at trial {}; continuing live", gpu_.id().uuid, seq);
    replay_.resize(replay_pos_);
  }

  TrialRecord rec;
  rec.seq = seq;
  rec.stage = stage;
  rec.kind = kind;
  rec.settings = s;
  rec.duration = duration;
  GPUOC_TRY(log_.trial_start(rec));  // durable before the trial starts
  const SteadyTime t0 = clock_.now();
  TrialResult res = kind == "bench" ? runner_.benchmark(gpu_, s, abort) : runner_.stress(gpu_, s, duration, abort);
  rec.completed = true;
  rec.verdict = res.verdict.kind;
  rec.reason = res.verdict.reason;
  rec.score = res.score;
  rec.avg_power_w = res.avg_power_w;
  rec.max_temp_c = res.max_temp_c;
  rec.limit_flags = res.limit_flags;
  rec.bench_valid = res.bench_valid;
  rec.elapsed_s = seconds_between(t0, clock_.now());
  GPUOC_TRY(log_.trial_result(rec));
  ++out_.trials;
  if (!res.unit.empty()) out_.unit = res.unit;
  return res;
}

Result<TrialResult> Tuner::trial(const std::string& stage, const Settings& s, Sec duration,
                                 const std::atomic<bool>* abort) {
  GPUOC_TRY_ASSIGN(TrialResult r, run_logged(stage, "stress", s, duration, abort));
  if (abort && abort->load()) return make_error(ErrorCode::Aborted, "tuning aborted");
  return r;
}

Result<TrialResult> Tuner::bench(const std::string& stage, const Settings& s, const std::atomic<bool>* abort) {
  GPUOC_TRY_ASSIGN(TrialResult r, run_logged(stage, "bench", s, Sec(0), abort));
  if (abort && abort->load()) return make_error(ErrorCode::Aborted, "tuning aborted");
  return r;
}

Result<bool> Tuner::decisive(const std::string& stage, const Settings& s, Sec duration,
                             const std::atomic<bool>* abort) {
  std::string why;
  for (int attempt = 0; attempt < 2; ++attempt) {
    GPUOC_TRY_ASSIGN(TrialResult r, trial(stage, s, duration, abort));
    if (r.safety_violation)
      return make_error(ErrorCode::SafetyRefusal, std::format("safety limit exceeded during a {} trial at {}: {}", stage,
                                                              s.to_string(), r.verdict.reason));
    if (r.verdict.kind == VerdictKind::Stable) {
      last_stress_max_temp_ = r.max_temp_c;
      last_stress_flags_ = r.limit_flags;
      return true;
    }
    if (r.verdict.kind == VerdictKind::Unstable) return false;
    why = r.verdict.reason;
    log::warn("[{}] inconclusive trial at {}: {}", gpu_.id().uuid, s.to_string(), why);
  }
  // Inconclusive never counts as stable, and guessing "unstable" would bias
  // the search; stop tuning instead.
  return make_error(ErrorCode::HardwareError,
                    std::format("repeated inconclusive results at {} ({}); tuning stopped", s.to_string(), why));
}

Result<SearchResult> Tuner::search(const Space& sp, const std::atomic<bool>* abort) {
  SearchResult res;
  std::map<int, bool> hist;  // offset -> stable
  std::set<int> lookahead_done;
  int retest_rounds = 0;
  res.best_stable = sp.lo;
  const int step = std::max(1, sp.step);
  const int max = sp.lo + ((sp.max - sp.lo) / step) * step;  // highest testable point on the grid
  if (max < sp.lo + step) return res;

  auto run_point = [&](int x, Sec dur) -> Result<void> {
    GPUOC_TRY_ASSIGN(bool stable, decisive(sp.stage, sp.settings_for(x), dur, abort));
    hist[x] = stable;
    return success();
  };

  for (;;) {
    if (abort && abort->load()) return make_error(ErrorCode::Aborted, "tuning aborted");
    // Non-monotonic check: a stable point above an unstable one.
    std::optional<int> u_min;
    for (const auto& [x, stable] : hist) {
      if (!stable) {
        u_min = x;
        break;
      }
    }
    if (u_min) {
      int top_stable = *u_min;
      for (const auto& [x, stable] : hist) {
        if (stable && x > *u_min) top_stable = x;
      }
      if (top_stable > *u_min) {
        ++res.suspect_regions;
        GPUOC_TRY(log_.event("suspect_region", json{{"stage", sp.stage}, {"from", *u_min}, {"to", top_stable}}));
        log::warn("[{}] {}: non-monotonic results (unstable at {}, stable at {}); region marked suspect", gpu_.id().uuid,
                  sp.stage, *u_min, top_stable);
        if (retest_rounds < opts_.tuning.max_retest_rounds) {
          ++retest_rounds;
          std::vector<int> region;
          for (const auto& [x, stable] : hist) {
            (void)stable;
            if (x >= *u_min && x <= top_stable) region.push_back(x);
          }
          for (int x : region) {
            if (!budget_allows(sp.duration * 2, opts_.tuning.long_stress + opts_.bench_estimate)) {
              res.budget_exhausted = true;
              break;
            }
            GPUOC_TRY(run_point(x, sp.duration * 2));
            ++res.retests;
          }
          if (res.budget_exhausted) break;
          continue;
        }
        // Still inconsistent: be conservative and treat everything above the
        // lowest unstable point as unstable.
        for (auto& [x, stable] : hist) {
          if (x > *u_min) stable = false;
        }
        out_.notes.push_back(std::format("{}: unresolved non-monotonic region from {}; capped below it", sp.stage, *u_min));
        continue;
      }
    }

    int L = sp.lo;
    for (const auto& [x, stable] : hist) {
      if (stable && (!u_min || x < *u_min)) L = std::max(L, x);
    }
    res.best_stable = L;
    res.lowest_unstable = u_min;

    int next = 0;
    if (!u_min) {
      if (L >= max) break;
      next = L == sp.lo ? sp.lo + step : sp.lo + 2 * (L - sp.lo);  // step x 2^n
      next = std::min(next, max);
    } else if (*u_min - L > step) {
      next = L + ((*u_min - L) / step / 2) * step;
    } else if (opts_.tuning.boundary_lookahead && !lookahead_done.count(*u_min) && *u_min + step <= max &&
               !hist.count(*u_min + step)) {
      lookahead_done.insert(*u_min);
      next = *u_min + step;
    } else {
      break;
    }
    if (!budget_allows(sp.duration, opts_.tuning.long_stress + opts_.bench_estimate)) {
      res.budget_exhausted = true;
      log::warn("[{}] {}: tuning budget nearly used; stopping the search at {}", gpu_.id().uuid, sp.stage, L);
      break;
    }
    GPUOC_TRY(run_point(next, sp.duration));
  }
  GPUOC_TRY(log_.event("search_done", json{{"stage", sp.stage},
                                           {"best_stable", res.best_stable},
                                           {"lowest_unstable", res.lowest_unstable ? json(*res.lowest_unstable) : json()},
                                           {"suspect_regions", res.suspect_regions}}));
  return res;
}

Result<BaselineResult> Tuner::baseline(const std::atomic<bool>* abort) {
  const Settings stock = base_settings();
  GPUOC_TRY_ASSIGN(TrialResult b, bench("baseline", stock, abort));
  if (b.verdict.kind != VerdictKind::Stable)
    return make_error(ErrorCode::HardwareError, "baseline benchmark failed at stock settings: " + b.verdict.reason);
  GPUOC_TRY_ASSIGN(bool stable, decisive("baseline", stock, opts_.tuning.short_stress, abort));
  if (!stable)
    return make_error(ErrorCode::HardwareError, "the GPU is unstable at stock settings; tuning refused");
  out_.baseline = BaselineResult{b.score, b.avg_power_w, std::max(b.max_temp_c, last_stress_max_temp_), out_.unit,
                                 b.bench_valid};
  if (!b.bench_valid) out_.notes.push_back("baseline benchmark invalid: " + b.verdict.reason);
  GPUOC_TRY(log_.event("stage_done", json{{"stage", "baseline"}, {"score", b.score}, {"power_w", b.avg_power_w}}));
  return out_.baseline;
}

Result<void> Tuner::stage_voltage(const std::atomic<bool>* abort) {
  if (!(caps_.voltage_control && lim_.voltage_allowed && caps_.core_offset && lim_.core_offset_allowed)) return success();
  if (opts_.objective == Objective::MaxPerfPerWatt) {
    out_.notes.push_back("voltage stage skipped: objective is max_perf_per_watt");
    return success();
  }
  if (!core_fail_) {
    out_.notes.push_back("voltage stage skipped: no failing core offset to extend");
    return success();
  }
  int v = caps_.stock_voltage_mv;
  const int vstep = std::max(1, lim_.voltage_step_mv);
  const int core_step = opts_.tuning.core_step_mhz;
  for (;;) {
    const int v_next = v + vstep;
    if (v_next > lim_.voltage_mv.max) {
      out_.notes.push_back(std::format("voltage stage stopped: ceiling {} mV reached", lim_.voltage_mv.max));
      break;
    }
    if (!budget_allows(opts_.tuning.short_stress, opts_.tuning.long_stress + opts_.bench_estimate)) {
      out_.budget_exhausted = true;
      break;
    }
    Settings probe = base_settings();
    probe.mem_offset_mhz = mem_;
    probe.core_offset_mhz = *core_fail_;
    probe.voltage_mv = v_next;
    GPUOC_TRY_ASSIGN(bool stable, decisive("voltage", probe, opts_.tuning.short_stress, abort));
    if (!stable) {
      out_.notes.push_back(std::format("voltage stage stopped: {} mV gains less than one core step", v_next));
      break;
    }
    if (last_stress_flags_ & kLimitPower) {
      out_.notes.push_back(std::format("voltage stage stopped: power limit reached at {} mV", v_next));
      break;
    }
    Space sp;
    sp.stage = "voltage-core";
    sp.step = core_step;
    sp.lo = *core_fail_;  // now known stable at v_next
    sp.max = lim_.core_offset_mhz.max;
    sp.duration = opts_.tuning.short_stress;
    const int mem = mem_;
    sp.settings_for = [this, mem, v_next](int x) {
      Settings s = base_settings();
      s.mem_offset_mhz = mem;
      s.core_offset_mhz = x;
      s.voltage_mv = v_next;
      return s;
    };
    GPUOC_TRY_ASSIGN(SearchResult r, search(sp, abort));
    out_.suspect_regions += r.suspect_regions;
    out_.retests += r.retests;
    log::info("[{}] voltage {} mV: core {:+} -> {:+} MHz", gpu_.id().uuid, v_next, core_, r.best_stable);
    v = v_next;
    voltage_ = v;
    core_ = r.best_stable;
    core_fail_ = r.lowest_unstable;
    if (r.budget_exhausted) {
      out_.budget_exhausted = true;
      break;
    }
    if (!core_fail_) break;
  }
  if (voltage_) out_.voltage_mv = voltage_;
  return log_.event("stage_done", json{{"stage", "voltage"}, {"voltage_mv", voltage_ ? json(*voltage_) : json()}});
}

Result<void> Tuner::stage_power_fan(const std::atomic<bool>* abort) {
  const double target = lim_.target_temp_c;
  auto with = [&](std::optional<int> power, std::optional<FanCurve> fan) {
    Settings s = base_settings();
    if (s.core_offset_mhz) s.core_offset_mhz = core_;
    if (s.mem_offset_mhz) s.mem_offset_mhz = mem_;
    if (power) s.power_limit_w = power;
    if (fan) s.fan_curve = fan;
    return s;
  };

  // Power limit: raise in steps only while the objective benefits and the
  // temperature stays under target.
  if (caps_.power_limit && lim_.power_allowed && opts_.tuning.max_power_steps > 0) {
    int p = power_ ? *power_ : caps_.stock_power_limit_w;
    const int pmax = lim_.power_limit_w.max;
    if (p + 1 > pmax) {
      out_.notes.push_back(std::format("power stage skipped: limit already at the effective maximum {} W", pmax));
    } else {
      GPUOC_TRY_ASSIGN(TrialResult b0, bench("power", with(p, std::nullopt), abort));
      if (b0.verdict.kind != VerdictKind::Stable || !b0.bench_valid) {
        out_.notes.push_back("power stage skipped: benchmark invalid (" + b0.verdict.reason + ")");
      } else if (!(b0.limit_flags & kLimitPower)) {
        out_.notes.push_back("power stage skipped: the GPU is not power limited");
      } else {
        double best = objective_metric(opts_.objective, b0.score, b0.avg_power_w);
        for (int i = 0; i < opts_.tuning.max_power_steps; ++i) {
          const int p_next = std::min(pmax, p + opts_.tuning.power_step_w);
          if (p_next <= p) break;
          if (!budget_allows(opts_.tuning.short_stress, opts_.tuning.long_stress + opts_.bench_estimate)) {
            out_.budget_exhausted = true;
            break;
          }
          GPUOC_TRY_ASSIGN(bool stable, decisive("power", with(p_next, std::nullopt), opts_.tuning.short_stress, abort));
          if (!stable) break;
          if (last_stress_max_temp_ > target) {
            out_.notes.push_back(std::format("power stage stopped: {:.1f} C above target at {} W", last_stress_max_temp_, p_next));
            break;
          }
          GPUOC_TRY_ASSIGN(TrialResult b, bench("power", with(p_next, std::nullopt), abort));
          if (b.verdict.kind != VerdictKind::Stable || !b.bench_valid) break;
          const double m = objective_metric(opts_.objective, b.score, b.avg_power_w);
          if (m < best * 1.01) {
            out_.notes.push_back(std::format("power stage stopped: {} W does not improve {}", p_next,
                                             to_string(opts_.objective)));
            break;
          }
          best = m;
          p = p_next;
          power_ = p;
        }
      }
    }
  }

  // Fan: only if fan control is available and the GPU runs near its target.
  if (caps_.fan_control && lim_.fan_allowed && opts_.tuning.tune_fan && last_stress_max_temp_ > target - 3.0 &&
      !out_.budget_exhausted) {
    const int t = static_cast<int>(target);
    FanCurve cool;
    for (FanPoint fp : {FanPoint{40, 35}, FanPoint{55, 50}, FanPoint{t - 5, 75}, FanPoint{t, 90}, FanPoint{t + 5, 100}}) {
      if (cool.points.empty() || fp.temp_c > cool.points.back().temp_c) cool.points.push_back(fp);
    }
    const double before = last_stress_max_temp_;
    if (gpu_.policy().check_fan_curve(cool) && budget_allows(opts_.tuning.short_stress, opts_.tuning.long_stress + opts_.bench_estimate)) {
      GPUOC_TRY_ASSIGN(bool stable, decisive("fan", with(std::nullopt, cool), opts_.tuning.short_stress, abort));
      if (stable && last_stress_max_temp_ < before) {
        fan_ = cool;
        out_.notes.push_back(std::format("fan curve applied: {:.1f} C -> {:.1f} C", before, last_stress_max_temp_));
      }
    }
  }
  return log_.event("stage_done", json{{"stage", "power_fan"},
                                       {"power_w", power_ ? json(*power_) : json()},
                                       {"fan_curve", fan_ ? to_json(*fan_) : json()}});
}

Result<Settings> Tuner::tune(const std::atomic<bool>* abort) {
  const int mem_step = opts_.tuning.mem_step_mhz;
  const int core_step = opts_.tuning.core_step_mhz;
  const int margin = opts_.tuning.margin_steps;

  // Stage 2: memory offset.
  int mem_found = 0;
  if (caps_.mem_offset && lim_.mem_offset_allowed) {
    Space sp;
    sp.stage = "mem";
    sp.step = mem_step;
    sp.lo = 0;
    sp.max = lim_.mem_offset_mhz.max;
    sp.duration = opts_.tuning.short_stress;
    sp.settings_for = [this](int x) {
      Settings s = base_settings();
      s.mem_offset_mhz = x;
      return s;
    };
    GPUOC_TRY_ASSIGN(SearchResult r, search(sp, abort));
    mem_found = r.best_stable;
    out_.suspect_regions += r.suspect_regions;
    out_.retests += r.retests;
    out_.budget_exhausted |= r.budget_exhausted;
  }
  out_.mem_found_mhz = mem_found;
  // The core search runs with the memory offset already backed off by the
  // margin, so memory errors are not mistaken for core instability.
  mem_ = std::max(0, mem_found - margin * mem_step);
  GPUOC_TRY(log_.event("stage_done", json{{"stage", "mem"}, {"found", mem_found}, {"working", mem_}}));

  // Stage 3: core offset.
  int core_found = 0;
  if (caps_.core_offset && lim_.core_offset_allowed && !out_.budget_exhausted) {
    Space sp;
    sp.stage = "core";
    sp.step = core_step;
    sp.lo = 0;
    sp.max = lim_.core_offset_mhz.max;
    sp.duration = opts_.tuning.short_stress;
    sp.settings_for = [this](int x) {
      Settings s = base_settings();
      if (s.mem_offset_mhz) s.mem_offset_mhz = mem_;
      s.core_offset_mhz = x;
      return s;
    };
    GPUOC_TRY_ASSIGN(SearchResult r, search(sp, abort));
    core_found = r.best_stable;
    core_fail_ = r.lowest_unstable;
    out_.suspect_regions += r.suspect_regions;
    out_.retests += r.retests;
    out_.budget_exhausted |= r.budget_exhausted;
  }
  core_ = core_found;
  GPUOC_TRY(log_.event("stage_done", json{{"stage", "core"}, {"found", core_found}}));

  // Stage 4: voltage (gated).
  if (!out_.budget_exhausted) GPUOC_TRY(stage_voltage(abort));
  out_.core_found_mhz = core_;

  // Stage 5: power limit and fan.
  if (!out_.budget_exhausted) GPUOC_TRY(stage_power_fan(abort));

  // Stage 6: margin.
  Settings cand = base_settings();
  if (cand.core_offset_mhz) cand.core_offset_mhz = std::max(0, core_ - margin * core_step);
  if (cand.mem_offset_mhz) cand.mem_offset_mhz = mem_;
  out_.candidate = cand;
  GPUOC_TRY(log_.event("stage_done", json{{"stage", "margin"}, {"candidate", to_json(cand)}}));
  log::info("[{}] candidate after margin: {}", gpu_.id().uuid, cand.to_string());
  return cand;
}

Result<bool> Tuner::validate(const std::atomic<bool>* abort) {
  Settings cand = out_.candidate;
  const Sec long_dur = opts_.tuning.long_stress;
  for (int attempt = 0; attempt <= opts_.tuning.validation_retries; ++attempt) {
    if (!budget_allows(long_dur, opts_.bench_estimate)) {
      out_.budget_exhausted = true;
      out_.notes.push_back("tuning budget exhausted before validation could finish");
      GPUOC_TRY(log_.event("validation_skipped", json{{"reason", "budget"}}));
      return false;
    }
    GPUOC_TRY_ASSIGN(bool stable, decisive("validate", cand, long_dur, abort));
    if (stable) {
      GPUOC_TRY_ASSIGN(TrialResult b, bench("final", cand, abort));
      out_.validated = true;
      out_.settings = cand;
      out_.final_score = b.score;
      out_.final_power_w = b.avg_power_w;
      out_.final_max_temp_c = std::max(b.max_temp_c, last_stress_max_temp_);
      out_.validation_duration = long_dur;
      GPUOC_TRY(log_.event("validated", json{{"settings", to_json(cand)}, {"score", b.score}, {"attempt", attempt}}));
      log::info("[{}] validated {} ({:.1f} s long stress)", gpu_.id().uuid, cand.to_string(),
                static_cast<double>(long_dur.count()));
      return true;
    }
    // Back off one more step and validate again.
    const bool core_left = cand.core_offset_mhz && *cand.core_offset_mhz > 0;
    const bool mem_left = cand.mem_offset_mhz && *cand.mem_offset_mhz > 0;
    if (!core_left && !mem_left) {
      out_.notes.push_back("validation failed even with zero offsets");
      break;
    }
    if (core_left) cand.core_offset_mhz = std::max(0, *cand.core_offset_mhz - opts_.tuning.core_step_mhz);
    if (mem_left) cand.mem_offset_mhz = std::max(0, *cand.mem_offset_mhz - opts_.tuning.mem_step_mhz);
    GPUOC_TRY(log_.event("validation_backoff", json{{"next", to_json(cand)}, {"attempt", attempt}}));
  }
  GPUOC_TRY(log_.event("validation_failed", json::object()));
  return false;
}

void Tuner::finish(const std::string& reason) {
  out_.elapsed_s = elapsed_s();
  if (auto r = log_.end_session(reason); !r) log::error("tuning log: {}", r.error().message);
}

Result<TuneOutcome> Tuner::run(const std::atomic<bool>* abort) {
  GPUOC_TRY(begin());
  auto body = [&]() -> Result<void> {
    GPUOC_TRY(baseline(abort));
    GPUOC_TRY(tune(abort));
    GPUOC_TRY(validate(abort));
    return success();
  };
  auto r = body();
  out_.elapsed_s = elapsed_s();
  if (!r) {
    // An abort (shutdown, Ctrl+C) leaves the session open so it can resume.
    if (r.error().code != ErrorCode::Aborted) finish("error: " + r.error().message);
    return r.error();
  }
  finish(out_.validated ? "validated" : (out_.budget_exhausted ? "budget_exhausted" : "not_validated"));
  return out_;
}

}  // namespace gpuoc
