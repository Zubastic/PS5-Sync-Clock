/*
 * PS5 Sync Clock - standalone payload entrypoint.
 *
 * Sets the PS5 system time (user wall clock + network/NP domains) from
 * a small web UI. See README.md.
 */

#include <stdio.h>

#include "api.h"
#include "log.h"
#include "sntp.h"
#include "websrv.h"

#ifndef SYNC_CLOCK_VERSION
#define SYNC_CLOCK_VERSION "dev"
#endif

#ifndef SYNC_CLOCK_PORT
#define SYNC_CLOCK_PORT 5915
#endif

int main() {
  log_message("PS5 Sync Clock %s starting\n", SYNC_CLOCK_VERSION);
  if (!sntp_init_network()) {
    /* The SNTP path needs the network pool, but the web UI and the
     * browser-NTP / manual paths do not; keep running anyway. */
    log_message("network init failed, console SNTP will be unavailable\n");
  }
  notify_message("Sync Clock %s: web UI on port %d", SYNC_CLOCK_VERSION,
                 SYNC_CLOCK_PORT);
  return http_serve(SYNC_CLOCK_PORT, api_handle_request);
}
