#include "tuner/tuning_log.hpp"

#include <format>
#include <map>
#include <sstream>

#include "core/json_codec.hpp"
#include "core/log.hpp"
#include "platform/fs.hpp"

namespace gpuoc {

using nlohmann::json;

namespace {

std::uint64_t fnv1a(const std::string& s) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ull;
  }
  return h;
}

Result<VerdictKind> parse_verdict(const std::string& s) {
  if (s == "stable") return VerdictKind::Stable;
  if (s == "unstable") return VerdictKind::Unstable;
  if (s == "inconclusive") return VerdictKind::Inconclusive;
  return make_error(ErrorCode::ParseError, "bad verdict " + s);
}

}  // namespace

std::string tuning_params_hash(const TuningConfig& t, Objective objective, const SafetyConfig& s) {
  json j{{"core_step", t.core_step_mhz},
         {"mem_step", t.mem_step_mhz},
         {"margin", t.margin_steps},
         {"short", t.short_stress.count()},
         {"long", t.long_stress.count()},
         {"retries", t.validation_retries},
         {"lookahead", t.boundary_lookahead},
         {"retest", t.max_retest_rounds},
         {"power_step", t.power_step_w},
         {"power_steps", t.max_power_steps},
         {"fan", t.tune_fan},
         {"objective", to_string(objective)},
         {"allow_voltage", s.allow_voltage},
         {"allow_power", s.allow_power_above_stock},
         {"max_temp", s.max_temp_c},
         {"target_temp", s.target_temp_c},
         {"max_power", s.max_power_w ? *s.max_power_w : 0},
         {"max_voltage", s.max_voltage_mv ? *s.max_voltage_mv : 0},
         {"max_core", s.max_core_offset_mhz ? *s.max_core_offset_mhz : 0},
         {"max_mem", s.max_mem_offset_mhz ? *s.max_mem_offset_mhz : 0}};
  return std::format("{:016x}", fnv1a(j.dump()));
}

TuningLog::TuningLog(std::filesystem::path file, bool read_only) : file_(std::move(file)), read_only_(read_only) {}

Result<void> TuningLog::write(json j) {
  j["ts"] = to_epoch_ms(std::chrono::system_clock::now());
  if (!session_.empty() && !j.contains("session")) j["session"] = session_;
  if (read_only_) {
    log::debug("[dry-run] tuning log: {}", j.dump());
    return success();
  }
  return platform::append_line_durable(file_, j.dump());
}

Result<void> TuningLog::begin_session(const TuneSessionInfo& info) {
  session_ = info.session_id;
  return write(json{{"type", "session"},
                    {"session", info.session_id},
                    {"gpu", info.gpu_uuid},
                    {"driver", info.driver_version},
                    {"objective", to_string(info.objective)},
                    {"params", info.params_hash},
                    {"started_ms", to_epoch_ms(info.started)}});
}

Result<void> TuningLog::resume_session(const TuneSessionInfo& info) {
  session_ = info.session_id;
  return write(json{{"type", "resume"}, {"session", info.session_id}});
}

Result<void> TuningLog::trial_start(const TrialRecord& t) {
  return write(json{{"type", "trial_start"},
                    {"seq", t.seq},
                    {"stage", t.stage},
                    {"kind", t.kind},
                    {"settings", to_json(t.settings)},
                    {"duration_s", t.duration.count()}});
}

Result<void> TuningLog::trial_result(const TrialRecord& t) {
  return write(json{{"type", "trial_result"},
                    {"seq", t.seq},
                    {"verdict", to_string(t.verdict)},
                    {"reason", t.reason},
                    {"score", t.score},
                    {"avg_power_w", t.avg_power_w},
                    {"max_temp_c", t.max_temp_c},
                    {"limit_flags", t.limit_flags},
                    {"bench_valid", t.bench_valid},
                    {"elapsed_s", t.elapsed_s}});
}

Result<void> TuningLog::event(const std::string& type, json data) {
  data["type"] = type;
  return write(std::move(data));
}

Result<void> TuningLog::end_session(const std::string& reason) {
  auto r = write(json{{"type", "session_end"}, {"reason", reason}});
  return r;
}

Result<std::optional<ResumeState>> TuningLog::find_resumable(const std::string& gpu_uuid,
                                                              const std::string& driver_version,
                                                              const std::string& params_hash) const {
  auto text = platform::read_file(file_);
  if (!text) return text.error().code == ErrorCode::NotFound ? Result<std::optional<ResumeState>>(std::nullopt)
                                                            : Result<std::optional<ResumeState>>(text.error());
  struct Session {
    TuneSessionInfo info;
    bool ended = false;
    std::map<int, TrialRecord> trials;
  };
  std::map<std::string, Session> sessions;
  std::string last_session;
  std::istringstream in(text.value());
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (line.empty()) continue;
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      // A torn last line after power loss is expected; anything else is logged.
      log::warn("tuning log {}:{}: unreadable line ignored", platform::path_utf8(file_), lineno);
      continue;
    }
    const std::string type = j.value("type", std::string());
    const std::string sid = j.value("session", std::string());
    if (type == "session") {
      Session s;
      s.info.session_id = sid;
      s.info.gpu_uuid = j.value("gpu", std::string());
      s.info.driver_version = j.value("driver", std::string());
      s.info.objective = parse_objective(j.value("objective", std::string("max_perf_per_watt"))).value_or(Objective::MaxPerfPerWatt);
      s.info.params_hash = j.value("params", std::string());
      s.info.started = from_epoch_ms(j.value("started_ms", 0LL));
      sessions[sid] = std::move(s);
      if (sessions[sid].info.gpu_uuid == gpu_uuid) last_session = sid;
      continue;
    }
    auto it = sessions.find(sid);
    if (it == sessions.end()) continue;
    Session& s = it->second;
    if (type == "session_end") {
      s.ended = true;
    } else if (type == "trial_start") {
      TrialRecord t;
      t.seq = j.value("seq", -1);
      t.stage = j.value("stage", std::string());
      t.kind = j.value("kind", std::string("stress"));
      t.duration = Sec(j.value("duration_s", 0LL));
      auto st = settings_from_json(j.value("settings", json::object()));
      if (!st) continue;
      t.settings = st.value();
      s.trials[t.seq] = t;
    } else if (type == "trial_result") {
      const int seq = j.value("seq", -1);
      auto tr = s.trials.find(seq);
      if (tr == s.trials.end()) continue;
      auto v = parse_verdict(j.value("verdict", std::string()));
      if (!v) continue;
      TrialRecord& t = tr->second;
      t.completed = true;
      t.verdict = v.value();
      t.reason = j.value("reason", std::string());
      t.score = j.value("score", 0.0);
      t.avg_power_w = j.value("avg_power_w", 0.0);
      t.max_temp_c = j.value("max_temp_c", 0.0);
      t.limit_flags = j.value("limit_flags", 0u);
      t.bench_valid = j.value("bench_valid", false);
      t.elapsed_s = j.value("elapsed_s", 0.0);
    }
  }
  if (last_session.empty()) return std::optional<ResumeState>();
  const Session& s = sessions[last_session];
  if (s.ended || s.info.driver_version != driver_version || s.info.params_hash != params_hash)
    return std::optional<ResumeState>();
  ResumeState r;
  r.info = s.info;
  for (const auto& [seq, t] : s.trials) {
    (void)seq;
    r.trials.push_back(t);
    r.elapsed_s += t.completed ? t.elapsed_s : static_cast<double>(t.duration.count());
  }
  return std::optional<ResumeState>(std::move(r));
}

Result<json> TuningLog::status_summary() const {
  auto text = platform::read_file(file_);
  if (!text) return text.error();
  json last_session, last_trial, last_event;
  int trials = 0;
  std::istringstream in(text.value());
  std::string line;
  while (std::getline(in, line)) {
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded()) continue;
    const std::string type = j.value("type", std::string());
    if (type == "session") {
      last_session = j;
      trials = 0;
      last_event = json();
      last_trial = json();
    } else if (type == "trial_result") {
      ++trials;
      last_trial = j;
    } else if (type == "trial_start") {
      last_trial = j;
    } else {
      last_event = j;
    }
  }
  return json{{"session", last_session}, {"trials_completed", trials}, {"last_trial", last_trial}, {"last_event", last_event}};
}

}  // namespace gpuoc
