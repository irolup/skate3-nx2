// Horizon has no signals, so structured exception handling is inert: faults
// kill the process (libnx's fatal handler), they are not translated.
#include <rex/platform.h>
#include <rex/platform/seh.h>
#include <cstdlib>
#include <rex/platform/exceptions.h>

namespace rex::platform {
static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;
SehThreadState& seh_thread_state() { return tls_seh_state; }
int seh_filter(uint32_t, void*) { return 0; }
[[noreturn]] void seh_rethrow() { abort(); }
void seh_initialize() { g_seh_initialized.exchange(true); }
bool& seh_active() { return tls_seh_active; }
}  // namespace rex::platform
