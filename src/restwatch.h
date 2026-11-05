/*
 * PS5 Sync Clock - rest mode (suspend/resume) watcher.
 *
 * Polls the SceSystemStateMgrInfo kernel event flag like
 * ShadowMountPlus does (sceKernelOpenEventFlag + sceKernelPollEventFlag
 * through the libkernel stubs). The low 16 bits of the flag value carry
 * the system state:
 *   100 SHUTDOWN_ON_GOING, 200 POWER_SAVING, 300 SUSPEND_ON_GOING,
 *   500 MAIN_ON_STANDBY, 1000 WORKING.
 *
 * The watcher is used in two modes:
 *  - sync-clock.elf (fix_on_wake = false): on wake it ONLY asks the
 *    web server to recreate its listen socket; it never writes clock
 *    state.
 *  - sync-clock-auto.elf (fix_on_wake = true): a resident daemon that
 *    restores time after every wake (srtc boot cache + RTC domains
 *    16/17, all from the console's own clock) and stays in the
 *    background monitoring further sleep/wake cycles.
 */

#pragma once

/*
 * Start the state manager watcher thread.
 * With fix_on_wake = false only the web-server socket recreate is
 * requested on resume; with true the full time recovery
 * (clock_on_resume_fix) runs on every wake.
 * Returns false when the thread could not be created.
 */
bool rest_watcher_start(bool fix_on_wake);
