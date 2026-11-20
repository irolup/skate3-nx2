// Under kaiHEN, CPU faults and abort() kill the process silently. These hooks
// record C++ exceptions, std::terminate and abort() to the SD card (fsync'd)
// before the process dies, so a boot failure leaves a trace.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <pthread.h>
#include <mutex>
#include <typeinfo>

namespace {

void Record(const char* kind, const char* detail, uintptr_t caller) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  if (FILE* f = std::fopen("sdmc:/switch/skate3/crash_hooks.txt", "a")) {
    std::fprintf(f, "%s: %s caller=0x%llx\n", kind, detail ? detail : "(null)",
                 static_cast<unsigned long long>(caller));
    std::fflush(f);
    fsync(fileno(f));
    std::fclose(f);
  }
}

[[noreturn]] void TerminateHandler() {
  const char* what = "(no active exception)";
  if (auto e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      Record("std::terminate", ex.what(), (uintptr_t)__builtin_return_address(0));
      std::abort();
    } catch (...) {
      what = "(non-std exception)";
    }
  }
  Record("std::terminate", what, (uintptr_t)__builtin_return_address(0));
  std::abort();
}

__attribute__((constructor)) void InstallTerminateHandler() {
  std::set_terminate(TerminateHandler);
}

}  // namespace

extern "C" {
void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void) {
  Record("abort", "abort() called", (uintptr_t)__builtin_return_address(0));
  __real_abort();
}

bool ns2_debug_enabled();  // platform/windowed_app_main_switch2.cpp
void __real___cxa_throw(void*, void*, void (*)(void*)) __attribute__((noreturn));
void __wrap___cxa_throw(void* obj, void* tinfo, void (*dest)(void*)) {
  // Every throw (most are caught and harmless) only with ns2_debug; abort/terminate always.
  if (ns2_debug_enabled()) {
    Record("throw", tinfo ? static_cast<const std::type_info*>(tinfo)->name() : "(null)",
           (uintptr_t)__builtin_return_address(0));
  }
  __real___cxa_throw(obj, tinfo, dest);
}

// libounce's pthread_detach returns ENOSYS, which makes std::thread::detach()
// throw std::system_error. Detached threads simply never get joined.
int __wrap_pthread_detach(pthread_t) { return 0; }
}
