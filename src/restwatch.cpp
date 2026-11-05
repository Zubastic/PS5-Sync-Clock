/*
 * PS5 Sync Clock - rest mode (suspend/resume) watcher.
 *
 * Polls the SceSystemStateMgrInfo event flag exactly like
 * ShadowMountPlus does (sceKernelOpenEventFlag + sceKernelPollEventFlag,
 * resolved through the libkernel stubs of the payload SDK). The low 16
 * bits of the flag value carry the system state:
 *   100 SHUTDOWN_ON_GOING, 200 POWER_SAVING, 300 SUSPEND_ON_GOING,
 *   500 MAIN_ON_STANDBY, 1000 WORKING.
 *
 * Two modes (restwatch.h):
 *  - fix_on_wake = false (sync-clock.elf): only websrv_request_restart
 *    on wake, no clock writes;
 *  - fix_on_wake = true (sync-clock-auto.elf): clock_on_resume_fix on
 *    every wake; the thread lives for the whole payload lifetime.
 */

#include "restwatch.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "log.h"
#include "websrv.h"

/* libkernel stubs (ps5-payload-sdk sce_stubs/libkernel.c); signatures
 * exactly as used by ShadowMountPlus src/sm_shellcore_flags.c.
 * The stubs have C linkage, so declare them extern "C" or the linker
 * looks for the mangled C++ names and fails. */
extern "C" {
typedef intptr_t sm_kernel_event_flag_t;

int sceKernelOpenEventFlag(sm_kernel_event_flag_t* ef, const char* name);
int sceKernelPollEventFlag(sm_kernel_event_flag_t ef,
                           unsigned long long bit_pattern,
                           unsigned int wait_mode,
                           unsigned long long* result_pattern);
int sceKernelCloseEventFlag(sm_kernel_event_flag_t ef);
}

/* SceSystemStateMgrInfo states (low 16 bits). */
#define SM_STATE_SUSPEND_ON_GOING 300
#define SM_STATE_MAIN_ON_STANDBY 500
#define SM_STATE_WORKING 1000

/* EVENT_FLAG_WAITMODE_OR: return as soon as any pattern bit is set. */
#define EVF_WAITMODE_OR 2u
#define EVF_ALL_BITS 0xFFFFFFFFFFFFFFFFull
/* EBUSY: flag value is zero right now (nothing latched) - fine. */
#define EVF_ERROR_EBUSY 0x80020010u

/* Grace period after resume before touching kernel state. */
#define RESUME_GRACE_SEC 10

#define POLL_AWAKE_SEC 1
#define POLL_ASLEEP_SEC 5

namespace {

bool g_fix_on_wake = false;

void* watcher_thread(void*) {
  sm_kernel_event_flag_t ef = -1;
  int rc = sceKernelOpenEventFlag(&ef, "SceSystemStateMgrInfo");
  if (rc < 0) {
    log_message("restwatch: cannot open SceSystemStateMgrInfo "
                "(rc=%#x), passive recovery only\n",
                static_cast<unsigned>(rc));
    return nullptr;
  }
  log_message("restwatch: watching SceSystemStateMgrInfo "
              "(fix_on_wake=%d)\n",
              g_fix_on_wake ? 1 : 0);

  bool asleep = false;
  while (true) {
    unsigned long long value = 0;
    rc = sceKernelPollEventFlag(ef, EVF_ALL_BITS, EVF_WAITMODE_OR, &value);
    int state = -1;
    if (rc == 0) {
      state = static_cast<int>(value & 0xFFFF);
    } else if (rc == static_cast<int>(EVF_ERROR_EBUSY)) {
      state = 0; /* flag is empty: no state change latched */
    }

    if (!asleep) {
      if (state == SM_STATE_SUSPEND_ON_GOING ||
          state == SM_STATE_MAIN_ON_STANDBY) {
        log_message("restwatch: entering rest mode (state %d)\n", state);
        asleep = true;
      }
      sleep(POLL_AWAKE_SEC);
      continue;
    }

    /* Asleep: WORKING means the console came back. The flag value
     * holds the LAST latched state, so poll errors are ignored here
     * and only an explicit WORKING transitions us out. */
    if (state != SM_STATE_WORKING) {
      sleep(POLL_ASLEEP_SEC);
      continue;
    }

    sleep(RESUME_GRACE_SEC);

    if (g_fix_on_wake) {
      log_message("restwatch: console awake, restoring time\n");
      if (!clock_on_resume_fix()) {
        log_message("restwatch: resume fix failed\n");
        notify_message("sync-clock-auto: fix FAILED after wake");
      } else {
        notify_message("sync-clock-auto: time restored after wake");
      }
    } else {
      log_message("restwatch: console awake (state %d), restarting web "
                  "server socket\n", state);
      /* The listen socket usually dies with the network stack. No
       * automatic time fix in this mode: that is sync-clock-auto.elf's
       * job. */
      websrv_request_restart();
    }

    asleep = false;
  }

  sceKernelCloseEventFlag(ef);
  return nullptr;
}

}  // namespace

bool rest_watcher_start(bool fix_on_wake) {
  g_fix_on_wake = fix_on_wake;
  pthread_t trd;
  const int rc = pthread_create(&trd, nullptr, watcher_thread, nullptr);
  if (rc != 0) {
    log_message("restwatch: thread create failed: %d\n", rc);
    return false;
  }
  pthread_detach(trd);
  return true;
}
