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
 * Parse a decimal unix-seconds value and apply it.
 * Returns true on success; on failure `error` (if non-null) receives a
 * short human-readable reason.
 */
bool clock_set_from_unix_seconds(long long seconds, const char** error);
