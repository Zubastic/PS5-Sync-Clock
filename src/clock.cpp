/*
 * PS5 Sync Clock - PS5 clock domain access.
 */

#include "clock.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "log.h"

#ifndef SYS_clock_settime
/* Prospero numbering if the SDK header ever omits it. */
#define SYS_clock_settime 233
#endif
#ifndef SYS_clock_gettime
#define SYS_clock_gettime 232
#endif

/* System authid: grants the SAR access checked in the kernel for
 * clockid 16/17 (srtc_cred_has_write_access + sub_6092B0). */
constexpr uint64_t kSystemAuthId = 0x4800000000010003ULL;

namespace {

struct clock_name {
  int id;
  const char* name;
};

const clock_name kClockNames[] = {
    {0, "CLOCK_REALTIME (wall, settable)"},
    {1, "CLOCK_VIRTUAL (proc user cputime)"},
    {2, "CLOCK_PROF (proc cputime)"},
    {3, "CLOCK_UPTIME (?)"},
    {4, "CLOCK_MONOTONIC"},
    {5, "CLOCK_UPTIME_PRECISE"},
    {6, "CLOCK_UPTIME_FAST"},
    {7, "CLOCK_REALTIME_PRECISE"},
    {8, "CLOCK_REALTIME_FAST"},
    {9, "CLOCK_MONOTONIC_FAST (2nd realtime?)"},
    {10, "CLOCK_MONOTONIC_PRECISE"},
    {11, "CLOCK_MONOTONIC_RAW"},
    {12, "CLOCK_MONOTONIC_RAW_APPROX"},
    {13, "SBL boot tick (Sony)"},
    {15, "proc-lifetime cputime"},
    {16, "RTC domain 0 - USER wall clock"},
    {17, "RTC domain 1 - NETWORK (NP/trophy)"},
    {18, "SECURE time (SAMU/srtc)"},
    {19, "Sony: force domain 0"},
    {20, "EMC timer raw (Sony)"},
};

int ps5_clock_gettime(int clockid, struct timespec* ts) {
  return syscall(SYS_clock_gettime, clockid, ts);
}

int ps5_clock_settime(int clockid, const struct timespec* ts) {
  return syscall(SYS_clock_settime, clockid, ts);
}

bool set_domain_time(int clockid, const struct timespec* ts) {
  if (ps5_clock_settime(clockid, ts) != 0) {
    log_message("clock_settime(%d) failed: errno=%d\n", clockid, errno);
    return false;
  }
  struct timespec check = {};
  if (ps5_clock_gettime(clockid, &check) == 0) {
    log_message("domain %d set: %lld.%09ld\n", clockid,
                static_cast<long long>(check.tv_sec),
                static_cast<long>(check.tv_nsec));
  }
  return true;
}

bool elevate(uint64_t* saved_authid) {
  *saved_authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), kSystemAuthId) != 0) {
    log_message("kernel_set_ucred_authid failed\n");
    return false;
  }
  uint8_t caps[16];
  memset(caps, 0xff, sizeof(caps));
  if (kernel_set_ucred_caps(getpid(), caps) != 0) {
    log_message("kernel_set_ucred_caps failed\n");
    kernel_set_ucred_authid(getpid(), *saved_authid);
    return false;
  }
  return true;
}

void restore(uint64_t saved_authid) {
  if (kernel_set_ucred_authid(getpid(), saved_authid) != 0) {
    log_message("failed to restore authid!\n");
  }
}

}  // namespace

void clock_build_state_json(char* json, size_t json_size) {
  char buf[4096];
  size_t len = static_cast<size_t>(
      snprintf(buf, sizeof(buf), "{"));

  for (int id = 0; id <= 20; ++id) {
    struct timespec ts = {};
    if (ps5_clock_gettime(id, &ts) == 0) {
      len += static_cast<size_t>(snprintf(
          buf + len, sizeof(buf) - len, "%s\"%d\":%lld",
          id > 0 ? "," : "", id, static_cast<long long>(ts.tv_sec)));
    } else {
      len += static_cast<size_t>(snprintf(
          buf + len, sizeof(buf) - len, "%s\"%d\":null,\"e%d\":%d",
          id > 0 ? "," : "", id, id, errno));
    }
  }

  struct timeval tv = {};
  if (gettimeofday(&tv, nullptr) == 0) {
    len += static_cast<size_t>(snprintf(buf + len, sizeof(buf) - len,
                                        ",\"realtime\":%lld",
                                        static_cast<long long>(tv.tv_sec)));
  }
  snprintf(buf + len, sizeof(buf) - len, "}");
  snprintf(json, json_size, "%s", buf);
}

bool clock_apply_time(const struct timespec* ts) {
  uint64_t saved_authid = 0;
  if (!elevate(&saved_authid)) {
    return false;
  }
  bool ok = true;
  ok = set_domain_time(PS5_CLOCK_NETWORK, ts) && ok;
  ok = set_domain_time(PS5_CLOCK_REALTIME_USER, ts) && ok;

  struct timeval tv = {};
  tv.tv_sec = ts->tv_sec;
  tv.tv_usec = ts->tv_nsec / 1000;
  if (settimeofday(&tv, nullptr) != 0) {
    log_message("settimeofday failed: errno=%d\n", errno);
    ok = false;
  }
  restore(saved_authid);
  return ok;
}

bool clock_set_from_unix_seconds(long long seconds, const char** error) {
  if (seconds < kMinimumUnixTime || seconds > kMaximumUnixTime) {
    if (error) {
      *error = "invalid unix seconds";
    }
    return false;
  }

  struct timespec ts = {};
  ts.tv_sec = static_cast<time_t>(seconds);
  ts.tv_nsec = 0;
  if (!clock_apply_time(&ts)) {
    if (error) {
      *error = "privilege escalation or clock write failed";
    }
    return false;
  }
  return true;
}
