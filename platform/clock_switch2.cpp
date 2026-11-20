// Host clock on Horizon. libounce's clock_gettime/getres know only
// CLOCK_MONOTONIC and CLOCK_REALTIME; the system counter is the native clock
// (cntpct_el0, 31.25 MHz).
#include <sys/time.h>

#include <switch.h>

#include <rex/chrono/clock.h>
#include <rex/platform.h>

namespace rex::chrono {

uint64_t Clock::host_tick_frequency_platform() { return armGetSystemTickFreq(); }
uint64_t Clock::host_tick_count_platform() { return armGetSystemTick(); }

uint64_t Clock::QueryHostSystemTime() {
  constexpr uint64_t seconds_per_day = 3600 * 24;
  constexpr uint64_t seconds_1601_to_1970 = ((369 * 365 + 89) * seconds_per_day);
  timeval now;
  gettimeofday(&now, nullptr);
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull + now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
