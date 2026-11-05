/*
 * PS5 Sync Clock - sync-clock-auto: resident time-recovery daemon.
 *
 * Stays in the background and watches the SceSystemStateMgrInfo event
 * flag (restwatch, fix_on_wake = true). After every wake from rest
 * mode it runs the same recovery as the web-UI buttons
 * (clock_on_resume_fix):
 *   - fills the srtc boot cache (softc+0x28) from the console's local
 *     clock when /dev/srtc is broken (kernel write only if needed),
 *   - re-aligns the drifting RTC domains 16/17 with the wall clock.
 * No web server. Add it to the PLK autoload list once and it keeps
 * guarding the clock for the whole session.
 *
 * Diagnostics: the payload manager does not show stdout, so the log
 * is appended to /data/homebrew/sync-clock-auto.log.
 */

#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "clock.h"
#include "log.h"
#include "restwatch.h"

#ifndef SYNC_CLOCK_VERSION
#define SYNC_CLOCK_VERSION "dev"
#endif

/* Show up as "sync-clock-auto.elf" in the payload manager instead of
 * the default "payload.elf" (same trick as PS5-Trophy-Info). */
#define SYNC_CLOCK_AUTO_PAYLOAD_NAME "sync-clock-auto.elf"
#define AUTO_LOG_PATH "/data/homebrew/sync-clock-auto.log"

int main() {
  syscall(SYS_thr_set_name, -1, SYNC_CLOCK_AUTO_PAYLOAD_NAME);

  /* PLK does not show stdout: tee the whole log to a file (append, so
   * every wake cycle stays visible). */
  if (!freopen(AUTO_LOG_PATH, "a", stdout)) {
    log_message("cannot open %s, logging to stdout only\n", AUTO_LOG_PATH);
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  log_message("sync-clock-auto %s starting (resident)\n",
              SYNC_CLOCK_VERSION);

  /* Initial probe: what the trophy stack sees right now. */
  srtc_snapshot_t before = {};
  srtc_probe(&before);
  log_message("before: opened=%d ioctl=%d status=%#x unix=%lld\n",
              before.opened, before.ioctl_ok, before.status,
              static_cast<long long>(before.unix_sec));

  /* Fix once at startup (covers the boot case and post-boot wake from
   * a previous session), then keep watching for further wake cycles. */
  if (clock_on_resume_fix()) {
    notify_message("sync-clock-auto: time OK");
  } else {
    notify_message("sync-clock-auto: initial fix FAILED, see log");
  }

  /* Resident loop: the watcher thread handles every future wake. */
  if (!rest_watcher_start(true /* fix on wake */)) {
    log_message("restwatch unavailable, exiting\n");
    return 1;
  }

  while (true) {
    sleep(3600);
  }
  return 0;
}
