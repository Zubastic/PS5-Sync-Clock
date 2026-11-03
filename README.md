# PS5 Sync Clock

Standalone PS5 payload for setting the system time from a simple web UI.
Designed for consoles with a dead CMOS/RTC battery: every boot the clock
resets, RTC domains go empty (`0x80B50001` from libSceRtc, NP/trophy
errors), and the only way to recover is to write the clock domains again.

The payload runs a small HTTP server on the PS5 and offers three ways to
set the time. After a successful set, the offsets persist in RegMgr and
survive reboots until the next power-loss event.

## Features

- **NTP via browser** (primary): the browser tab queries CORS-enabled
  HTTPS time APIs, compensates the network delay (RTT/2, like SNTP),
  and sends the corrected unix time to the console. Works from a PC or
  phone on the same network even when the PS5 itself is offline.
- **NTP via console**: the console queries public NTP servers over
  UDP/123 (based on ps5-date-time-sync by Deckerr97). Requires internet
  access on the PS5.
- **Manual**: paste raw unix seconds (UTC) and set them directly.
- **Stop payload**: gracefully stops the HTTP server and exits the ELF.
- Live table of every Sony clockid (0..20) refreshed each second, so
  you can see which RTC domains are readable and whether they match.

## How it works

The PS5 keeps time in RTC-derived clock domains exposed through
`clock_gettime`/`clock_settime` with Sony-specific clockids:

| clockid | meaning |
|---|---|
| 16 | RTC domain 0 - user wall clock |
| 17 | RTC domain 1 - network time (NP / trophy stack reads this) |
| 19 | force domain 0 |

Writing domains 16/17 requires the system authid; the payload
temporarily elevates its ucred (`kernel_set_ucred_authid` +
`kernel_set_ucred_caps`), applies the time to both domains plus
`settimeofday`, then restores the original credentials.

The HTTP API deliberately passes the timestamp as a URL parameter
(`GET /api/set?ts=N`) rather than a request body: on live consoles a
single `read()` may return only the request headers while the body
arrives in a later TCP segment, which made body-based endpoints
unreliable. The server still reads full requests correctly, but the
set-endpoint no longer depends on it.

## Requirements

- A jailbroken PS5 with a payload loader (elfldr / Payload Manager).
- [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) to build.

## Build

Set `PS5_PAYLOAD_SDK` to your local SDK path, then run:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
./build.sh
```

The build output is:

```text
sync-clock.elf
```

Generated build outputs are intentionally ignored by git:

```text
build/
gen/
sync-clock.elf
```

## Deploy

Copy `sync-clock.elf` to your PS5 payload folder and launch it with
Payload Manager / elfldr. Then open:

```text
http://<PS5_IP>:5915/
```

The payload shows a system notification with the URL at startup.

## HTTP API

| Method | Path | Description |
|---|---|---|
| GET | `/` | HTML interface |
| GET | `/api/time` | JSON: current values of all clockids 0..20 (unreadable - null + errno) |
| GET | `/api/set?ts=N` | set the console time to unix seconds N (URL parameter, no body) |
| POST | `/api/set` | same, with `ts` in the query string |
| POST | `/api/ntp` | console-side SNTP sync and apply |
| POST | `/api/quit` | stop the payload (server loop exits, ELF returns) |

Examples:

```sh
curl "http://192.168.1.5:5915/api/time"
curl "http://192.168.1.5:5915/api/set?ts=$(date +%s)"
curl -X POST "http://192.168.1.5:5915/api/ntp"
```

## Project layout

```text
assets/index.html      web UI (embedded into the ELF at build time)
src/main.cpp           payload entrypoint
src/websrv.{h,cpp}     tiny HTTP server (request parsing, responses)
src/api.{h,cpp}        endpoint routing and handlers
src/clock.{h,cpp}      clock domain access, privilege elevation, apply
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
- This project assumes you understand the risks of running PS5
  homebrew payloads.
