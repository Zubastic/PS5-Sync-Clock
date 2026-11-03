/*
 * PS5 Sync Clock - console-side SNTP client.
 *
 * Queries public NTP servers over UDP/123 (based on ps5-date-time-sync
 * by Deckerr97), validates the response, and applies the half-RTT
 * delay correction. Requires sceNet to be initialized once via
 * sntp_init_network().
 */

#pragma once

#include <time.h>

/*
 * Initialize sceNet and create the network pool used by the SNTP
 * client. Safe to call once at startup; returns false on failure.
 */
bool sntp_init_network(void);

/*
 * Query the NTP server pool and store the corrected time in `output`.
 * Returns true when a valid response was received and applied checks
 * passed.
 */
bool sntp_fetch_time(struct timespec* output);
