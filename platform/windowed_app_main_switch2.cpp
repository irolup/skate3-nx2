// Entry point for windowed apps on Switch 2 (replaces the SDL3 main).
#include <switch.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <pthread.h>

#include <rex/cvar.h>
#include <rex/thread/mutex.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_switch2.h>

extern "C" void ns2_start_sampler(int interval_ms);
extern "C" void ns2_start_fast_sampler(int hz);
extern "C" pthread_mutex_t* ns2_global_lock_ptr;
extern "C" u32 ns2_cpu_target_hz;
extern "C" u32 ns2_gpu_target_hz;
extern "C" u32 ns2_mem_target_hz;
extern "C" int ns2_global_lock_spin;
extern "C" int ns2_mutex_spin;
extern "C" bool ns2_wait_wakeups;
// Master debug switch (options.txt: ns2_debug=true). Off by default for release builds: no
// sampler.txt, no per-file/kernel trace logging, no diagnostics bookkeeping. prof_hz=N (prof.txt)
// and ns2_screenshots are separate opt-ins.
REXCVAR_DEFINE_BOOL(ns2_debug, false, "Switch2",
                    "Enable debug output: sampler.txt, verbose logs and diagnostics statistics");
REXCVAR_DEFINE_INT32(handheld_fps_cap, 0, "Switch2",
                     "Handheld only: present and cap swaps at this many fps (evenly, using the 120 Hz "
                     "screen), e.g. 40 or 30; 0 = off. Docked is unaffected (60).");
extern "C" void ns2_apply_handheld_pacing();
extern "C" void ns2_add_handheld_override(const char* name, const char* value);
REXCVAR_DEFINE_STRING(ns2_ab_phases, "", "Switch2",
                      "A/B experiment cycler: comma-separated phases of '+'-separated settings "
                      "(base, valid, ow, tex, aniso<N>, or name:value), switched every "
                      "ns2_ab_windows 5 s statistics windows; empty = off");
REXCVAR_DEFINE_INT32(ns2_ab_windows, 4, "Switch2",
                     "A/B experiment cycler: 5 s statistics windows per phase");
REXCVAR_DEFINE_INT32(ns2_ab_min_draws, 1000, "Switch2",
                     "A/B experiment cycler: 5 s windows averaging fewer draws per frame (menus, "
                     "loading) don't count towards a phase");
static bool g_ns2_debug = false;
extern "C" bool ns2_debug_enabled() { return g_ns2_debug; }

// trace.txt (rex_s2_trace): lines written before options.txt is read are kept in memory, then
// written out if ns2_debug is on and dropped otherwise; later lines only with ns2_debug.
namespace {
enum class TraceState { kUndecided, kOn, kOff };
TraceState g_trace_state = TraceState::kUndecided;
std::vector<std::string>* g_trace_early = nullptr;
pthread_mutex_t g_trace_mutex = PTHREAD_MUTEX_INITIALIZER;

void TraceWrite(const char* msg) {
  if (FILE* f = std::fopen("sdmc:/switch/skate3/trace.txt", "a")) {
    std::fprintf(f, "%s\n", msg);
    std::fflush(f);
    std::fclose(f);
  }
}

void TraceDecide(bool enabled) {
  pthread_mutex_lock(&g_trace_mutex);
  g_trace_state = enabled ? TraceState::kOn : TraceState::kOff;
  if (g_trace_early) {
    if (enabled) {
      for (const std::string& line : *g_trace_early) {
        TraceWrite(line.c_str());
      }
    }
    delete g_trace_early;
    g_trace_early = nullptr;
  }
  pthread_mutex_unlock(&g_trace_mutex);
}
}  // namespace

extern "C" void rex_s2_trace(const char* msg) {
  pthread_mutex_lock(&g_trace_mutex);
  switch (g_trace_state) {
    case TraceState::kUndecided:
      if (!g_trace_early) {
        g_trace_early = new std::vector<std::string>();
      }
      g_trace_early->emplace_back(msg);
      break;
    case TraceState::kOn:
      TraceWrite(msg);
      break;
    case TraceState::kOff:
      break;
  }
  pthread_mutex_unlock(&g_trace_mutex);
}
REXCVAR_DEFINE_BOOL(ns2_wait_wakeups, true, "Switch2",
                    "Multi-object waits sleep on a signal generation and events notify outside "
                    "their lock (false = original 1 ms polling)");
REXCVAR_DEFINE_INT32(ns2_mutex_spin, 50, "Switch2",
                     "Trylock attempts on any other mutex before blocking (0 = off)");
REXCVAR_DEFINE_INT32(ns2_global_lock_spin, 200, "Switch2",
                     "Trylock attempts on the global critical region before blocking (0 = off)");

namespace {

// Optional sdmc:/switch/skate3/options.txt: one "name=value" per line ('#' comments). Any SDK
// cvar can be set (e.g. readback_resolve=none, gpu_revalidate_guest_memory=false); the special key
// cpu_mhz=<MHz|max|default> sets the CPU clock through clkrst. Lets settings be tried on the
// console without rebuilding. Results are collected in `notes` and logged once logging is up.
// options.txt: cpu_mhz / gpu_mhz = <MHz | max | default>. Logs the current clock, the possible rates
// (a range or a list, as reported by the system) and the result into trace.txt. The target is kept
// so the app can set it again after the system resets it (HOME menu, dock/undock).
void ApplyClockOption(std::vector<std::string>& notes, PcvModuleId module, const char* label,
                      const std::string& value, u32& target_out) {
  const std::string tag = std::string("options: ") + label + " clock ";
  Result rc = clkrstInitialize();
  if (R_FAILED(rc)) {
    notes.push_back("options: clkrstInitialize failed 0x" + std::to_string(rc));
    return;
  }
  ClkrstSession session;
  rc = clkrstOpenSession(&session, module, 3);
  if (R_FAILED(rc)) {
    notes.push_back(tag + "session failed 0x" + std::to_string(rc));
    clkrstExit();
    return;
  }
  u32 current = 0;
  clkrstGetClockRate(&session, &current);
  u32 rates[32] = {};
  PcvClockRatesListType type;
  s32 count = 0;
  clkrstGetPossibleClockRates(&session, rates, 32, &type, &count);
  std::string list;
  u32 best = 0;
  for (s32 i = 0; i < count && i < 32; ++i) {
    list += std::to_string(rates[i] / 1000000) + " ";
    best = std::max(best, rates[i]);
  }
  u32 target = current;
  if (value == "max") {
    target = best;
  } else if (value != "default" && !value.empty()) {
    target = u32(std::atoi(value.c_str())) * 1000000u;
  }
  notes.push_back(tag + std::to_string(current / 1000000) + " MHz, possible (type " +
                  std::to_string(int(type)) + ", " + std::to_string(count) + "): " + list +
                  "-> requesting " + std::to_string(target / 1000000) + " MHz");
  target_out = (value == "default") ? 0 : target;
  if (target && target != current) {
    rc = clkrstSetClockRate(&session, target);
    u32 now = 0;
    clkrstGetClockRate(&session, &now);
    notes.push_back(tag + "set rc=0x" + std::to_string(rc) + ", now " + std::to_string(now / 1000000) +
                    " MHz");
  }
  clkrstCloseSession(&session);
}

void ApplyOptionsFile(std::vector<std::string>& notes) {
  FILE* f = std::fopen("sdmc:/switch/skate3/options.txt", "r");
  if (!f) {
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
    size_t b = l.find_first_not_of(" \t");
    if (b == std::string::npos || l[b] == '#') continue;
    l = l.substr(b);
    size_t eq = l.find('=');
    if (eq == std::string::npos) continue;
    std::string name = l.substr(0, eq), value = l.substr(eq + 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    size_t vb = value.find_first_not_of(" \t");
    value = vb == std::string::npos ? std::string() : value.substr(vb);
    if (name == "prof_hz") {
      const int hz = std::atoi(value.c_str());
      if (hz > 0) {
        ns2_start_fast_sampler(std::min(hz, 500));
        notes.push_back("options: fast profiler at " + std::to_string(hz) + " Hz -> prof.txt");
      }
      continue;
    }
    if (name == "cpu_mhz") {
      ApplyClockOption(notes, PcvModuleId_CpuBus, "cpu", value, ns2_cpu_target_hz);
      continue;
    }
    if (name == "gpu_mhz") {
      ApplyClockOption(notes, PcvModuleId_GPU, "gpu", value, ns2_gpu_target_hz);
      continue;
    }
    if (name == "mem_mhz") {
      ApplyClockOption(notes, PcvModuleId_EMC, "mem", value, ns2_mem_target_hz);
      continue;
    }
    if (name.rfind("handheld.", 0) == 0) {
      // Handheld-only value of a setting (applied on undock, the normal value restored on dock).
      const std::string setting = name.substr(9);
      const bool known = rex::cvar::GetFlagInfo(setting) != nullptr;
      if (known) {
        ns2_add_handheld_override(setting.c_str(), value.c_str());
      }
      notes.push_back("options: handheld only: " + setting + "=" + value +
                      (known ? "" : " REJECTED (unknown name?)"));
      continue;
    }
    bool ok = rex::cvar::SetFlagByName(name, value);
    notes.push_back("options: " + name + "=" + value + (ok ? " applied" : " REJECTED (unknown name?)"));
  }
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  // All data lives under the app folder on the SD card.
  chdir("sdmc:/switch/skate3");
  // Mesa 26.2 NVK/NAK on the console miscompiles pixel shaders that use discard (triangle-edge
  // dropouts) when uniform values are promoted to uniform GPRs; disabling that promotion cures it.
  // Must be set before any Vulkan device exists. (An explicit NAK_DEBUG wins: overwrite = 0.)
  setenv("NAK_DEBUG", "nougpr", 0);
  rex_s2_trace("main: entered");
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  std::vector<std::string> option_notes;
  ApplyOptionsFile(option_notes);
  g_ns2_debug = REXCVAR_GET(ns2_debug);
  TraceDecide(g_ns2_debug);
  if (!g_ns2_debug && !rex::cvar::HasNonDefaultValue("log_keep_sessions")) {
    // Release: keep the log of this launch and of the previous one only.
    rex::cvar::SetFlagByName("log_keep_sessions", "2");
  }
  option_notes.push_back(std::string("options: ns2_debug = ") + (g_ns2_debug ? "on" : "off"));
  option_notes.push_back("options: handheld_fps_cap = " +
                         std::to_string(int(REXCVAR_GET(handheld_fps_cap))));
  if (g_ns2_debug) {
    ns2_start_sampler(3000);  // writes sdmc:/switch/skate3/sampler.txt every 3 s
    // Sampled host GPU frame timing for the GPU stats line (one frame in 16), unless set explicitly.
    if (!rex::cvar::HasNonDefaultValue("vulkan_gpu_frame_span_sample_interval")) {
      rex::cvar::SetFlagByName("vulkan_gpu_frame_span_sample_interval", "16");
    }
  }
  // Global critical region spin-then-block (see threading_switch2.cpp). Set before guest threads.
  ns2_global_lock_ptr = rex::thread::global_critical_region::mutex().native_handle();
  ns2_global_lock_spin = std::max(0, int(REXCVAR_GET(ns2_global_lock_spin)));
  ns2_mutex_spin = std::max(0, int(REXCVAR_GET(ns2_mutex_spin)));
  ns2_wait_wakeups = REXCVAR_GET(ns2_wait_wakeups);
  option_notes.push_back(std::string("options: wait wakeups = ") +
                         (ns2_wait_wakeups ? "on" : "off (1 ms polling)"));
  option_notes.push_back("options: mutex spin = " + std::to_string(ns2_mutex_spin));
  option_notes.push_back("options: global lock spin = " + std::to_string(ns2_global_lock_spin));
  rex::InitLoggingEarly();
  for (const auto& note : option_notes) {
    rex_s2_trace(note.c_str());  // trace.txt: logging to the SD card is not up yet
    REXLOG_INFO("{}", note);
  }
  ns2_apply_handheld_pacing();  // after logging is up (it logs its result)

  int result = EXIT_FAILURE;
  {
    rex::ui::Switch2WindowedAppContext app_context;
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }
    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));
    rex_s2_trace("main: app OnInitialize");
    bool initialized = app->OnInitialize();
    result = initialized ? app_context.RunMainLoop() : EXIT_FAILURE;
    app->InvokeOnDestroy();
  }
  rex::ShutdownLogging();
  return result;
}
