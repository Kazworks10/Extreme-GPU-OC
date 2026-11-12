// gpuoc-worker: runs the deterministic Vulkan kernel in its own process so a
// driver crash or GPU hang cannot take down the daemon. Writes one JSON object
// per line to stdout:
//   {"event":"ready", "device":..., "uuid":..., "reference":"0x...", ...}
//   {"event":"iteration", "n":1, "ok":true, "checksum":"0x...", "seconds":..., "work":...}
//   {"event":"error", "kind":"init|device_lost|hang|error", "message":...}
// Exit codes: 0 done, 1 usage, 2 init failure, 4 device lost, 5 hang.
//
// --simulate <mode> exercises the process plumbing without a GPU (tests).
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "bench/kernel_reference.hpp"
#include "bench/vulkan_kernel.hpp"

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

using nlohmann::json;

namespace {

void emit(const json& j) {
  std::fputs((j.dump() + "\n").c_str(), stdout);
  std::fflush(stdout);
}

std::string hex64(std::uint64_t v) { return std::format("0x{:016x}", v); }

[[noreturn]] void crash_now() {
#ifdef _WIN32
  RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
  std::raise(SIGSEGV);
#endif
  std::abort();
}

int usage() {
  std::fputs(
      "usage: gpuoc-worker [--uuid GPU-... | --index N] [--elements N] [--iters N] [--passes N]\n"
      "                    [--duration S] [--iterations N] [--repeat N] [--timeout-ms N]\n"
      "                    [--list] [--simulate ok|mismatch|crash|hang|exit3|device_lost] [--simulate-after N]\n",
      stderr);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
  gpuoc::DeviceSelector sel;
  gpuoc::KernelParams params;
  double duration_s = 0;
  long long max_iterations = 0;
  int repeat = 0;
  long long timeout_ms = 10000;
  std::string simulate;
  int simulate_after = 2;
  bool list = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    try {
      if (a == "--uuid" && (v = next())) sel.uuid = v;
      else if (a == "--index" && (v = next())) sel.index = std::stoi(v);
      else if (a == "--elements" && (v = next())) params.elements = static_cast<std::uint32_t>(std::stoul(v));
      else if (a == "--iters" && (v = next())) params.iters = static_cast<std::uint32_t>(std::stoul(v));
      else if (a == "--passes" && (v = next())) params.passes = static_cast<std::uint32_t>(std::stoul(v));
      else if (a == "--duration" && (v = next())) duration_s = std::stod(v);
      else if (a == "--iterations" && (v = next())) max_iterations = std::stoll(v);
      else if (a == "--repeat" && (v = next())) repeat = std::stoi(v);
      else if (a == "--timeout-ms" && (v = next())) timeout_ms = std::stoll(v);
      else if (a == "--simulate" && (v = next())) simulate = v;
      else if (a == "--simulate-after" && (v = next())) simulate_after = std::stoi(v);
      else if (a == "--list") list = true;
      else return usage();
    } catch (...) {
      return usage();
    }
  }

  if (list) {
    auto devs = gpuoc::VulkanKernel::list_devices();
    if (!devs) {
      emit({{"event", "error"}, {"kind", "init"}, {"message", devs.error().message}});
      return 2;
    }
    for (const auto& d : devs.value())
      emit({{"event", "device"}, {"index", d.index}, {"name", d.name}, {"uuid", d.uuid}, {"vendor_id", d.vendor_id}});
    return 0;
  }

  const auto start = std::chrono::steady_clock::now();
  auto keep_going = [&](long long n) {
    if (max_iterations > 0 && n >= max_iterations) return false;
    if (duration_s > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= duration_s)
      return false;
    return true;
  };

  if (!simulate.empty()) {
    const std::uint64_t ref = 0x1234abcd5678ef00ull;
    emit({{"event", "ready"}, {"device", "simulated"}, {"uuid", sel.uuid}, {"reference", hex64(ref)}, {"simulate", simulate}});
    for (long long n = 1; keep_going(n - 1); ++n) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      if (n > simulate_after) {
        if (simulate == "mismatch") {
          emit({{"event", "iteration"}, {"n", n}, {"ok", false}, {"checksum", hex64(ref ^ 1)}, {"seconds", 0.02}, {"work", 0.0}});
          continue;
        }
        if (simulate == "crash") crash_now();
        if (simulate == "hang") std::this_thread::sleep_for(std::chrono::hours(1));
        if (simulate == "exit3") return 3;
        if (simulate == "device_lost") {
          emit({{"event", "error"}, {"kind", "device_lost"}, {"message", "VK_ERROR_DEVICE_LOST (simulated)"}});
          return 4;
        }
      }
      emit({{"event", "iteration"}, {"n", n}, {"ok", true}, {"checksum", hex64(ref)}, {"seconds", 0.02}, {"work", 1.0}});
    }
    return 0;
  }

  if (auto v = gpuoc::validate(params); !v) {
    emit({{"event", "error"}, {"kind", "init"}, {"message", v.error().message}});
    return 2;
  }
  std::uint64_t reference = 0;
  if (auto known = gpuoc::known_checksum(params)) {
    reference = *known;
  } else {
    reference = gpuoc::reference_checksum(params);  // CPU; cost elements*iters*passes
  }
  auto kernel = gpuoc::VulkanKernel::create(sel, params);
  if (!kernel) {
    emit({{"event", "error"}, {"kind", "init"}, {"message", kernel.error().message}});
    return 2;
  }
  auto& k = *kernel.value();
  emit({{"event", "ready"},
        {"device", k.device().name},
        {"uuid", k.device().uuid},
        {"reference", hex64(reference)},
        {"elements", params.elements},
        {"iters", params.iters},
        {"passes", params.passes}});

  const auto timeout = std::chrono::milliseconds(timeout_ms);
  // Calibrate submissions per reported iteration to roughly 250 ms.
  if (repeat <= 0) {
    auto r = k.run_once(timeout);
    repeat = 1;
    if (r.status == gpuoc::KernelRunResult::Status::Ok && r.seconds > 0)
      repeat = static_cast<int>(std::clamp(0.25 / r.seconds, 1.0, 1000.0));
  }

  for (long long n = 1; keep_going(n - 1); ++n) {
    double seconds = 0;
    int good = 0;
    std::uint64_t bad_sum = 0;
    bool mismatch = false;
    for (int r = 0; r < repeat; ++r) {
      auto res = k.run_once(timeout);
      seconds += res.seconds;
      switch (res.status) {
        case gpuoc::KernelRunResult::Status::Ok:
          if (res.checksum == reference) {
            ++good;
          } else if (!mismatch) {
            mismatch = true;
            bad_sum = res.checksum;
          }
          break;
        case gpuoc::KernelRunResult::Status::DeviceLost:
          emit({{"event", "error"}, {"kind", "device_lost"}, {"message", res.detail}});
          return 4;
        case gpuoc::KernelRunResult::Status::Timeout:
          emit({{"event", "error"}, {"kind", "hang"}, {"message", res.detail}});
          return 5;
        case gpuoc::KernelRunResult::Status::Error:
          emit({{"event", "error"}, {"kind", "error"}, {"message", res.detail}});
          return 2;
      }
    }
    json line{{"event", "iteration"},
              {"n", n},
              {"ok", !mismatch},
              {"checksum", hex64(mismatch ? bad_sum : reference)},
              {"seconds", seconds},
              {"work", gpuoc::kernel_gop(params) * good}};
    if (mismatch) line["expected"] = hex64(reference);
    emit(line);
  }
  return 0;
}
