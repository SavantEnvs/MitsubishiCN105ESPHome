// Deterministic monotonic clock for the CN105 harness. ESPHome's host HAL (esphome/components/host/
// hal.cpp: millis(), micros(), millis_64()) reads CLOCK_MONOTONIC; build.sh compiles only that file
// with -Dclock_gettime=cn105_fuzz_clock_gettime (and -Dnanosleep=...), so the component's timers and cycle logic advance
// with the fuzz input instead of the wall clock. Everything else (libFuzzer included) keeps the real
// clock_gettime.
#include <cstdint>
#include <ctime>

static uint64_t g_ms = 1000000;

extern "C" void cn105_fuzz_clock_set_ms(uint64_t ms) { g_ms = ms; }
extern "C" uint64_t cn105_fuzz_clock_get_ms(void) { return g_ms; }

extern "C" int cn105_fuzz_clock_gettime(clockid_t clk, struct timespec *ts) {
  (void) clk;
  ts->tv_sec = static_cast<time_t>(g_ms / 1000);
  ts->tv_nsec = static_cast<long>((g_ms % 1000) * 1000000);
  return 0;
}

// delay()/delayMicroseconds() in the same file sleep with nanosleep (-Dnanosleep=cn105_fuzz_nanosleep):
// advance the fake clock instead of sleeping (e.g. hp_writings.cpp's CUSTOM_DELAY(750) when the UART
// is not connected), so a delay costs no wall time but still moves the component's timeline.
extern "C" int cn105_fuzz_nanosleep(const struct timespec *req, struct timespec *rem) {
  g_ms += static_cast<uint64_t>(req->tv_sec) * 1000 + static_cast<uint64_t>(req->tv_nsec) / 1000000;
  if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
  return 0;
}
