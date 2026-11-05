/*
 * PS5 Sync Clock - PS5 clock domain access.
 */

#include "clock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "log.h"

/* KERNEL_ADDRESS_TEXT_BASE / KERNEL_ADDRESS_DATA_BASE are declared as
 * `extern const intptr_t` in <ps5/kernel.h> and resolved by the SDK crt
 * at startup from the payload arguments and the firmware version. */

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

/* Authid for /dev/srtc ioctls: the kernel gate (0x9e9f8ff0 in the
 * 13.60 dump) requires (authid & 0xFF0F000000000000) == 0x4801000000000000,
 * i.e. an 0x4801xxxxxxxxxxxx value; the plain system authid above FAILS
 * it (0x4800...) and the ioctl reports 0x800f0501. 0x4801000000000013
 * is the value observed passing the same check on live systems. */
constexpr uint64_t kSrtcAuthId = 0x4801000000000013ULL;

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

bool elevate(uint64_t authid, uint64_t* saved_authid) {
  *saved_authid = kernel_get_ucred_authid(getpid());
  if (kernel_set_ucred_authid(getpid(), authid) != 0) {
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

/* ---------- kernel boot-time cache fallback ----------
 *
 * The SAM service (8002100D) refuses direct SRTC commands on this
 * console, so /dev/srtc reads fail and trophies are stored with
 * ts = 0. The kernel read helper (srtc_get_boot_time_nvs, 0x4fdc30 in
 * the 13.60 dump) short-circuits that service whenever the boot SRTC
 * value cached at srtc_softc+0x28 is non-zero: it simply returns
 *     cache + uptime
 * Writing that field through the SDK kernel primitives makes every
 * future /dev/srtc GET - and therefore every trophy timestamp -
 * valid, no SAMU involved. */

/*
 * Per-firmware byte-exact offset of the srtc_softc pointer global,
 * relative to KERNEL_ADDRESS_DATA_BASE, is hardcoded in
 * srtc_resolve_softc below. Derived offline by matching the unique
 * SAMU parameter block (0x0100000100000000 / ...
 * 0x0000000500000000 / 0x0000000100000002 ...) and the surrounding
 * structure against reference kernel dumps:
 *   11.20: SAMU block at data-rel 0x2850960, block - softc = 0x950
 *          (same delta as 13.60); the neighborhood (zero run, heap
 *          pointer pair at G-8/G, 593 identical qwords vs 13.60)
 *          confirms data-rel 0x2850010.
 * NEVER scan kernel text: it is XOM under the hypervisor and any
 * kernel_copyout of it panics the kernel. Data reads/writes only.
 */

bool srtc_kread64(uint64_t kaddr, uint64_t* out) {
  return kernel_copyout(static_cast<intptr_t>(kaddr), out, sizeof(*out)) == 0;
}

bool srtc_kwrite64(uint64_t kaddr, uint64_t val) {
  return kernel_copyin(&val, static_cast<intptr_t>(kaddr), sizeof(val)) == 0;
}

/*
 * Resolve the srtc_softc pointer using the per-firmware offset table.
 * Strictly read-only: one 8-byte copyout from .data, then validation
 * copyouts from the pointed-to heap structure. If any validation
 * fails, nothing else is touched (no text access anywhere).
 */
bool srtc_resolve_softc(uint64_t* global_out, uint64_t* softc_out,
                        const char** error) {
  const uint64_t data = static_cast<uint64_t>(KERNEL_ADDRESS_DATA_BASE);
  const uint64_t allproc = static_cast<uint64_t>(KERNEL_ADDRESS_ALLPROC);
  if (!data || !allproc || allproc <= data) {
    if (error) {
      *error = "kernel data base unknown";
    }
    return false;
  }

  /* Single supported firmware for now: 11.20. On any other firmware
   * the pointer at the 11.20 offset will fail validation below, so
   * this stays panic-safe either way. */
  const uint64_t global = data + 0x2850010;
  if (global >= allproc) {
    if (error) {
      *error = "offset beyond mapped data";
    }
    return false;
  }

  uint64_t softc = 0;
  if (!srtc_kread64(global, &softc)) {
    if (error) {
      *error = "read of srtc_softc global failed";
    }
    return false;
  }

  /* Heap pointer sanity (mirrors kernel_is_heap_addr): kernel heap
   * pointers look like 0xffff926d_xxxxxxxx. */
  if ((softc & 7) || (softc >> 48) != 0xffff) {
    if (error) {
      *error = "global does not hold a heap pointer";
    }
    return false;
  }
  const uint64_t hi = (softc >> 32) & 0xffff;
  if (hi == 0 || hi == 0xffff) {
    if (error) {
      *error = "global pointer not kernel-heap";
    }
    return false;
  }

  /* NOTE: no lock-field check here. In the 13.60 dump softc+8 (the
   * sx lock) reads as 0 while unlocked, so requiring it non-zero
   * would reject a perfectly valid softc. The heap-pointer sanity
   * above is sufficient and panic-safe. */

  if (global_out) {
    *global_out = global;
  }
  if (softc_out) {
    *softc_out = softc;
  }
  return true;
}

/*
 * Read-only JSON report of the resolved srtc softc, including a dump
 * of the first 0x40 bytes of the structure. No kernel write.
 */
bool srtc_scan_report_impl(char* json, size_t json_size) {
  uint64_t global = 0;
  uint64_t softc = 0;
  const char* err = nullptr;
  const bool ok = srtc_resolve_softc(&global, &softc, &err);

  size_t len = static_cast<size_t>(
      snprintf(json, json_size, "{\"count\":%d,\"error\":\"%s\"",
               ok ? 1 : 0, ok ? "" : (err ? err : "unknown")));
  if (ok && len + 512 < json_size) {
    uint64_t q[8] = {};
    for (int f = 0; f < 8; f++) {
      srtc_kread64(softc + static_cast<uint64_t>(f) * 8, &q[f]);
    }
    len += static_cast<size_t>(snprintf(
        json + len, json_size - len,
        ",\"global\":\"%#llx\",\"softc\":\"%#llx\","
        "\"q00\":\"%#llx\",\"q08\":\"%#llx\",\"q10\":\"%#llx\","
        "\"q18\":\"%#llx\",\"q20\":\"%#llx\",\"q28\":\"%#llx\","
        "\"q30\":\"%#llx\",\"q38\":\"%#llx\"",
        (unsigned long long)global, (unsigned long long)softc,
        (unsigned long long)q[0], (unsigned long long)q[1],
        (unsigned long long)q[2], (unsigned long long)q[3],
        (unsigned long long)q[4], (unsigned long long)q[5],
        (unsigned long long)q[6], (unsigned long long)q[7]));
  }
  snprintf(json + len, json_size - len, "}");
  return ok;
}

/*
 * Fill the boot SRTC cache so /dev/srtc reads (trophy timestamps)
 * return a valid time without the SAM service. Only an empty (zero)
 * cache field is used; a failed verification rolls the write back.
 * Returns true when a follow-up probe reports status 0.
 */
bool srtc_fix_boot_cache(long long unix_sec, const char** error) {
  uint64_t global = 0;
  uint64_t softc = 0;
  if (!srtc_resolve_softc(&global, &softc, error)) {
    return false;
  }

  struct timespec up = {};
  clock_gettime(5 /* CLOCK_UPTIME_PRECISE */, &up);

  /* Always use the console's own local wall clock (CLOCK_REALTIME)
   * as the time source — never a host-provided/NTP value passed in
   * unix_sec. The boot time = now(2012-based) − uptime. */
  struct timespec now = {};
  clock_gettime(CLOCK_REALTIME, &now);
  const long long local_unix = static_cast<long long>(now.tv_sec);
  (void)unix_sec;

  /* softc+0x28 is the boot seconds cache field (layout verified in
   * the 13.60 dump and structurally identical on 11.20); writing the
   * wrong field would be riskier than failing, so no fallback
   * offsets are tried. */
  const uint64_t field = softc + 0x28;
  uint64_t orig = 0;
  if (!srtc_kread64(field, &orig)) {
    if (error) {
      *error = "cannot read boot cache field";
    }
    return false;
  }
  if (orig != 0) {
    if (error) {
      *error = "boot cache already non-zero";
    }
    return false;
  }

  long long boot = (local_unix - SRTC_EPOCH_UNIX) -
                   static_cast<long long>(up.tv_sec);
  if (!srtc_kwrite64(field, static_cast<uint64_t>(boot))) {
    if (error) {
      *error = "kernel write of boot cache failed";
    }
    return false;
  }

  srtc_snapshot_t chk = {};
  srtc_probe(&chk);
  if (chk.ioctl_ok && chk.status == 0) {
    /* Correct any residual drift once, judged against the console's
     * own local clock at the moment of the probe. */
    struct timespec now2 = {};
    clock_gettime(CLOCK_REALTIME, &now2);
    const long long expect =
        static_cast<long long>(now2.tv_sec) - SRTC_EPOCH_UNIX;
    const long long delta = chk.srtc_sec - expect;
    if (delta != 0) {
      boot -= delta;
      srtc_kwrite64(field, static_cast<uint64_t>(boot));
      srtc_probe(&chk);
    }
    if (chk.ioctl_ok && chk.status == 0 &&
        chk.unix_sec > kMinimumUnixTime) {
      log_message("srtc: boot cache fixed (softc+%#x = %lld, now %lld)\n",
                  0x28, boot, static_cast<long long>(chk.unix_sec));
      return true;
    }
  }
  srtc_kwrite64(field, orig); /* roll back */

  log_message("srtc: boot cache fix failed\n");
  if (error) {
    *error = "boot cache fix failed";
  }
  return false;
}

}  // namespace

/*
 * Public read-only scan report (see clock.h).
 */
bool srtc_scan_report(char* json, size_t json_size) {
  return srtc_scan_report_impl(json, json_size);
}

/*
 * Step-by-step kernel read diagnostic: each call performs exactly ONE
 * 8-byte copyout at a known-mapped address and reports the step in
 * JSON and as an on-screen notification. If the kernel panics, the
 * last notification identifies the failing read.
 */
static int srtc_diag_stage = 0;
int srtc_diag_step(char* json, size_t json_size) {
  const int stage = srtc_diag_stage;
  struct addr_name {
    const char* name;
    intptr_t addr;
  };
  const addr_name steps[] = {
      {"allproc", KERNEL_ADDRESS_ALLPROC},
      {"text", KERNEL_ADDRESS_TEXT_BASE},
      {"data", KERNEL_ADDRESS_DATA_BASE},
  };
  const int nsteps = static_cast<int>(sizeof(steps) / sizeof(steps[0]));

  if (stage >= nsteps) {
    snprintf(json, json_size,
             "{\"stage\":%d,\"done\":true,"
             "\"hint\":\"all single reads ok, run /api/srtc/scan\"}",
             stage);
    return stage;
  }

  const uint64_t addr = static_cast<uint64_t>(steps[stage].addr);
  notify_message("srtc diag step %d: read %s at %#llx", stage,
                 steps[stage].name, (unsigned long long)addr);

  uint64_t value = 0;
  const int rc = kernel_copyout(static_cast<intptr_t>(addr), &value,
                                sizeof(value));
  notify_message("srtc diag step %d: rc=%d value=%#llx", stage, rc,
                 (unsigned long long)value);

  snprintf(json, json_size,
           "{\"stage\":%d,\"name\":\"%s\",\"addr\":\"%#llx\","
           "\"rc\":%d,\"value\":\"%#llx\"}",
           stage, steps[stage].name, (unsigned long long)addr, rc,
           (unsigned long long)value);
  srtc_diag_stage = stage + 1;
  return stage;
}

/*
 * Dump the kernel .data region [DATA_BASE, ALLPROC + extra) to a file
 * on the console: /data/homebrew/kernel-11.20-data.bin. The text
 * segment is deliberately NOT read: it is XOM (execute-only) under
 * the hypervisor and any read of it panics the kernel; the data
 * segment, however, is freely readable (the SDK credential helpers
 * already prove that). A 24-byte header (text, data, allproc as
 * little-endian u64) is written first so the dump can be loaded at
 * the right base in IDA.
 */
bool srtc_kdump_file(long long extra, const char** error) {
  const uint64_t text = static_cast<uint64_t>(KERNEL_ADDRESS_TEXT_BASE);
  const uint64_t data = static_cast<uint64_t>(KERNEL_ADDRESS_DATA_BASE);
  const uint64_t allproc = static_cast<uint64_t>(KERNEL_ADDRESS_ALLPROC);
  if (!text || !data || !allproc || data <= text || allproc <= data) {
    if (error) {
      *error = "kernel base addresses invalid";
    }
    return false;
  }
  if (extra < 0 || extra > 0x400000) {
    if (error) {
      *error = "extra out of range";
    }
    return false;
  }

  mkdir("/data", 0777);
  mkdir("/data/homebrew", 0777);

  const char* path = "/data/homebrew/kernel-11.20-data.bin";
  FILE* out = fopen(path, "wb");
  if (!out) {
    log_message("kdump: open %s failed: errno=%d\n", path, errno);
    if (error) {
      *error = "cannot open output file in /data/homebrew";
    }
    return false;
  }

  uint64_t header[3] = {text, data, allproc};
  bool ok = fwrite(header, sizeof(header), 1, out) == 1;

  /* Small chunks only: the pipe-based copyout path has a limited
   * kernel pipe buffer (proven dumpers use 0x4000). */
  static uint8_t buf[0x4000];
  uint64_t written = 0;
  const uint64_t total = (allproc - data) + static_cast<uint64_t>(extra);
  for (uint64_t off = 0; ok && off < total; off += sizeof(buf)) {
    uint64_t chunk = sizeof(buf);
    if (off + chunk > total) {
      chunk = total - off;
    }
    if (kernel_copyout(static_cast<intptr_t>(data + off), buf,
                       static_cast<size_t>(chunk)) != 0) {
      log_message("kdump: copyout failed at %#llx\n",
                  (unsigned long long)(data + off));
      ok = false;
      break;
    }
    if (fwrite(buf, 1, static_cast<size_t>(chunk), out) != chunk) {
      log_message("kdump: write failed at %#llx (errno=%d)\n",
                  (unsigned long long)(data + off), errno);
      ok = false;
      break;
    }
    written += chunk;
    if ((written & 0xFFFFFF) < sizeof(buf)) {
      notify_message("kdump: %llu MB / %llu",
                     (unsigned long long)(written >> 20),
                     (unsigned long long)(total >> 20));
    }
  }

  if (fclose(out) != 0) {
    ok = false;
  }
  if (ok) {
    log_message("kdump: done, %llu bytes -> %s\n",
                (unsigned long long)written, path);
    notify_message("kdump done: %s", path);
  } else {
    if (error) {
      *error = "dump failed mid-way (see log)";
    }
    notify_message("kdump FAILED");
  }
  return ok;
}

/* ---------- /dev/srtc (SAMU secure RTC) ---------- */

/*
 * Replicates SceShellCore sub_1673000: open("/dev/srtc", O_RDWR) and
 * ioctl 0x40105305. The trophy unlock timestamp is derived from this
 * call alone (sec * 1e6 + 63460972800000000 us); when it fails the
 * trophy file receives ts = 0 forever.
 */
void srtc_probe(srtc_snapshot_t* out) {
  memset(out, 0, sizeof(*out));
  out->opened = 0;
  out->ioctl_ok = 0;
  out->ioctl_errno = 0;
  out->status = 0xFFFFFFFFu;

  /* The ioctl handlers check the caller ucred (cap bit 62 for the
   * time read, authid 0x4801... for the other commands), so probe
   * with elevated credentials exactly like the sync path. */
  uint64_t saved_authid = 0;
  const bool elevated = elevate(kSrtcAuthId, &saved_authid);

  const int fd = open("/dev/srtc", O_RDWR);
  if (fd < 0) {
    out->ioctl_errno = errno;
    if (elevated) {
      restore(saved_authid);
    }
    return;
  }
  out->opened = 1;

  srtc_query_t q = {};
  if (ioctl(fd, SRTC_IOCTL_GET_SEC, &q) != 0) {
    out->ioctl_errno = errno;
    close(fd);
    if (elevated) {
      restore(saved_authid);
    }
    return;
  }
  close(fd);
  if (elevated) {
    restore(saved_authid);
  }

  out->ioctl_ok = 1;
  out->status = q.status;
  out->srtc_sec = static_cast<int64_t>(q.sec);
  out->unix_sec = out->srtc_sec + SRTC_EPOCH_UNIX;
  out->trophy_tick = static_cast<uint64_t>(out->unix_sec) * 1000000ULL +
                     static_cast<uint64_t>(kRtcTickUnixBase);
  close(fd);
}

bool srtc_set_unix_seconds(long long seconds, const char** error) {
  if (seconds < kMinimumUnixTime || seconds > kMaximumUnixTime) {
    if (error) {
      *error = "invalid unix seconds";
    }
    return false;
  }

  uint64_t saved_authid = 0;
  if (!elevate(kSrtcAuthId, &saved_authid)) {
    if (error) {
      *error = "privilege escalation failed";
    }
    return false;
  }

  const int fd = open("/dev/srtc", O_RDWR);
  if (fd < 0) {
    log_message("srtc open failed: errno=%d\n", errno);
    restore(saved_authid);
    if (error) {
      *error = "cannot open /dev/srtc";
    }
    return false;
  }

  srtc_query_t q = {};
  q.sec = static_cast<uint64_t>(seconds - SRTC_EPOCH_UNIX);
  int rc = ioctl(fd, SRTC_IOCTL_SET_SEC, &q);
  if (rc != 0) {
    log_message("srtc set ioctl failed: rc=%d errno=%d status=%#x\n", rc,
                errno, q.status);
  } else if (q.status != 0) {
    log_message("srtc set rejected by SAMU: status=%#x\n", q.status);
    rc = -1;
  }
  close(fd);
  restore(saved_authid);

  if (rc != 0) {
    /*
     * The SAM service refuses direct SRTC writes (observed status
     * 0x800f0532). The kernel boot-cache fallback is NOT performed
     * automatically: it involves a direct kernel write, so it only
     * runs when explicitly requested through /api/srtc/fix (see
     * srtc_kernel_fix_boot_cache).
     */
    if (error) {
      *error = "SRTC write rejected by SAMU (use /api/srtc/fix)";
    }
    return false;
  }

  /* Verify with the same read the trophy code performs. */
  srtc_snapshot_t check = {};
  srtc_probe(&check);
  log_message("srtc set ok: verify sec=%lld status=%#x\n",
              static_cast<long long>(check.srtc_sec), check.status);
  return check.ioctl_ok && check.status == 0;
}

/*
 * Explicit opt-in kernel fix (never called automatically): after
 * reviewing /api/srtc/scan output, apply the boot-cache write for the
 * given unix time.
 */
bool srtc_kernel_fix_boot_cache(long long unix_sec, const char** error) {
  if (unix_sec < kMinimumUnixTime || unix_sec > kMaximumUnixTime) {
    if (error) {
      *error = "invalid unix seconds";
    }
    return false;
  }
  return srtc_fix_boot_cache(unix_sec, error);
}

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

  /* What the trophy stack sees through /dev/srtc. */
  srtc_snapshot_t srtc = {};
  srtc_probe(&srtc);
  len += static_cast<size_t>(snprintf(
      buf + len, sizeof(buf) - len,
      ",\"srtc_opened\":%d,\"srtc_ioctl\":%d,\"srtc_errno\":%d,"
      "\"srtc_status\":%u,\"srtc_sec\":%lld,\"srtc_unix\":%lld",
      srtc.opened, srtc.ioctl_ok, srtc.ioctl_errno, srtc.status,
      static_cast<long long>(srtc.srtc_sec),
      static_cast<long long>(srtc.unix_sec)));
  if (srtc.ioctl_ok && srtc.status == 0) {
    len += static_cast<size_t>(snprintf(
        buf + len, sizeof(buf) - len, ",\"srtc_tick\":%llu",
        static_cast<unsigned long long>(srtc.trophy_tick)));
  }

  /* Copy the state object, then append the kernel base addresses as
   * computed by the SDK crt (userland globals — safe to read, no
   * kernel access). Used to diagnose wrong firmware detection without
   * risking a panic. */
  snprintf(json, json_size, "%s", buf);
  size_t jlen = strlen(json);
  snprintf(json + jlen, json_size - jlen,
           ",\"text_base\":\"%#llx\",\"data_base\":\"%#llx\","
           "\"allproc\":\"%#llx\"}",
           (unsigned long long)(intptr_t)KERNEL_ADDRESS_TEXT_BASE,
           (unsigned long long)(intptr_t)KERNEL_ADDRESS_DATA_BASE,
           (unsigned long long)(intptr_t)KERNEL_ADDRESS_ALLPROC);
}

bool clock_apply_time(const struct timespec* ts) {
  uint64_t saved_authid = 0;
  if (!elevate(kSystemAuthId, &saved_authid)) {
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

  /*
   * Fix the secure RTC as well: trophy unlock timestamps are derived
   * exclusively from /dev/srtc (SceShellCore sub_1673000). Without this
   * write every new trophy is stored with ts = 0. A failure here must
   * not invalidate the whole sync, so it is logged and reported
   * separately.
   */
  const char* srtc_error = nullptr;
  if (!srtc_set_unix_seconds(static_cast<long long>(ts->tv_sec),
                             &srtc_error)) {
    log_message("srtc fix failed: %s\n",
                srtc_error ? srtc_error : "unknown error");
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

/*
 * Rest-mode resume handler (called by the watcher in restwatch.cpp):
 * the kernel zeroes the srtc boot cache on resume (observed live:
 * srtc_status 0x800F0532 right after wake), and the RTC domains 16/17
 * keep drifting against the wall clock. Fully offline: everything is
 * re-aligned from the console's own CLOCK_REALTIME. A failed srtc fix
 * (e.g. cache already filled by the kernel) is not an error as long
 * as /dev/srtc answers with status 0.
 */
bool clock_on_resume_fix(void) {
  bool ok = true;

  /* 1. srtc boot cache: only written when it is actually broken. */
  srtc_snapshot_t pre = {};
  srtc_probe(&pre);
  const bool srtc_ok = pre.opened && pre.ioctl_ok &&
                       pre.status == 0 && pre.unix_sec > kMinimumUnixTime;
  if (!srtc_ok) {
    /* Boot the fix from the console's own wall clock (CLOCK_REALTIME):
     * the SRTC cache semantics mean the uptime part is added back on
     * every read, so one validated write at the current time is all
     * that is needed. */
    struct timespec boot_ts = {};
    clock_gettime(CLOCK_REALTIME, &boot_ts);
    const char* err = nullptr;
    if (!srtc_kernel_fix_boot_cache(
            static_cast<long long>(boot_ts.tv_sec), &err)) {
      log_message("resume: srtc fix failed: %s\n",
                  err ? err : "unknown");
      ok = false;
    }
  }

  /* 2. Re-align the drifting RTC domains 16/17 with the wall clock. */
  struct timespec now = {};
  clock_gettime(CLOCK_REALTIME, &now);
  uint64_t saved_authid = 0;
  if (elevate(kSystemAuthId, &saved_authid)) {
    ok = set_domain_time(PS5_CLOCK_NETWORK, &now) && ok;
    ok = set_domain_time(PS5_CLOCK_REALTIME_USER, &now) && ok;
    restore(saved_authid);
  } else {
    log_message("resume: cannot elevate for RTC domains\n");
    ok = false;
  }

  /* 3. Final verification. */
  srtc_snapshot_t post = {};
  srtc_probe(&post);
  log_message("resume: srtc status=%u unix=%lld ok=%d\n", post.status,
              static_cast<long long>(post.unix_sec), ok);
  return ok;
}
