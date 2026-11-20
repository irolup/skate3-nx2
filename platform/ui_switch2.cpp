// Switch 2 UI platform layer: surface, window, UI loop (no SDL3 on Horizon).
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/surface_switch2.h>
#include <rex/ui/window_switch2.h>
#include <rex/ui/windowed_app_context_switch2.h>

namespace rex::ui {

// ---- Surface ----
bool Switch2ViSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  u32 w = 0, h = 0;
  if (nwindow_) {
    nwindowGetDimensions(static_cast<NWindow*>(nwindow_), &w, &h);
  }
  width_out = w;
  height_out = h;
  return w && h;
}

// ---- Window ----
std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<WindowSwitch2>(app_context, title, desired_logical_width,
                                         desired_logical_height);
}

WindowSwitch2::WindowSwitch2(WindowedAppContext& app_context, const std::string_view title,
                             uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

Switch2WindowedAppContext& WindowSwitch2::switch2_context() const {
  return static_cast<Switch2WindowedAppContext&>(app_context());
}

WindowSwitch2::~WindowSwitch2() {
  EnterDestructor();
  if (open_) {
    switch2_context().UnregisterWindow(this);
    open_ = false;
  }
}

bool WindowSwitch2::OpenImpl() {
  u32 width = 1280, height = 720;
  if (NWindow* nw = nwindowGetDefault()) {
    nwindowGetDimensions(nw, &width, &height);
  }
  open_ = true;
  switch2_context().RegisterWindow(this);
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(width, height, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) {
    return true;
  }
  OnFocusUpdate(true, destruction_receiver);
  return true;
}

void WindowSwitch2::RequestCloseImpl() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
  if (open_) {
    switch2_context().UnregisterWindow(this);
    open_ = false;
  }
  OnAfterClose();
}

void WindowSwitch2::HandleCloseRequest() {
  RequestCloseImpl();
}

std::unique_ptr<Surface> WindowSwitch2::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_Switch2Vi)) {
    return nullptr;
  }
  return std::make_unique<Switch2ViSurface>(nwindowGetDefault());
}

void WindowSwitch2::RequestPaintImpl() {
  switch2_context().RequestPaint();
}

void WindowSwitch2::HandlePaint() {
  OnPaint();
}

void WindowSwitch2::HandleSurfaceReset() {
  if (!open_) {
    return;
  }
  OnSurfaceChanged(true);
  RequestPaint();
}

// ---- UI loop ----
Switch2WindowedAppContext::~Switch2WindowedAppContext() = default;

namespace {
// HOME menu handling. While the HOME menu is open the UI thread is blocked inside
// appletMainLoop(), so focus changes are seen through applet hooks (called from inside that
// loop) and acted on by the UI loop afterwards.
AppletHookCookie g_applet_hook_cookie;
std::atomic<bool> g_resume_pending{false};
std::atomic<bool> g_clock_reapply_pending{false};

void AppletHookCallback(AppletHookType hook, void*) {
  const AppletFocusState focus = appletGetFocusState();
  REXLOG_INFO("Switch 2: applet hook {} (focus state {})", int(hook), int(focus));
  if ((hook == AppletHookType_OnFocusState && focus == AppletFocusState_InFocus) ||
      hook == AppletHookType_OnResume) {
    g_resume_pending.store(true, std::memory_order_release);
  }
  if (hook == AppletHookType_OnOperationMode || hook == AppletHookType_OnPerformanceMode) {
    // Dock/undock: the system picks new clocks for the mode; set ours again.
    g_clock_reapply_pending.store(true, std::memory_order_release);
  }
}

// VI2 shows the layer selected on the display's "default" endpoint. The HOME menu takes that
// selection; give it back to the game's layer.
void ReselectGameLayer(const char* when) {
#if defined(__SWITCH2__)
  NWindow* nw = nwindowGetDefault();
  Vi2Layer* layer = nullptr;
  Vi2Display* display = nullptr;
  Result rc_layer = nwindowGetVi2Layer(nw, &layer);
  Result rc_display = nwindowGetVi2Display(nw, &display);
  if (R_FAILED(rc_layer) || R_FAILED(rc_display) || !layer || !display) {
    REXLOG_WARN("Switch 2: {}: no VI2 layer/display (rc 0x{:X} / 0x{:X})", when, rc_layer,
                rc_display);
    return;
  }
  u64 slots_before = 0, slots_after = 0;
  vi2GetLayerAvailability(layer, &slots_before);
  Vi2Endpoint endpoint;
  vi2CreateEndpoint(&endpoint, display, "default");
  Result rc_select = vi2SelectLayer(layer, &endpoint);
  vi2CloseEndpoint(&endpoint);
  vi2GetLayerAvailability(layer, &slots_after);
  REXLOG_INFO("Switch 2: {}: re-selected the game layer (rc 0x{:X}), slots 0x{:X} -> 0x{:X}", when,
              rc_select, slots_before, slots_after);
#else
  (void)when;
#endif
}
}  // namespace

bool Switch2WindowedAppContext::Initialize() {
  // Ask to be told about HOME-menu exits instead of being killed.
  appletLockExit();
  appletHook(&g_applet_hook_cookie, AppletHookCallback, nullptr);
  // Keep the default suspend behaviour, but still deliver focus hooks.
  Result rc = appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
  REXLOG_INFO("Switch 2: applet hook registered, focus handling mode rc=0x{:X}", rc);
  return true;
}

void Switch2WindowedAppContext::NotifyUILoopOfPendingFunctions() {
  {
    std::lock_guard lock(wake_mutex_);
    wake_pending_ = true;
  }
  wake_cv_.notify_one();
}

void Switch2WindowedAppContext::PlatformQuitFromUIThread() {
  NotifyUILoopOfPendingFunctions();
}

void Switch2WindowedAppContext::RegisterWindow(WindowSwitch2* window) {
  windows_.push_back(window);
}

void Switch2WindowedAppContext::UnregisterWindow(WindowSwitch2* window) {
  windows_.erase(std::remove(windows_.begin(), windows_.end(), window), windows_.end());
}

void Switch2WindowedAppContext::RequestPaint() {
  if (!paint_pending_.exchange(true, std::memory_order_acq_rel)) {
    NotifyUILoopOfPendingFunctions();
  }
}

// ---- System software keyboard (XamShowKeyboardUI) ----
// Shows the Horizon keyboard library applet; blocks the calling (kernel dispatch) thread until
// the user confirms or cancels. Returns 1 = text entered, 0 = cancelled by the user,
// -1 = applet unavailable (caller falls back to the default text).
int ShowSystemKeyboard(const std::string& header, const std::string& sub,
                       const std::string& initial, uint32_t max_chars, std::string& out_text) {
  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_FAILED(rc)) {
    REXLOG_ERROR("Switch 2: swkbdCreate failed: 0x{:X}", rc);
    return -1;
  }
  swkbdConfigMakePresetDefault(&kbd);
  if (!header.empty()) {
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetGuideText(&kbd, header.c_str());
  }
  if (!sub.empty()) {
    swkbdConfigSetSubText(&kbd, sub.c_str());
  }
  if (!initial.empty()) {
    swkbdConfigSetInitialText(&kbd, initial.c_str());
  }
  if (max_chars) {
    swkbdConfigSetStringLenMax(&kbd, std::min<uint32_t>(max_chars, 500));
  }
  // UTF-8 output: up to 4 bytes per character plus the terminator.
  std::string buffer(size_t(max_chars ? std::min<uint32_t>(max_chars, 500) : 500) * 4 + 1, '\0');
  REXLOG_INFO("Switch 2: showing system keyboard ('{}', max {} chars)", header, max_chars);
  rc = swkbdShow(&kbd, buffer.data(), buffer.size());
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc)) {
    out_text.assign(buffer.c_str());
    REXLOG_INFO("Switch 2: system keyboard returned {} bytes", out_text.size());
    return 1;
  }
  if (rc == MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit)) {
    REXLOG_INFO("Switch 2: system keyboard cancelled");
    return 0;
  }
  REXLOG_ERROR("Switch 2: swkbdShow failed: 0x{:X}", rc);
  return -1;
}

extern "C" u32 ns2_cpu_target_hz;
u32 ns2_cpu_target_hz = 0;
extern "C" u32 ns2_gpu_target_hz;
u32 ns2_gpu_target_hz = 0;
extern "C" u32 ns2_mem_target_hz;
u32 ns2_mem_target_hz = 0;
extern "C" uint64_t rex_clock_gap_events;
extern "C" uint64_t rex_clock_gap_ms_total;

namespace {
u32 ReadClockMHz(PcvModuleId module) {
  ClkrstSession session;
  if (R_FAILED(clkrstOpenSession(&session, module, 3))) {
    return 0;
  }
  u32 hz = 0;
  clkrstGetClockRate(&session, &hz);
  clkrstCloseSession(&session);
  return hz / 1000000;
}

void ReapplyClock(PcvModuleId module, u32 target_hz, const char* label) {
  if (!target_hz) {
    return;
  }
  ClkrstSession session;
  if (R_FAILED(clkrstOpenSession(&session, module, 3))) {
    return;
  }
  u32 hz = 0;
  clkrstGetClockRate(&session, &hz);
  if (hz != target_hz) {
    Result rc = clkrstSetClockRate(&session, target_hz);
    u32 now = 0;
    clkrstGetClockRate(&session, &now);
    REXLOG_INFO("Switch 2: {} clock was {} MHz, re-set to {} MHz (rc=0x{:X})", label, hz / 1000000,
                now / 1000000, rc);
  }
  clkrstCloseSession(&session);
}

void ReapplyCpuClock() {
  ReapplyClock(PcvModuleId_CpuBus, ns2_cpu_target_hz, "CPU");
  ReapplyClock(PcvModuleId_GPU, ns2_gpu_target_hz, "GPU");
  ReapplyClock(PcvModuleId_EMC, ns2_mem_target_hz, "memory");
}

std::atomic<uint32_t> g_clock_resets{0};

// Handheld-only settings (options.txt: handheld.<setting>=<value>): applied when the console is in
// handheld mode, the normal value restored when docked. The docked value is the one in effect when
// the first mode is applied (after options.txt).
struct HandheldOverride {
  std::string name;
  std::string handheld_value;
  std::string docked_value;
  bool docked_value_captured = false;
};
std::vector<HandheldOverride> g_handheld_overrides;

void ApplyHandheldOverrides(bool handheld, const char* when) {
  for (HandheldOverride& override_entry : g_handheld_overrides) {
    if (!override_entry.docked_value_captured) {
      override_entry.docked_value = rex::cvar::GetFlagByName(override_entry.name);
      override_entry.docked_value_captured = true;
    }
    const std::string& value =
        handheld ? override_entry.handheld_value : override_entry.docked_value;
    const bool ok = rex::cvar::SetFlagByName(override_entry.name, value);
    REXLOG_INFO("Switch 2: {} ({}): {}={}{}", handheld ? "handheld setting" : "docked setting",
                when, override_entry.name, value, ok ? "" : " REJECTED");
  }
}

// The startup pacing line is written before the log file exists: keep it and log it again from
// the first 5 s statistics tick.
std::mutex g_pacing_message_mutex;
std::string g_pacing_message;
std::atomic<bool> g_pacing_relog{false};

// Handheld frame pacing (options.txt: handheld_fps_cap=N, 0 = off). On the handheld screen (120 Hz
// capable, variable refresh) the window is told to present at N fps evenly, and the guest's swaps are
// capped to N; docked, both return to normal (60). Without this a ~42 fps game on a 60 Hz FIFO
// swapchain shows frames for 1 or 2 refreshes in an uneven mix, which feels choppy.
void ApplyHandheldPacing(const char* when) {
  const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
  ApplyHandheldOverrides(handheld, when);
  const int cap = rex::cvar::Query<int32_t>("handheld_fps_cap");
  if (cap <= 0) {
    return;
  }
  NWindow* nw = nwindowGetDefault();
  Result rc = 0;
  if (handheld) {
    // fps at a 120 Hz display = cap; fps at a 60 Hz display = the largest of 30/20/... that divides 60
    // and does not exceed the cap (so a 60 Hz fallback is still even).
    const u32 fps60 = cap >= 30 ? 30 : (cap >= 20 ? 20 : 15);
    rc = nwindowSetFrameRates(nw, u32(cap), fps60, true);
    rex::cvar::SetFlagByName("swap_frame_limit_fps", std::to_string(cap));
  } else {
    rc = nwindowSetFrameRates(nw, 0, 0, true);
    rex::cvar::SetFlagByName("swap_frame_limit_fps", "60");
  }
  u16 num = 0, den = 0;
  nwindowGetPresentInterval(nw, &num, &den);
  std::string message = fmt::format("({}): {} cap {} fps -> rc=0x{:X}, present interval {}/{} s",
                                    when, handheld ? "handheld" : "docked", handheld ? cap : 60,
                                    rc, num, den);
  REXLOG_INFO("Switch 2: pacing {}", message);
  {
    std::lock_guard<std::mutex> lock(g_pacing_message_mutex);
    g_pacing_message = std::move(message);
  }
  g_pacing_relog.store(true, std::memory_order_release);
}

// ---- A/B experiment cycler (options.txt: ns2_ab_phases=..., ns2_ab_windows=N) ----
// Cycles through phases of settings, N 5-second statistics windows each, so one run measures
// several experiments in the same place. The phase label is printed in the "GPU stats", "Vulkan GPU
// profile", "RT transfers" and "Texture loads" lines as <phase>#<window in phase>/c<cycle>.
// Phases are separated by ',', settings within a phase by '+'. Shortcuts: base (nothing),
// valid (ns2_rt_valid_tracking), ow / noow (ns2_rt_skip_overwritten_transfers on / off), tex
// (ns2_resolve_to_texture), eager (ns2_eager_texture_loads), rect2 (ns2_rt_overwrite_max_rects=2),
// pw (ns2_rt_overwrite_partial_width), aniso<N> (anisotropic_override=N); any setting as name:value.
// A setting not listed in a phase has its startup value.
struct AbPhase {
  std::string label;
  std::vector<std::pair<std::string, std::string>> settings;
};
std::vector<AbPhase> g_ab_phases;
std::vector<std::pair<std::string, std::string>> g_ab_baseline;
bool g_ab_initialized = false;
size_t g_ab_phase = 0;
uint32_t g_ab_window = 1;
uint32_t g_ab_cycle = 1;
std::string g_ab_label = "-";
std::string g_ab_finished_label = "-";

std::string TrimCopy(const std::string& text) {
  size_t b = text.find_first_not_of(" \t");
  if (b == std::string::npos) {
    return std::string();
  }
  size_t e = text.find_last_not_of(" \t");
  return text.substr(b, e - b + 1);
}

void AbApplyPhase(const char* reason) {
  const AbPhase& phase = g_ab_phases[g_ab_phase];
  std::string applied;
  for (const auto& baseline : g_ab_baseline) {
    std::string value = baseline.second;
    for (const auto& setting : phase.settings) {
      if (setting.first == baseline.first) {
        value = setting.second;
      }
    }
    rex::cvar::SetFlagByName(baseline.first, value);
    applied += fmt::format(" {}={}", baseline.first, value);
  }
  g_ab_label = fmt::format("{}#{}/c{}", phase.label, g_ab_window, g_ab_cycle);
  REXLOG_INFO("A/B: phase '{}' ({}), cycle {}:{}", phase.label, reason, g_ab_cycle, applied);
}

void AbInitialize() {
  if (g_ab_initialized) {
    return;
  }
  g_ab_initialized = true;
  const std::string spec = rex::cvar::Query<std::string>("ns2_ab_phases");
  size_t start = 0;
  while (start <= spec.size()) {
    size_t comma = spec.find(',', start);
    std::string token = TrimCopy(spec.substr(start, comma == std::string::npos ? std::string::npos
                                                                               : comma - start));
    start = comma == std::string::npos ? spec.size() + 1 : comma + 1;
    if (token.empty()) {
      continue;
    }
    AbPhase phase;
    phase.label = token;
    size_t item_start = 0;
    while (item_start <= token.size()) {
      size_t plus = token.find('+', item_start);
      std::string item = TrimCopy(token.substr(
          item_start, plus == std::string::npos ? std::string::npos : plus - item_start));
      item_start = plus == std::string::npos ? token.size() + 1 : plus + 1;
      if (item.empty() || item == "base") {
        continue;
      }
      if (item == "valid") {
        phase.settings.emplace_back("ns2_rt_valid_tracking", "true");
      } else if (item == "ow") {
        phase.settings.emplace_back("ns2_rt_skip_overwritten_transfers", "true");
      } else if (item == "tex") {
        phase.settings.emplace_back("ns2_resolve_to_texture", "true");
      } else if (item == "eager") {
        phase.settings.emplace_back("ns2_eager_texture_loads", "true");
      } else if (item == "rect2") {
        phase.settings.emplace_back("ns2_rt_overwrite_max_rects", "2");
      } else if (item == "pw") {
        phase.settings.emplace_back("ns2_rt_overwrite_partial_width", "true");
      } else if (item == "noow") {
        phase.settings.emplace_back("ns2_rt_skip_overwritten_transfers", "false");
      } else if (item.compare(0, 5, "aniso") == 0 && item.size() > 5) {
        phase.settings.emplace_back("anisotropic_override", item.substr(5));
      } else {
        size_t separator = item.find_first_of(":=");
        if (separator == std::string::npos) {
          REXLOG_WARN("A/B: unknown item '{}' in phase '{}' (ignored)", item, token);
          continue;
        }
        phase.settings.emplace_back(TrimCopy(item.substr(0, separator)),
                                    TrimCopy(item.substr(separator + 1)));
      }
    }
    for (const auto& setting : phase.settings) {
      bool known = false;
      for (const auto& baseline : g_ab_baseline) {
        known = known || baseline.first == setting.first;
      }
      if (!known) {
        g_ab_baseline.emplace_back(setting.first, rex::cvar::GetFlagByName(setting.first));
      }
    }
    g_ab_phases.push_back(std::move(phase));
  }
  if (!g_ab_phases.empty()) {
    AbApplyPhase("start");
  }
}
}  // namespace

extern "C" bool ns2_ab_cycle_enabled() {
  AbInitialize();
  return !g_ab_phases.empty();
}

extern "C" const char* ns2_ab_label() {
  AbInitialize();
  return g_ab_label.c_str();
}

// Called with every 5 s statistics line (GPU command thread): returns the label of the window
// that just ended and moves to the next phase when its windows are done.
extern "C" const char* ns2_ab_tick(uint32_t avg_draws) {
  AbInitialize();
  if (g_ab_phases.empty()) {
    return "-";
  }
  g_ab_finished_label = g_ab_label;
  // Menus and loading screens (few draws per frame): stay in the phase so every phase gets its
  // windows in gameplay.
  if (avg_draws < uint32_t(std::max(0, int(rex::cvar::Query<int32_t>("ns2_ab_min_draws"))))) {
    g_ab_finished_label += "(hold)";
    return g_ab_finished_label.c_str();
  }
  const uint32_t windows =
      uint32_t(std::max(1, int(rex::cvar::Query<int32_t>("ns2_ab_windows"))));
  if (++g_ab_window > windows) {
    g_ab_window = 1;
    if (++g_ab_phase >= g_ab_phases.size()) {
      g_ab_phase = 0;
      ++g_ab_cycle;
    }
    AbApplyPhase("next");
  } else {
    g_ab_label = fmt::format("{}#{}/c{}", g_ab_phases[g_ab_phase].label, g_ab_window, g_ab_cycle);
  }
  return g_ab_finished_label.c_str();
}

extern "C" void ns2_add_handheld_override(const char* name, const char* value) {
  HandheldOverride override_entry;
  override_entry.name = name;
  override_entry.handheld_value = value;
  g_handheld_overrides.push_back(std::move(override_entry));
}

extern "C" void ns2_apply_handheld_pacing() { ApplyHandheldPacing("startup"); }

// Called every 5 s from the GPU stats line: reports the clocks the system is really running at and
// sets the requested clocks again if the system changed them (counted).
// Effective CPU speed: time for a fixed dependent integer loop (about 1.2 ms at 1.7 GHz). It does
// not follow the requested clock: heat and power limits show up here but not in the clock reading.
static uint32_t CpuBenchMicros() {
  const uint64_t t0 = armGetSystemTick();
  volatile uint32_t sink = 1;
  uint32_t x = 1;
  for (uint32_t i = 0; i < 2000000; ++i) {
    x = x * 1664525u + 1013904223u;
  }
  sink = x;
  (void)sink;
  return uint32_t((armGetSystemTick() - t0) * 1000000ull / armGetSystemTickFreq());
}

extern "C" void ns2_clock_check(uint32_t* cpu_mhz, uint32_t* gpu_mhz, uint32_t* resets,
                                uint32_t* mem_mhz, uint32_t* cpu_bench_us) {
  if (g_pacing_relog.exchange(false, std::memory_order_acq_rel)) {
    std::lock_guard<std::mutex> lock(g_pacing_message_mutex);
    REXLOG_INFO("Switch 2: pacing {} (repeated for the log file)", g_pacing_message);
  }
  *cpu_bench_us = CpuBenchMicros();
  const u32 cpu_now = ReadClockMHz(PcvModuleId_CpuBus);
  const u32 gpu_now = ReadClockMHz(PcvModuleId_GPU);
  const u32 mem_now = ReadClockMHz(PcvModuleId_EMC);
  *cpu_mhz = cpu_now;
  *gpu_mhz = gpu_now;
  *mem_mhz = mem_now;
  bool changed = false;
  if (ns2_mem_target_hz && mem_now != ns2_mem_target_hz / 1000000) {
    ReapplyClock(PcvModuleId_EMC, ns2_mem_target_hz, "memory");
    changed = true;
  }
  if (ns2_cpu_target_hz && cpu_now != ns2_cpu_target_hz / 1000000) {
    ReapplyClock(PcvModuleId_CpuBus, ns2_cpu_target_hz, "CPU");
    changed = true;
  }
  if (ns2_gpu_target_hz && gpu_now != ns2_gpu_target_hz / 1000000) {
    ReapplyClock(PcvModuleId_GPU, ns2_gpu_target_hz, "GPU");
    changed = true;
  }
  if (changed) {
    g_clock_resets.fetch_add(1, std::memory_order_relaxed);
  }
  *resets = g_clock_resets.load(std::memory_order_relaxed);
}

int Switch2WindowedAppContext::RunMainLoop() {
  bool applet_running = true;
  clkrstInitialize();
  AppletFocusState last_focus = appletGetFocusState();
  REXLOG_INFO("Switch 2: focus state {} (CPU {} MHz, GPU {} MHz, memory {} MHz)", int(last_focus),
              ReadClockMHz(PcvModuleId_CpuBus), ReadClockMHz(PcvModuleId_GPU),
              ReadClockMHz(PcvModuleId_EMC));
  // When focus comes back from the HOME menu: re-apply the CPU clock and recreate the surface
  // after a short delay (the display layer may not be ready immediately).
  std::chrono::steady_clock::time_point surface_reset_at{};
  bool surface_reset_pending = false;
  uint64_t last_clock_gap_events = __atomic_load_n(&rex_clock_gap_events, __ATOMIC_RELAXED);
  while (!HasQuitFromUIThread()) {
    {
      std::unique_lock lock(wake_mutex_);
      wake_cv_.wait_for(lock, std::chrono::milliseconds(16), [this] { return wake_pending_; });
      wake_pending_ = false;
    }
    ExecutePendingFunctionsFromUIThread();
    if (HasQuitFromUIThread()) {
      break;
    }
    if (paint_pending_.exchange(false, std::memory_order_acq_rel)) {
      std::vector<WindowSwitch2*> windows = windows_;
      for (WindowSwitch2* window : windows) {
        window->HandlePaint();
      }
    }
    {
      const AppletFocusState focus = appletGetFocusState();
      {
        // The HOME menu suspends the whole process without focus messages reaching the hooks;
        // the guest clock sees the suspension as a gap and absorbs it. Treat it as a resume.
        const uint64_t gap_events = __atomic_load_n(&rex_clock_gap_events, __ATOMIC_RELAXED);
        if (gap_events != last_clock_gap_events) {
          REXLOG_INFO("Switch 2: process suspension detected ({} gaps, {} ms total hidden from "
                      "the guest clock)",
                      gap_events, __atomic_load_n(&rex_clock_gap_ms_total, __ATOMIC_RELAXED));
          last_clock_gap_events = gap_events;
          g_resume_pending.store(true, std::memory_order_release);
        }
      }
      if (g_clock_reapply_pending.exchange(false, std::memory_order_acq_rel)) {
        REXLOG_INFO("Switch 2: operation/performance mode changed (CPU {} MHz, GPU {} MHz)",
                    ReadClockMHz(PcvModuleId_CpuBus), ReadClockMHz(PcvModuleId_GPU));
        ReapplyCpuClock();
        ApplyHandheldPacing("mode change");
      }
      if (g_resume_pending.exchange(false, std::memory_order_acq_rel)) {
        ReapplyCpuClock();
        ReselectGameLayer("resume");
        surface_reset_pending = true;
        surface_reset_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
      }
      if (focus != last_focus) {
        REXLOG_INFO("Switch 2: focus state {} -> {} (CPU {} MHz, GPU {} MHz)", int(last_focus),
                    int(focus), ReadClockMHz(PcvModuleId_CpuBus), ReadClockMHz(PcvModuleId_GPU));
        if (focus == AppletFocusState_InFocus) {
          ReapplyCpuClock();
          surface_reset_pending = true;
          surface_reset_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        }
        last_focus = focus;
      }
      if (surface_reset_pending && std::chrono::steady_clock::now() >= surface_reset_at) {
        surface_reset_pending = false;
        REXLOG_INFO("Switch 2: recreating the window surface after regaining focus");
        std::vector<WindowSwitch2*> windows = windows_;
        for (WindowSwitch2* window : windows) {
          window->HandleSurfaceReset();
        }
        ReselectGameLayer("after surface reset");
      }
    }
    if (applet_running && !appletMainLoop()) {
      applet_running = false;
      REXLOG_INFO("Switch 2: exit requested by the system");
      std::vector<WindowSwitch2*> windows = windows_;
      for (WindowSwitch2* window : windows) {
        window->HandleCloseRequest();
      }
      RequestDeferredQuit();
    }
  }
  appletUnlockExit();
  return EXIT_SUCCESS;
}

}  // namespace rex::ui
