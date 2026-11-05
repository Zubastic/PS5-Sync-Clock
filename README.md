# PS5 Sync Clock

Standalone PS5 payload for setting the system time from a simple web UI.
Designed for consoles with a dead CMOS/RTC battery: every boot the clock
resets, RTC domains go empty (`0x80B50001` from libSceRtc, NP/trophy
errors), and the only way to recover is to write the clock domains again.

The project ships **two payloads**:

- **`sync-clock.elf`** — interactive web UI on port 5915 with manual
  time sync and SRTC diagnostics. Never writes the clock on its own.
- **`sync-clock-auto.elf`** — resident background daemon. Fixes the
  clock once at startup and after **every wake from rest mode**, so
  trophy timestamps stay valid for the whole session. No web server.

After a successful set, the offsets persist in RegMgr and survive
reboots until the next power-loss event.

## Features

### sync-clock.elf (web UI)

- **NTP via browser** (primary): the browser tab queries CORS-enabled
  HTTPS time APIs, compensates the network delay (RTT/2, like SNTP),
  and sends the corrected unix time to the console. Works from a PC or
  phone on the same network even when the PS5 itself is offline.
- **NTP via console**: the console queries public NTP servers over
  UDP/123 (based on ps5-date-time-sync by Deckerr97). Requires internet
  access on the PS5.
- **Manual**: paste raw unix seconds (UTC) and set them directly.
- **SRTC Cache (/dev/srtc)**: probe the trophy-time source, read-only
  kernel scan of the srtc softc, and an opt-in kernel write that fills
  the empty SRTC boot-time cache (the fix for `0x800F0532` /
  broken trophy timestamps).
- **Stop payload**: gracefully stops the HTTP server and exits the ELF.
- Live table of Sony clockids refreshed each second, so you can see
  which RTC domains are readable and whether they match. Dark/light
  theme, mobile-friendly layout.
- **Rest-mode watcher (socket-only)**: on wake from rest mode the web
  server recreates its listen socket, so the UI keeps working without
  relaunching the payload. It never touches the clock.

### sync-clock-auto.elf (resident daemon)

- Fixes the time **once at startup** and after **every wake from rest
  mode** (`clock_on_resume_fix`): fills the SRTC boot cache from the
  console's own wall clock (only when it is actually broken), then
  re-aligns RTC domains 16/17.
- Fully offline: everything is re-aligned from the console's own
  `CLOCK_REALTIME`, no NTP or host required.
- Notifies on failure; logs are appended to
  `/data/homebrew/sync-clock-auto.log` (the payload manager does not
  show stdout).
- Add it to the PLK autoload list once and it keeps guarding the clock
  for the whole session.

Both payloads show up as `sync-clock.elf` / `sync-clock-auto.elf` in
the payload manager (not the default `payload.elf`).

## How it works

The PS5 keeps time in RTC-derived clock domains exposed through
`clock_gettime`/`clock_settime` with Sony-specific clockids:

| clockid | meaning |
|---|---|
| 16 | RTC domain 0 - user wall clock |
| 17 | RTC domain 1 - network time (NP / trophy stack reads this) |
| 19 | force domain 0 |

Writing domains 16/17 requires the system authid; the payloads
temporarily elevate their ucred (`kernel_set_ucred_authid` +
`kernel_set_ucred_caps`), apply the time to both domains plus
`settimeofday`, then restore the original credentials.

### SRTC boot cache (trophy time source)

The trophy stack reads its timestamps from `/dev/srtc`. On consoles
with a dead battery the device answers `0x800F0532` because the
boot-time cache inside the kernel's `srtc_softc` is empty. The fix
writes `[softc + 0x28] = boot_2012_sec - uptime` (SRTC epoch is
2012-01-01, unix minus 1325376000) directly into kernel memory:

- firmware 11.20: `softc = KERNEL_ADDRESS_DATA_BASE + 0x2850010`
  (hardcoded offset, no runtime sig-scanning);
- only SDK primitives are used (`kernel_copyout`/`kernel_copyin`),
  kernel text is never touched, so no kernel panics;
- the write happens **only when the probe shows the cache is actually
  broken** (`status != 0` or stale seconds);
- trophy timestamp value = `unix*1e6 + 62135596800000000`.

The web-UI SRTC panel offers the same operations manually:
probe (`/api/srtc`), read-only softc scan (`/api/srtc/scan`),
opt-in cache write (`/api/srtc/fix`).

### Rest mode handling

Both payloads poll the `SceSystemStateMgrInfo` event flag (ShadowMountPlus
style, via libkernel stubs). Two modes:

- `sync-clock.elf` (`fix_on_wake = false`): on wake only
  `websrv_request_restart()` is called — the HTTP listen socket is
  recreated because the network stack re-initializes. A recovery loop
  retries bind failures, so the payload never turns into a zombie.
- `sync-clock-auto.elf` (`fix_on_wake = true`): on wake it runs the
  full `clock_on_resume_fix()` (SRTC cache + domains 16/17).

## Requirements

- A jailbroken PS5 on firmware **11.20** with a payload loader
  (elfldr / Payload Manager / PLK).
- [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) to
  build (Linux host; on Windows use WSL/VMware share).

## Build

Set `PS5_PAYLOAD_SDK` to your local SDK path, then run:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
./build.sh
```

`build.sh` runs `make clean`, deletes `gen/` (so embedded resources are
always rebuilt from scratch) and runs `make`.

The build output is:

```text
sync-clock.elf       web UI payload
sync-clock-auto.elf  resident auto-fix daemon
```

Generated build outputs are intentionally ignored by git:

```text
build/
gen/
sync-clock.elf
sync-clock-auto.elf
```

## Deploy

Copy both ELF files to your PS5 payload folder.

- Launch `sync-clock.elf` with Payload Manager / elfldr and open:

  ```text
  http://<PS5_IP>:5915/
  ```

  The payload shows a system notification with the URL at startup.

- Add `sync-clock-auto.elf` to the **PLK autoload list** so it starts
  on boot and stays resident.

## HTTP API

| Method | Path | Description |
|---|---|---|
| GET | `/` | HTML interface |
| GET | `/api/time` | JSON: current values of all clockids (unreadable - null + errno) plus the /dev/srtc probe |
| GET | `/api/set?ts=N` | set the console time to unix seconds N (URL parameter, no body) |
| POST | `/api/set` | same, with `ts` in the query string |
| POST | `/api/ntp` | console-side SNTP sync and apply |
| GET | `/api/srtc` | verbose /dev/srtc diagnostic: raw seconds, SAMU status, the exact trophy unlock timestamp |
| GET | `/api/srtc/scan` | read-only kernel scan: srtc softc candidates with a field dump (kernel_copyout only) |
| GET | `/api/srtc/step` | small one-step srtc diagnostic |
| GET | `/api/srtc/fix?ts=N` | opt-in kernel write of the SRTC boot-time cache (unix seconds N, defaults to now) |
| GET | `/api/kdump` | dump kernel .data (+ optional `extra` bytes) to /data/homebrew for offline analysis |
| POST | `/api/quit` | stop the payload (server loop exits, ELF returns) |

Examples:

```sh
curl "http://192.168.1.5:5915/api/time"
curl "http://192.168.1.5:5915/api/set?ts=$(date +%s)"
curl -X POST "http://192.168.1.5:5915/api/ntp"
curl "http://192.168.1.5:5915/api/srtc"
curl "http://192.168.1.5:5915/api/srtc/scan"
curl "http://192.168.1.5:5915/api/srtc/fix?ts=$(date +%s)"
```

## Project layout

```text
assets/index.html      web UI (embedded into the ELF at build time)
src/main.cpp           sync-clock.elf entrypoint (web server + socket-only watcher)
src/syncauto_main.cpp  sync-clock-auto.elf entrypoint (resident daemon)
src/websrv.{h,cpp}     tiny HTTP server (request parsing, responses, restart flag)
src/api.{h,cpp}        endpoint routing and handlers
src/clock.{h,cpp}      clock domain access, privilege elevation, SRTC cache fix
src/restwatch.{h,cpp}  rest-mode watcher (dual mode: socket-only / full fix)
src/sntp.{h,cpp}       console-side SNTP client
src/asset.{h,cpp}      embedded asset registry
src/log.{h,cpp}        stdout logging + system notifications
gen-asset-module.py    embeds assets/ into a C module (from ps5-payload-sdk)
```

## Notes

- After setting the time, RTC domains become readable and the
  NP/trophy stack works again; the values persist across reboots until
  the next power anomaly.
- The NTP-via-browser path is timezone-safe: time APIs are parsed as
  UTC explicitly (timeapi.io returns UTC strings without a `Z` suffix,
  which `Date.parse` would otherwise treat as browser-local time).
- The web server sends `Cache-Control: no-store`, so UI updates are
  picked up without hard-refreshing the browser.
- The firmware-specific SRTC offset (`DATA_BASE + 0x2850010`) is valid
  for 11.20. Other firmwares need a new offset (kernel dumps from
  `/api/kdump` are used to find it offline).
- This project assumes you understand the risks of running PS5
  homebrew payloads.
