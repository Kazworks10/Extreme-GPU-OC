#include "stress/event_log.hpp"

#include <algorithm>
#include <ctime>
#include <format>

#ifdef _WIN32
#include <windows.h>
#include <winevt.h>
#include "platform/fs.hpp"
#endif

namespace gpuoc {

bool is_driver_failure(const DriverEvent& e) noexcept {
  if (e.provider == "nvlddmkm") {
    for (int id : DriverEventIds::kNvlddmkmIds) {
      if (e.event_id == id) return true;
    }
    return false;
  }
  return e.provider == "Display" && e.event_id == DriverEventIds::kDisplayTdrId;
}

std::string build_driver_event_query(WallTime since) {
  using namespace std::chrono;
  const auto t = system_clock::to_time_t(since);
  const auto ms = duration_cast<milliseconds>(since.time_since_epoch()).count() % 1000;
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  std::string ids;
  for (int id : DriverEventIds::kNvlddmkmIds) {
    if (!ids.empty()) ids += " or ";
    ids += std::format("EventID={}", id);
  }
  return std::format(
      "*[System[((Provider[@Name='nvlddmkm'] and ({})) or (Provider[@Name='Display'] and EventID={})) and "
      "TimeCreated[@SystemTime>='{}.{:03}Z']]]",
      ids, DriverEventIds::kDisplayTdrId, buf, ms < 0 ? 0 : ms);
}

#ifdef _WIN32
namespace {

class WindowsEventLog final : public IEventLogSource {
 public:
  WindowsEventLog() {
    lib_ = LoadLibraryW(L"wevtapi.dll");
    if (!lib_) return;
    query_ = reinterpret_cast<decltype(&EvtQuery)>(reinterpret_cast<void*>(GetProcAddress(lib_, "EvtQuery")));
    next_ = reinterpret_cast<decltype(&EvtNext)>(reinterpret_cast<void*>(GetProcAddress(lib_, "EvtNext")));
    render_ = reinterpret_cast<decltype(&EvtRender)>(reinterpret_cast<void*>(GetProcAddress(lib_, "EvtRender")));
    ctx_ = reinterpret_cast<decltype(&EvtCreateRenderContext)>(
        reinterpret_cast<void*>(GetProcAddress(lib_, "EvtCreateRenderContext")));
    close_ = reinterpret_cast<decltype(&EvtClose)>(reinterpret_cast<void*>(GetProcAddress(lib_, "EvtClose")));
    ok_ = query_ && next_ && render_ && ctx_ && close_;
  }
  ~WindowsEventLog() override {
    if (lib_) FreeLibrary(lib_);
  }
  [[nodiscard]] bool available() const override { return ok_; }

  Result<std::vector<DriverEvent>> driver_events_since(WallTime since) override {
    if (!ok_) return make_error(ErrorCode::Unavailable, "wevtapi.dll not available");
    const std::wstring q = platform::widen(build_driver_event_query(since));
    EVT_HANDLE results = query_(nullptr, L"System", q.c_str(), EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!results) {
      const DWORD err = GetLastError();
      if (err == ERROR_ACCESS_DENIED) return make_error(ErrorCode::PermissionDenied, "cannot read the System event log");
      return make_error(ErrorCode::IoError, std::format("EvtQuery failed (error {})", err));
    }
    EVT_HANDLE render_ctx = ctx_(0, nullptr, EvtRenderContextSystem);
    std::vector<DriverEvent> out;
    EVT_HANDLE events[16];
    DWORD returned = 0;
    while (next_(results, 16, events, 1000, 0, &returned)) {
      for (DWORD i = 0; i < returned; ++i) {
        DWORD used = 0, props = 0;
        std::vector<BYTE> buf(4096);
        if (render_(render_ctx, events[i], EvtRenderEventValues, static_cast<DWORD>(buf.size()), buf.data(), &used,
                    &props)) {
          auto* v = reinterpret_cast<PEVT_VARIANT>(buf.data());
          DriverEvent e;
          if (v[EvtSystemProviderName].Type == EvtVarTypeString && v[EvtSystemProviderName].StringVal)
            e.provider = platform::narrow(v[EvtSystemProviderName].StringVal);
          e.event_id = v[EvtSystemEventID].UInt16Val;
          if (v[EvtSystemTimeCreated].Type == EvtVarTypeFileTime) {
            // FILETIME (100 ns since 1601) -> system_clock
            const auto ft = static_cast<long long>(v[EvtSystemTimeCreated].FileTimeVal);
            const long long unix100ns = ft - 116444736000000000LL;
            e.time = WallTime(std::chrono::duration_cast<WallTime::duration>(
                std::chrono::duration<long long, std::ratio<1, 10'000'000>>(unix100ns)));
          }
          e.message = std::format("{} event {}", e.provider, e.event_id);
          out.push_back(std::move(e));
        }
        close_(events[i]);
      }
    }
    if (render_ctx) close_(render_ctx);
    close_(results);
    return out;
  }

 private:
  HMODULE lib_ = nullptr;
  decltype(&EvtQuery) query_ = nullptr;
  decltype(&EvtNext) next_ = nullptr;
  decltype(&EvtRender) render_ = nullptr;
  decltype(&EvtCreateRenderContext) ctx_ = nullptr;
  decltype(&EvtClose) close_ = nullptr;
  bool ok_ = false;
};

}  // namespace

std::unique_ptr<IEventLogSource> make_system_event_log() { return std::make_unique<WindowsEventLog>(); }

#else

namespace {
class UnavailableEventLog final : public IEventLogSource {
 public:
  [[nodiscard]] bool available() const override { return false; }
  Result<std::vector<DriverEvent>> driver_events_since(WallTime) override {
    return make_error(ErrorCode::Unavailable, "the Windows Event Log is not available on this platform");
  }
};
}  // namespace

std::unique_ptr<IEventLogSource> make_system_event_log() { return std::make_unique<UnavailableEventLog>(); }

#endif

}  // namespace gpuoc
