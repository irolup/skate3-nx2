/**
 * @file        core/threading_posix.cpp
 * @brief       POSIX platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>
#include <switch.h>
#include <vector>
#include <mutex>
#include <cstdio>
#include <unistd.h>



#include <signal.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <deque>
#include <limits>
#include <memory>

#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#if 0  // Horizon: no robust mutexes / linux-only
#endif

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/logging.h>
#include <rex/thread/timer_queue.h>

#include <sched.h>

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>

#include <rex/main_android.h>
#include <rex/string/util.h>
#endif

#if 0  // Horizon: no robust mutexes / linux-only
// SIGEV_THREAD_ID in timer_create(...) is a Linux extension
#define REX_HAS_SIGEV_THREAD_ID 1
#ifdef __GLIBC__
#define sigev_notify_thread_id _sigev_un._tid
#endif
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#define gettid() reinterpret_cast<uintptr_t>(pthread_self())
#endif
#else
#define REX_HAS_SIGEV_THREAD_ID 0
#endif


extern "C" void rex_switch2_sampler_anchor();
extern "C" int main(int, char**);

// ---------------------------------------------------------------------------
// Thread sampler (Switch 2 debugging aid). CPU faults and stalls leave no trace under kaiHEN, so
// every few seconds each thread is paused for a moment and its pc/lr/sp are written to
// sdmc:/switch/skate3/sampler.txt. Use addr2line on build/skate3.elf with
// (pc - runtime_anchor + elf_anchor) to see where each thread is.
// ---------------------------------------------------------------------------
namespace {
struct SamplerEntry {
  Handle handle;
  char name[48];
};
std::mutex g_sampler_mutex;
std::vector<SamplerEntry> g_sampler_entries;

void SamplerRegister(Handle h) {
  std::lock_guard<std::mutex> lock(g_sampler_mutex);
  for (auto& e : g_sampler_entries) {
    if (e.handle == h) return;
  }
  SamplerEntry e{};
  e.handle = h;
  g_sampler_entries.push_back(e);
}
void SamplerSetName(Handle h, const char* name) {
  std::lock_guard<std::mutex> lock(g_sampler_mutex);
  for (auto& e : g_sampler_entries) {
    if (e.handle == h) {
      std::strncpy(e.name, name, sizeof(e.name) - 1);
      return;
    }
  }
}
void SamplerUnregister(Handle h) {
  std::lock_guard<std::mutex> lock(g_sampler_mutex);
  for (size_t i = 0; i < g_sampler_entries.size(); ++i) {
    if (g_sampler_entries[i].handle == h) {
      g_sampler_entries.erase(g_sampler_entries.begin() + i);
      return;
    }
  }
}

constexpr int kSamplerMax = 48;
ThreadContext g_sampler_ctx[kSamplerMax];
Result g_sampler_rc[kSamplerMax];
SamplerEntry g_sampler_snapshot[kSamplerMax];

void SamplerLoop(int interval_ms) {
  FILE* fp = std::fopen("sdmc:/switch/skate3/sampler.txt", "w");
  if (!fp) return;
  std::fprintf(fp, "anchor=0x%llx main=0x%llx interval_ms=%d\n",
               (unsigned long long)(uintptr_t)&rex_switch2_sampler_anchor,
               (unsigned long long)(uintptr_t)&main, interval_ms);
  std::fflush(fp);
  const Handle self = threadGetSelf()->handle;
  for (int round = 1; round <= 400; ++round) {
    svcSleepThread(int64_t(interval_ms) * 1000000);
    int n = 0;
    {
      std::lock_guard<std::mutex> lock(g_sampler_mutex);
      for (const auto& e : g_sampler_entries) {
        if (e.handle == self || n >= kSamplerMax) continue;
        g_sampler_snapshot[n++] = e;
      }
    }
    // Pause everything first, copy registers without allocating or locking, resume, then print.
    for (int i = 0; i < n; ++i) {
      g_sampler_rc[i] = svcSetThreadActivity(g_sampler_snapshot[i].handle, ThreadActivity_Paused);
    }
    for (int i = 0; i < n; ++i) {
      if (R_SUCCEEDED(g_sampler_rc[i])) {
        g_sampler_rc[i] = svcGetThreadContext3(&g_sampler_ctx[i], g_sampler_snapshot[i].handle);
      }
    }
    for (int i = 0; i < n; ++i) {
      svcSetThreadActivity(g_sampler_snapshot[i].handle, ThreadActivity_Runnable);
    }
    std::fprintf(fp, "=== round %d tick=%llu threads=%d ===\n", round,
                 (unsigned long long)armGetSystemTick(), n);
    for (int i = 0; i < n; ++i) {
      if (R_FAILED(g_sampler_rc[i])) {
        std::fprintf(fp, "  %-34s rc=0x%x\n", g_sampler_snapshot[i].name, g_sampler_rc[i]);
        continue;
      }
      const ThreadContext& c = g_sampler_ctx[i];
      s32 ideal_core = -1;
      u64 core_mask = 0;
      svcGetThreadCoreMask(&ideal_core, &core_mask, g_sampler_snapshot[i].handle);
      s32 prio = -1;
      svcGetThreadPriority(&prio, g_sampler_snapshot[i].handle);
      std::fprintf(fp, "  %-34s pc=0x%llx lr=0x%llx sp=0x%llx core=%d mask=0x%llx prio=%d\n",
                   g_sampler_snapshot[i].name, (unsigned long long)c.pc.x,
                   (unsigned long long)c.lr, (unsigned long long)c.sp, int(ideal_core),
                   (unsigned long long)core_mask, int(prio));
    }
    std::fflush(fp);
    fsync(fileno(fp));
  }
  std::fclose(fp);
}
}  // namespace

// Fast statistical profiler (options.txt: prof_hz=100): samples the busy threads (GPU command
// processor, render, main, job workers) at a high rate and writes sdmc:/switch/skate3/prof.txt:
//   "anchor=0x<runtime addr of rex_switch2_sampler_anchor>" then "N <idx> <thread name>" then
//   "S <idx> <pc> <lr>" lines. Symbolize pc/lr with addr2line as for sampler.txt.
namespace {
void FastSamplerLoop(int hz) {
  FILE* fp = std::fopen("sdmc:/switch/skate3/prof.txt", "w");
  if (!fp) return;
  // v2 format: "S <idx> <pc> <lr> [<ret1> <ret2> ...]" where ret* are return addresses from the
  // frame-pointer chain (x29), and "T <ms since profiler start>" markers about once a second.
  std::fprintf(fp, "anchor=0x%llx hz=%d v=2\n",
               (unsigned long long)(uintptr_t)&rex_switch2_sampler_anchor, hz);
  static const char* kWanted[] = {"GPU Commands", "render_thread", "Main XThread", "Job Manager",
                                  "GPU VSync",    "GPU Submit"};
  constexpr int kMaxFrames = 10;
  const Handle self = threadGetSelf()->handle;
  std::vector<std::string> names;
  struct StackRange { Handle handle; uint64_t lo, hi; };
  std::vector<StackRange> stacks;
  struct Sample { uint16_t idx; uint8_t nframes; uint64_t pc, lr; uint64_t frames[kMaxFrames]; };
  std::vector<Sample> pending;
  pending.reserve(4096);
  const uint64_t start_tick = armGetSystemTick();
  const uint64_t tick_freq = armGetSystemTickFreq();
  uint64_t last_flush = start_tick;
  uint64_t last_marker = 0;
  const int64_t period_ns = 1000000000ll / (hz > 0 ? hz : 100);
  for (long iter = 0; iter < 400000; ++iter) {  // bounded: ~66 min at 100 Hz
    svcSleepThread(period_ns);
    SamplerEntry targets[kSamplerMax];
    int n = 0;
    {
      std::lock_guard<std::mutex> lock(g_sampler_mutex);
      for (const auto& e : g_sampler_entries) {
        if (e.handle == self || n >= kSamplerMax) continue;
        for (const char* w : kWanted) {
          if (std::strncmp(e.name, w, std::strlen(w)) == 0) {
            targets[n++] = e;
            break;
          }
        }
      }
    }
    const uint64_t now_tick = armGetSystemTick();
    const uint64_t now_ms = (now_tick - start_tick) * 1000 / tick_freq;
    if (now_ms >= last_marker + 1000) {
      last_marker = now_ms;
      Sample m{};
      m.idx = 0xFFFF;
      m.pc = now_ms;
      pending.push_back(m);
    }
    for (int i = 0; i < n; ++i) {
      ThreadContext ctx;
      Sample smp{};
      Result rc = svcSetThreadActivity(targets[i].handle, ThreadActivity_Paused);
      if (R_SUCCEEDED(rc)) {
        rc = svcGetThreadContext3(&ctx, targets[i].handle);
        if (R_SUCCEEDED(rc)) {
          // Walk the frame records while the thread is paused. Bounds come from the memory
          // mapping that holds the thread's stack, so a stray x29 can never fault.
          const uint64_t sp = ctx.sp;
          StackRange* sr = nullptr;
          for (auto& r : stacks) {
            if (r.handle == targets[i].handle && sp >= r.lo && sp < r.hi) { sr = &r; break; }
          }
          if (!sr) {
            MemoryInfo mi{};
            u32 pi = 0;
            if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, sp)) && mi.perm & Perm_R) {
              for (auto it = stacks.begin(); it != stacks.end(); ++it) {
                if (it->handle == targets[i].handle) { stacks.erase(it); break; }
              }
              stacks.push_back({targets[i].handle, mi.addr, mi.addr + mi.size});
              sr = &stacks.back();
            }
          }
          if (sr) {
            uint64_t fpv = ctx.fp;
            uint64_t prev = sp;
            while (smp.nframes < kMaxFrames && fpv >= prev && (fpv & 15) == 0 &&
                   fpv + 16 <= sr->hi && fpv >= sr->lo) {
              const uint64_t* rec = reinterpret_cast<const uint64_t*>(fpv);
              const uint64_t ret = rec[1];
              if (ret == 0) break;
              smp.frames[smp.nframes++] = ret;
              prev = fpv + 16;
              fpv = rec[0];
            }
          }
        }
        svcSetThreadActivity(targets[i].handle, ThreadActivity_Runnable);
      }
      if (R_FAILED(rc)) continue;
      // name table: one index per distinct thread name incl. its id suffix
      uint16_t idx = 0;
      for (; idx < names.size(); ++idx) {
        if (names[idx] == targets[i].name) break;
      }
      if (idx == names.size()) {
        names.emplace_back(targets[i].name);
        std::fprintf(fp, "N %u %s\n", unsigned(idx), targets[i].name);
      }
      smp.idx = idx;
      smp.pc = ctx.pc.x;
      smp.lr = ctx.lr;
      pending.push_back(smp);
    }
    if (now_tick - last_flush > tick_freq * 5) {
      for (const auto& smp : pending) {
        if (smp.idx == 0xFFFF) {
          std::fprintf(fp, "T %llu\n", (unsigned long long)smp.pc);
          continue;
        }
        std::fprintf(fp, "S %u %llx %llx", unsigned(smp.idx), (unsigned long long)smp.pc,
                     (unsigned long long)smp.lr);
        for (int f = 0; f < smp.nframes; ++f) {
          std::fprintf(fp, " %llx", (unsigned long long)smp.frames[f]);
        }
        std::fputc('\n', fp);
      }
      pending.clear();
      std::fflush(fp);
      last_flush = now_tick;
    }
  }
  std::fclose(fp);
}
}  // namespace

extern "C" void ns2_start_fast_sampler(int hz) {
  pthread_t tid{};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64 * 1024);
  pthread_create(&tid, &attr,
                 [](void* arg) -> void* {
                   FastSamplerLoop(int(reinterpret_cast<intptr_t>(arg)));
                   return nullptr;
                 },
                 reinterpret_cast<void*>(intptr_t(hz)));
  pthread_attr_destroy(&attr);
}

extern "C" __attribute__((noinline, used)) void rex_switch2_sampler_anchor() { __asm__ volatile(""); }
extern "C" void ns2_start_sampler(int interval_ms) {
  pthread_t tid{};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64 * 1024);
  pthread_create(&tid, &attr,
                 [](void* arg) -> void* {
                   SamplerLoop(int(reinterpret_cast<intptr_t>(arg)));
                   return nullptr;
                 },
                 reinterpret_cast<void*>(intptr_t(interval_ms)));
  pthread_attr_destroy(&attr);
}
namespace rex::thread {

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* android_libc_;
// API 26+.
static int (*android_pthread_getname_np_)(pthread_t pthread, char* buf, size_t n);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    android_libc_ = dlopen("libc.so", RTLD_NOW);
    assert_not_null(android_libc_);
    if (android_libc_) {
      android_pthread_getname_np_ = reinterpret_cast<decltype(android_pthread_getname_np_)>(
          dlsym(android_libc_, "pthread_getname_np"));
      assert_not_null(android_pthread_getname_np_);
    }
  }
}

void AndroidShutdown() {
  android_pthread_getname_np_ = nullptr;
  if (android_libc_) {
    dlclose(android_libc_);
    android_libc_ = nullptr;
  }
}
#endif

template <typename _Rep, typename _Period>
inline timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Horizon has no signals: suspend/terminate are cooperative. The target thread
// notices at its next checkpoint (waits, sleeps, yields); user callbacks are
// drained by alertable waits.
enum class SignalType { kThreadSuspend, kThreadUserCallback, kThreadTerminate, k_Count };
void install_signal_handler(SignalType) {}
void thread_checkpoint();

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
#if REX_PLATFORM_MAC
  uint64_t thread_id;
  pthread_threadid_np(nullptr, &thread_id);
  return static_cast<uint32_t>(thread_id);
#else
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#endif
}

void MaybeYield() {
  thread_checkpoint();
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() {
  __sync_synchronize();
}

void Sleep(std::chrono::microseconds duration) {
  thread_checkpoint();
  timespec rqtp = DurationToTimeSpec(duration);
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  do {
    ret = nanosleep(p_rqtp, p_rmtp);
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
bool DispatchCurrentThreadUserCallback();
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  auto deadline = std::chrono::steady_clock::now() + duration;
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      alertable_state_ = false;
      return SleepResult::kAlerted;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      alertable_state_ = false;
      return SleepResult::kSuccess;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    Sleep(std::min(remaining, std::chrono::microseconds(1000)));
  }
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) {
  return pthread_key_delete(static_cast<pthread_key_t>(handle)) == 0;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(static_cast<pthread_key_t>(handle)));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(static_cast<pthread_key_t>(handle), reinterpret_cast<void*>(value)) ==
         0;
}

// Multi-object waits (WaitForMultipleObjects / WaitAny) used to poll every handle with try_lock and
// sleep 1 ms between rounds: up to 1+ ms latency per wait and constant churn on the handles'
// mutexes (which XEvent::Set then contended with). Every signal now bumps a global generation and
// wakes multi-waiters, which sleep on it instead of polling. Waiters snapshot the generation before
// checking their handles, so a signal can never be missed (seq_cst on both sides).
namespace {
std::mutex g_multi_wait_mutex;
std::condition_variable g_multi_wait_cv;
std::atomic<uint64_t> g_multi_wait_generation{0};
std::atomic<int> g_multi_waiters{0};
}  // namespace

// options.txt switch (ns2_wait_wakeups, read once at startup by the app): false restores the original
// behaviour (multi-object waits poll every 1 ms, events/semaphores notify under their lock).
extern "C" {
bool ns2_wait_wakeups = true;
}

static void NotifyMultiWaiters() {
  if (!ns2_wait_wakeups) {
    return;
  }
  g_multi_wait_generation.fetch_add(1, std::memory_order_seq_cst);
  if (g_multi_waiters.load(std::memory_order_seq_cst) > 0) {
    std::lock_guard<std::mutex> lock(g_multi_wait_mutex);
    g_multi_wait_cv.notify_all();
  }
}

class PosixConditionBase {
 public:
  PosixConditionBase() {
#if 0  // Horizon: no robust mutexes / linux-only
    // Use robust mutexes so waits can recover if owner thread terminates.
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
      if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0) {
        auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
        pthread_mutex_destroy(native_mutex);
        pthread_mutex_init(native_mutex, &attr);
      }
      pthread_mutexattr_destroy(&attr);
    }
#endif
  }

  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    bool executed;
    auto predicate = [this] { return this->signaled(); };
#if 0  // Horizon: no robust mutexes / linux-only
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    int lock_result = pthread_mutex_lock(native_mutex);
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (lock_result != 0) {
      return WaitResult::kFailed;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
#else
    std::unique_lock<std::mutex> lock(mutex_);
#endif
    if (predicate()) {
      executed = true;
    } else {
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;  // Did not time out;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
    }
    if (executed) {
      post_execution();
      return WaitResult::kSuccess;
    } else {
      return WaitResult::kTimeout;
    }
  }

  static std::pair<WaitResult, size_t> WaitMultiple(std::vector<PosixConditionBase*>&& handles,
                                                    bool wait_all,
                                                    std::chrono::milliseconds timeout) {
    assert_true(!handles.empty());

    if (handles.size() == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    g_multi_waiters.fetch_add(1, std::memory_order_seq_cst);
    struct MultiWaiterGuard {
      ~MultiWaiterGuard() { g_multi_waiters.fetch_sub(1, std::memory_order_seq_cst); }
    } multi_waiter_guard;

    while (true) {
      const uint64_t generation = g_multi_wait_generation.load(std::memory_order_seq_cst);
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;
      bool all_locked = true;

      std::vector<std::unique_lock<std::mutex>> locks;
      locks.reserve(handles.size());

      for (size_t i = 0; i < handles.size(); ++i) {
#if 0  // Horizon: no robust mutexes / linux-only
        auto native_mutex = static_cast<pthread_mutex_t*>(handles[i]->mutex_.native_handle());
        int result = pthread_mutex_trylock(native_mutex);
        if (result == 0 || result == EOWNERDEAD) {
          if (result == EOWNERDEAD) {
            pthread_mutex_consistent(native_mutex);
          }
          locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
        } else {
          all_locked = false;
          break;
        }
#else
        locks.emplace_back(handles[i]->mutex_, std::try_to_lock);
        if (!locks.back().owns_lock()) {
          all_locked = false;
          break;
        }
#endif
      }

      if (!all_locked) {
        locks.clear();
        std::this_thread::yield();
        continue;
      }

      if (wait_all) {
        bool all_signaled = true;
        for (size_t i = 0; i < handles.size(); ++i) {
          if (!handles[i]->signaled()) {
            all_signaled = false;
            break;
          }
          if (first_signaled == std::numeric_limits<size_t>::max()) {
            first_signaled = i;
          }
        }
        condition_met = all_signaled;
      } else {
        for (size_t i = 0; i < handles.size(); ++i) {
          if (handles[i]->signaled()) {
            first_signaled = i;
            condition_met = true;
            break;
          }
        }
      }

      if (condition_met) {
        if (wait_all) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution();
          }
        } else {
          handles[first_signaled]->post_execution();
        }
        return std::make_pair(WaitResult::kSuccess, first_signaled);
      }

      locks.clear();

      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      if (!ns2_wait_wakeups) {
        // Original polling.
        if (timeout == std::chrono::milliseconds::max()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
          auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now);
          std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(1)));
        }
      } else {
        // Sleep until any handle is signaled (generation changes), the timeout, or a 10 ms
        // safety re-check.
        auto wake_time = std::min(end_time, now + std::chrono::milliseconds(10));
        std::unique_lock<std::mutex> lock(g_multi_wait_mutex);
        g_multi_wait_cv.wait_until(lock, wake_time, [generation] {
          return g_multi_wait_generation.load(std::memory_order_seq_cst) != generation;
        });
      }
    }
  }

  virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;
  std::condition_variable cond_;
  std::mutex mutex_;
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  virtual ~PosixCondition() = default;

  bool Signal() override {
    if (!ns2_wait_wakeups) {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      signal_ = true;
      cond_.notify_all();
      return true;
    }
    {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      signal_ = true;
    }
    // Notify after unlocking: a woken waiter would otherwise block right away on mutex_.
    cond_.notify_all();
    NotifyMultiWaiters();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = false;
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  bool signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (release_count > maximum_count_ - count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    if (!ns2_wait_wakeups) {
      cond_.notify_all();
      return true;
    }
    lock.unlock();
    cond_.notify_all();
    NotifyMultiWaiters();
    return true;
  }

 private:
  inline bool signaled() const override { return count_ > 0; }
  inline void post_execution() override {
    count_--;
    cond_.notify_all();
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        cond_.notify_all();
        NotifyMultiWaiters();
      }
      return true;
    }
    return false;
  }

  void* native_handle() const override { return const_cast<std::mutex&>(mutex_).native_handle(); }

 private:
  inline bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  inline void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  virtual ~PosixCondition() { Cancel(); }

  bool Signal() override {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    NotifyMultiWaiters();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = reinterpret_cast<PosixCondition<Timer>*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params, ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
      return false;
    if (pthread_attr_setstacksize(&attr, params.stack_size) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    // Horizon: priorities are not mapped (cooperative fixed priority).
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start_data) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    pthread_attr_destroy(&attr);
    return true;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }

  virtual ~PosixCondition() {
    // Match Canary/Edge behavior.
    // Force-cancel/join from the condition destructor can self-join/crash
    // depending on shutdown ordering, so threads must be stopped explicitly.
  }

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
      {
        std::lock_guard<std::mutex> lock(name_mutex_);
        std::strncpy(result.data(), name_buf_, result.size() - 1);
      }
    }
    return std::string(result.data());
  }

  void set_name(const std::string& name) {
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
      std::lock_guard<std::mutex> nl(name_mutex_);
      std::strncpy(name_buf_, name.c_str(), sizeof(name_buf_) - 1);
      if (horizon_handle_.load() != INVALID_HANDLE) SamplerSetName(horizon_handle_.load(), name_buf_);
    }
  }

#if REX_PLATFORM_ANDROID
  void SetAndroidPreApi26Name(const std::string_view name) {
    if (android_pthread_getname_np_) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    rex::string::util_copy_truncating(android_pre_api_26_name_, name,
                                      rex::countof(android_pre_api_26_name_));
  }
#endif

  uint32_t system_id() const {
#if REX_PLATFORM_MAC
    uint64_t thread_id;
    if (pthread_threadid_np(thread_, &thread_id) != 0) {
      return 0;
    }
    return static_cast<uint32_t>(thread_id);
#else
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thread_));
#endif
  }

  uint64_t affinity_mask() { return affinity_; }

  void set_affinity_mask(uint64_t mask) {
    affinity_ = mask;
    ApplyHorizonScheduling();
  }

  // Horizon: the guest's hardware-thread pinning and priorities are real kernel settings here.
  // The Xbox 360 game relies on them (a low-priority helper sharing a core with a busy thread
  // only runs when that thread blocks); without them its threads race.
  void ApplyHorizonScheduling() {
    const Handle h = horizon_handle_.load(std::memory_order_acquire);
    if (h == INVALID_HANDLE) return;
    if (affinity_) {
      const uint32_t mask = uint32_t(affinity_ & 0x3F);
      if (mask) {
        int ideal = 0;
        while (!(mask & (1u << ideal))) ++ideal;
        const Result rc = svcSetThreadCoreMask(h, ideal, mask);
        if (R_FAILED(rc)) HorizonWarn("svcSetThreadCoreMask", rc);
      }
    }
    // POSIX-style scale: 1 / 8 / 16 / 24 / 32  ->  levels -2..+2  ->  Horizon priority offsets.
    static const s32 base = [] {
      s32 p = 44;
      svcGetThreadPriority(&p, CUR_THREAD_HANDLE);
      return p;
    }();
    int level = (priority_ - 16) / 8;
    if (level < -2) level = -2;
    if (level > 2) level = 2;
    s32 prio = base - level * 4;
    if (prio < 0x1C) prio = 0x1C;
    if (prio > 0x3B) prio = 0x3B;
    const Result rc = svcSetThreadPriority(h, prio);
    if (R_FAILED(rc)) HorizonWarn("svcSetThreadPriority", rc);
  }

  static void HorizonWarn(const char* what, Result rc) {
    static std::atomic<int> count{0};
    if (count.fetch_add(1) < 8) {
      REXSYS_WARN("switch2: {} failed: 0x{:X}", what, rc);
    }
  }

  void SetHorizonHandle(Handle h) {
    horizon_handle_.store(h, std::memory_order_release);
    SamplerRegister(h);
    ApplyHorizonScheduling();
  }

  int priority() {
    WaitStarted();
    return priority_;
  }

  void set_priority(int new_priority) {
    WaitStarted();
    priority_ = new_priority;
    ApplyHorizonScheduling();
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      user_callbacks_.push_back(std::move(callback));
      has_pending_user_callbacks_.store(true, std::memory_order_release);
    }

    // If the callback is queued on the current thread, don't self-signal.
    // Alertable waits drain this queue in normal thread context.
    if (pthread_equal(thread_, pthread_self())) {
      if (alertable_state_) {
        DispatchQueuedUserCallbacks();
      }
      return;
    }

  }

  bool DispatchQueuedUserCallbacks() {
    if (!has_pending_user_callbacks_.load(std::memory_order_acquire)) {
      return false;
    }

    std::function<void()> callback;
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      if (user_callbacks_.empty()) {
        has_pending_user_callbacks_.store(false, std::memory_order_release);
        return false;
      }
      callback = std::move(user_callbacks_.front());
      user_callbacks_.pop_front();
      has_pending_user_callbacks_.store(!user_callbacks_.empty(), std::memory_order_release);
    }
    if (callback) {
      callback();
    }
    return true;
  }

  bool IsSuspendRequested() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    return state_ == State::kSuspended && suspend_count_ > 0;
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0 && state_ == State::kSuspended) {
      state_ = State::kRunning;
      // Async-signal-safe wakeup for WaitSuspended() from signal handler path.
      sem_post(&suspend_sem_);
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    bool is_current_thread = pthread_self() == thread_;
    if (!is_current_thread) {
      while (sem_trywait(&suspend_sem_) == 0) {
      }
    }
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    if (is_current_thread) {
      WaitSuspended();
      return true;
    }

    // Cooperative: the target parks itself at its next checkpoint.
    return true;
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          // This is really bad. Some thread must have called Terminate() on us
          // just before we decided to terminate ourselves
          assert_always();
          for (;;) {
            // Wait for pthread_cancel() to actually happen.
          }
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);

      exit_code_ = exit_code;
      signaled_ = true;
      cond_.notify_all();
    }
    NotifyMultiWaiters();
    if (is_current_thread) {
      pthread_exit(reinterpret_cast<void*>(exit_code));
    } else {
      terminate_requested_.store(true, std::memory_order_release);
      // Wake it if it is parked by a suspend.
      sem_post(&suspend_sem_);
    }
  }

  void WaitStarted() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_signal_.wait(lock, [this] { return state_ != State::kUninitialized; });
  }

  /// Uses sem_wait because it may be called from signal handler context.
  void WaitSuspended() {
    int ret;
    do {
      ret = sem_wait(&suspend_sem_);
    } while (ret == -1 && errno == EINTR);
  }

  void* native_handle() const override { return reinterpret_cast<void*>(thread_); }

 private:
  static void* ThreadStartRoutine(void* parameter);
  inline bool signaled() const override { return signaled_; }
  inline void post_execution() override {
    if (thread_) {
      pthread_join(thread_, nullptr);
    }
    sem_destroy(&suspend_sem_);
  }
  pthread_t thread_;
  bool signaled_;
  int exit_code_;
  State state_;             // Protected by state_mutex_
  uint32_t suspend_count_;  // Protected by state_mutex_
  sem_t suspend_sem_;
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::deque<std::function<void()>> user_callbacks_;
  std::atomic<bool> has_pending_user_callbacks_{false};
  mutable std::mutex name_mutex_;
  char name_buf_[64] = {};
  uint64_t affinity_ = 0;
  int priority_ = 16;
  std::atomic<Handle> horizon_handle_{INVALID_HANDLE};
 public:
  std::atomic<bool> terminate_requested_{false};
 private:
#if REX_PLATFORM_ANDROID
  // Name accessible via name() on Android before API 26 which added
  // pthread_getname_np.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

PosixWaitHandle::~PosixWaitHandle() = default;

thread_local PosixCondition<Thread>* current_thread_condition_ = nullptr;

bool DispatchCurrentThreadUserCallback() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ && current_thread_condition_->DispatchQueuedUserCallbacks();
}

namespace {

constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);

class ScopedAlertableState {
 public:
  explicit ScopedAlertableState(bool alertable) : alertable_(alertable) {
    if (alertable_) {
      alertable_state_ = true;
    }
  }
  ~ScopedAlertableState() {
    if (alertable_) {
      alertable_state_ = false;
    }
  }

 private:
  bool alertable_;
};

std::chrono::steady_clock::time_point ComputeAlertableDeadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return std::chrono::steady_clock::time_point::max();
  }
  return std::chrono::steady_clock::now() + timeout;
}

bool HasAlertableTimeoutElapsed(std::chrono::steady_clock::time_point deadline) {
  return deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline;
}

std::chrono::milliseconds ComputeAlertableWaitTimeout(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return kAlertablePollSlice;
  }
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  return std::min(remaining, kAlertablePollSlice);
}

}  // namespace

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  void* native_handle() const override { return handle_.native_handle(); }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner) : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset) : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset, bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread) : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  thread_checkpoint();
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (!is_alertable) {
    return posix_wait_handle->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);

  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    auto result = posix_wait_handle->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (!posix_wait_handle_to_signal->condition().Signal()) {
    return WaitResult::kFailed;
  }

  if (!is_alertable) {
    return posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    result = posix_wait_handle_to_wait_on->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  if (!is_alertable) {
    return PosixConditionBase::WaitMultiple(std::move(conditions), wait_all, timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return std::make_pair(WaitResult::kTimeout, 0);
    }
    auto result = PosixConditionBase::WaitMultiple(std::vector<PosixConditionBase*>(conditions),
                                                   wait_all, ComputeAlertableWaitTimeout(deadline));
    if (result.first != WaitResult::kTimeout) {
      return result;
    }
  }
}

class PosixEvent : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  void Pulse() override {
    using namespace std::chrono_literals;
    handle_.Signal();
    MaybeYield();
    Sleep(10us);
    handle_.Reset();
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count), out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count || maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner) : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer : public PosixConditionHandle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(due_time), std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params, std::function<void()> start_routine) {
    auto start_data = new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override { handle_.set_affinity_mask(mask); }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override { handle_.set_priority(new_priority); }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
  set_current_thread_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  thread->handle_.SetHorizonHandle(threadGetSelf()->handle);
  current_thread_condition_ = &thread->handle_;
  {
    // The state and the suspend count must change together: the creator waits for "started" and
    // then calls Resume(). With the count set in a second locked step (as before), a Resume()
    // landing in the gap saw count 0, did nothing, and the thread parked itself forever
    // (random hangs at startup: Audio Worker, GPU Commands or Kernel Dispatch never ran).
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    if (create_suspended) {
      thread->handle_.suspend_count_ = 1;
      thread->handle_.state_ = State::kSuspended;
    } else {
      thread->handle_.state_ = State::kRunning;
    }
    thread->handle_.state_signal_.notify_all();
  }

  if (create_suspended) {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_signal_.wait(lock,
                                       [thread] { return thread->handle_.suspend_count_ == 0; });
  }

  start_routine();

  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  {
    std::unique_lock<std::mutex> lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.cond_.notify_all();
  }
  NotifyMultiWaiters();

  SamplerUnregister(threadGetSelf()->handle);
  current_thread_ = nullptr;
  current_thread_condition_ = nullptr;
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine)))
    return nullptr;
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Threads not created by Thread::Create (typically main thread) still need
  // process-wide signal handlers used by suspend/APC machinery.
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  current_thread_->condition().SetHorizonHandle(threadGetSelf()->handle);
  current_thread_condition_ = &current_thread_->condition();
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  // Function must not return
  assert_always();
}

void set_current_thread_name(const std::string_view name) {
#if REX_PLATFORM_MAC
  pthread_setname_np(std::string(name).c_str());
#else
  if (current_thread_condition_) current_thread_condition_->set_name(std::string(name));
#endif
#if REX_PLATFORM_ANDROID
  if (!android_pthread_getname_np_ && current_thread_) {
    current_thread_->condition().SetAndroidPreApi26Name(name);
  }
#endif
}

void thread_checkpoint() {
  if (!current_thread_condition_) return;
  auto* c = current_thread_condition_;
  if (c->terminate_requested_.load(std::memory_order_acquire)) {
    pthread_exit(reinterpret_cast<void*>(-1));
  }
  if (c->IsSuspendRequested()) {
    c->WaitSuspended();
    if (c->terminate_requested_.load(std::memory_order_acquire)) pthread_exit(reinterpret_cast<void*>(-1));
  }
}

}  // namespace rex::thread

// Global critical region spinning (linked with -Wl,--wrap=pthread_mutex_lock).
// Guest mfmsr/mtmsrd sequences (the Xbox 360 way of making short atomic sections) and kernel object
// lookups all take one recursive mutex. libnx mutexes go to svcArbitrateLock as soon as they are
// contended, so these ~100 ns critical sections turned into syscall convoys (Main XThread was 52%
// blocked, Job Managers 42%, render thread 25%). For that one mutex, retry with trylock for a short
// bounded spin before blocking. Every other mutex goes straight to the real implementation.
extern "C" {
pthread_mutex_t* ns2_global_lock_ptr = nullptr;
int ns2_global_lock_spin = 0;
int ns2_mutex_spin = 0;
int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
static std::atomic<uint64_t> g_global_lock_contended{0};
static std::atomic<uint64_t> g_global_lock_blocked{0};

int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (mutex == ns2_global_lock_ptr && ns2_global_lock_spin > 0) {
    if (pthread_mutex_trylock(mutex) == 0) {
      return 0;
    }
    g_global_lock_contended.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < ns2_global_lock_spin; ++i) {
      __asm__ volatile("yield");
      if (pthread_mutex_trylock(mutex) == 0) {
        return 0;
      }
    }
    g_global_lock_blocked.fetch_add(1, std::memory_order_relaxed);
  } else if (ns2_mutex_spin > 0) {
    // Other mutexes (kernel event objects, caches): a short spin before blocking.
    if (pthread_mutex_trylock(mutex) == 0) {
      return 0;
    }
    for (int i = 0; i < ns2_mutex_spin; ++i) {
      __asm__ volatile("yield");
      if (pthread_mutex_trylock(mutex) == 0) {
        return 0;
      }
    }
  }
  return __real_pthread_mutex_lock(mutex);
}

void ns2_take_global_lock_stats(uint64_t* contended, uint64_t* blocked) {
  *contended = g_global_lock_contended.exchange(0, std::memory_order_relaxed);
  *blocked = g_global_lock_blocked.exchange(0, std::memory_order_relaxed);
}
}  // extern "C"
