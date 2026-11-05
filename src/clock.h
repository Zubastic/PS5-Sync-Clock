/*
 * PS5 Sync Clock - PS5 clock domain access.
 *
 * The console exposes RTC-derived clock domains through clock_gettime /
 * clock_settime with Sony-specific clockids:
 *   16 - RTC domain 0, the user wall clock
 *   17 - RTC domain 1, network time (read by the NP / trophy stack)
 * Writing them requires the system authid; the payload temporarily
 * elevates its ucred, applies the time, and restores the original
 * credentials.
 */

#pragma once

#include <stdint.h>
#include <time.h>

/* Sony clockids (RTC domains), see kern_clock_gettime_ps5 in the kernel. */
#define PS5_CLOCK_REALTIME_USER 16 /* RTC domain 0: user wall clock  */
#define PS5_CLOCK_NETWORK 17       /* RTC domain 1: NP / network time */

/*
 * /dev/srtc ioctl commands (kernel cdev handler, group 'S'):
 *   0x40105305 - _IOR(S,5,16)  GET: fills srtc_query_t
 *   0x40105303 - _IOR(S,3,16)  GET (second command, req 3)
 *   0xC0105301 - _IOWR(S,1,16) SET: seconds since the SRTC epoch in
 *                 srtc_query_t::sec, result status lands in ::status
 * The SRTC counts seconds since 2012-01-01 UTC; the trophy code computes
 * its timestamp as sec * 1000000 + 63460972800000000 (microseconds since
 * 0001-01-01), which equals unix * 1000000 + 62135596800000000.
 */
#define SRTC_IOCTL_GET_SEC 0x40105305
#define SRTC_IOCTL_GET_2 0x40105303
#define SRTC_IOCTL_SET_SEC 0xC0105301
#define SRTC_EPOCH_UNIX 1325376000LL /* 2012-01-01 00:00:00 UTC */

/* Microseconds from 0001-01-01 to the unix epoch (RtcTick base). */
constexpr int64_t kRtcTickUnixBase = 62135596800000000LL;

/*
 * Exact 16-byte parameter block of the /dev/srtc ioctls, mirroring what
 * np_trophy does in SceShellCore (sub_1673000): a u64 seconds value and
 * a u32 status word at offset 0xC.
 */
typedef struct srtc_query {
  uint64_t sec;    /* seconds since 2012-01-01 UTC                  */
  uint32_t pad;    /* undefined                                     */
  uint32_t status; /* 0 = ok, else SAMU/kernel error (e.g. 800f0501) */
} srtc_query_t;

/* Snapshot of one /dev/srtc probe, used by the JSON state builder. */
typedef struct srtc_snapshot {
  int opened;      /* open("/dev/srtc") succeeded                   */
  int ioctl_ok;    /* ioctl(0x40105305) syscall succeeded           */
  int ioctl_errno; /* errno of a failed open/ioctl                  */
  uint32_t status; /* in-struct SAMU status (0 = trophy-usable)     */
  int64_t srtc_sec;
  int64_t unix_sec;      /* srtc epoch corrected to unix seconds    */
  uint64_t trophy_tick;  /* the exact unlock timestamp value        */
} srtc_snapshot_t;

/*
 * Probe /dev/srtc exactly like the trophy code does (open O_RDWR +
 * ioctl SRTC_IOCTL_GET_SEC). Never throws; failures are reported
 * through the snapshot fields.
 */
void srtc_probe(srtc_snapshot_t* out);

/*
 * Write the SRTC seconds counter through SRTC_IOCTL_SET_SEC so that
 * future trophy unlocks receive a real timestamp. Privileges are
 * elevated internally. Returns true when the ioctl succeeded and a
 * follow-up probe reports status 0.
 */
bool srtc_set_unix_seconds(long long seconds, const char** error);

/*
 * Read-only diagnostic: scan the kernel text for the srtc driver
 * globals and report every candidate with a dump of its softc fields
 * as JSON. Only kernel_copyout (fault-handled reads) is used, so this
 * is safe to call at any time and never panics the kernel.
 */
bool srtc_scan_report(char* json, size_t json_size);

/*
 * Explicitly opt-in kernel fix: locate the srtc softc, fill the empty
 * boot-time cache (softc+0x28) and verify through /dev/srtc. Performs
 * a single validated qword write; never runs automatically.
 */
bool srtc_kernel_fix_boot_cache(long long unix_sec, const char** error);

/*
 * One-shot kernel read diagnostic (see api.cpp /api/srtc/step): each
 * call performs a single 8-byte read at a known-mapped address,
 * reports the result as JSON and an on-screen notification. Advances
 * an internal step counter; returns the step just executed.
 */
int srtc_diag_step(char* json, size_t json_size);

/*
 * Dump the kernel .data region [DATA_BASE, ALLPROC + extra) to
 * /data/homebrew/kernel-11.20-data.bin (24-byte address header
 * first). Text is XOM and must NOT be read. Small chunks only.
 * Returns true on success.
 */
bool srtc_kdump_file(long long extra, const char** error);


/* Sanity bounds for accepted unix timestamps: 2000-01-01 .. 2100-01-01. */
constexpr int64_t kMinimumUnixTime = 946684800LL;
constexpr int64_t kMaximumUnixTime = 4102444800LL;

/*
 * Probe every supported clockid and build the JSON document used by the
 * UI table. Unreadable ids come back as null with their errno.
 * Writes at most json_size bytes including the NUL.
 */
void clock_build_state_json(char* json, size_t json_size);

/*
 * Apply a wall time to all writable domains (16, 17) and the plain
 * timeofday clock. Privileges are elevated internally.
 * Returns true when every write succeeded.
 */
bool clock_apply_time(const struct timespec* ts);

/*
 * Rest-mode resume handler (see restwatch.h): re-fills the srtc boot
 * cache and re-aligns the RTC domains from the console's own clock.
 * Returns true when /dev/srtc is valid afterwards.
 */
bool clock_on_resume_fix(void);

/*
 * Parse a decimal unix-seconds value and apply it.
 * Returns true on success; on failure `error` (if non-null) receives a
 * short human-readable reason.
 */
bool clock_set_from_unix_seconds(long long seconds, const char** error);
