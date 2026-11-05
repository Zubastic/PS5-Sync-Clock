/*
 * PS5 Sync Clock - standalone payload entrypoint.
 *
 * Sets the PS5 system time (user wall clock + network/NP domains) from
 * a small web UI. See README.md.
 */

#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "api.h"
#include "log.h"
#include "restwatch.h"
#include "sntp.h"
#include "websrv.h"

#ifndef SYNC_CLOCK_VERSION
#define SYNC_CLOCK_VERSION "dev"
#endif

#ifndef SYNC_CLOCK_PORT
#define SYNC_CLOCK_PORT 5915
#endif

/* Show up as "sync-clock.elf" in the payload manager instead of the
 * default "payload.elf" (same trick as PS5-Trophy-Info). */
#define SYNC_CLOCK_PAYLOAD_NAME "sync-clock.elf"

static void set_thread_name(void) {
  syscall(SYS_thr_set_name, -1, SYNC_CLOCK_PAYLOAD_NAME);
}

int main() {
  set_thread_name();
  log_message("PS5 Sync Clock %s starting\n", SYNC_CLOCK_VERSION);
  if (!sntp_init_network()) {
    /* The SNTP path needs the network pool, but the web UI and the
     * browser-NTP / manual paths do not; keep running anyway. */
    log_message("network init failed, console SNTP will be unavailable\n");
  }
  notify_message("Sync Clock %s: web UI on port %d", SYNC_CLOCK_VERSION,
                 SYNC_CLOCK_PORT);

  /* Proactive rest-mode watcher (ShadowMountPlus style): it does NOT
   * touch the clock on wake — time recovery is sync-clock-auto.elf's job.
   * Its only task is to detect wake and ask the web server loop below
   * to recreate its listen socket (the old one usually dies with the
   * network stack). Failure to start it is not fatal: the recovery
   * loop + accept-error counter remain as a passive safety net. */
  rest_watcher_start(false /* socket recreate only, no clock writes */);

  /* Recovery loop, mirroring ftpsrv: when the listen socket dies
   * (rest-mode wake re-initializes the network stack), http_serve
   * returns and the whole server is recreated from scratch instead
   * of the payload turning into a zombie that never accepts again.
   * Both socket-loss (rc=2) and transient bind failures (rc=1 right
   * after wake, while the old port is still held) are retried; the
   * payload only exits through /api/quit (rc=0). */
  while (true) {
    const int rc = http_serve(SYNC_CLOCK_PORT, api_handle_request);
    if (rc == 0) {
      return 0; /* shutdown requested */
    }
    log_message("web UI socket lost (rc=%d), recreating in 3s\n", rc);
    sleep(3);
  }
}
