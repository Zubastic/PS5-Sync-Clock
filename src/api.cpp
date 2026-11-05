/*
 * PS5 Sync Clock - HTTP API routing and handlers.
 *
 * Endpoints:
 *   GET  /                - embedded web UI
 *   GET  /api/time        - JSON with the current value of every clockid
 *                           plus the /dev/srtc probe (trophy timestamp
 *                           source)
 *   GET  /api/srtc        - verbose /dev/srtc diagnostic: raw seconds,
 *                           SAMU status and the exact unlock timestamp
 *                           value the trophy code would store right now
 *   GET  /api/set?ts=N    - set the console time to unix seconds N
 *                           (time travels as a URL parameter; the request
 *                           body is never parsed, which keeps this working
 *                           regardless of socket read quirks)
 *   POST /api/ntp         - query NTP servers from the console and apply
 *   POST /api/quit        - stop the payload
 */

#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "asset.h"
#include "clock.h"
#include "log.h"
#include "sntp.h"
#include "websrv.h"

#ifndef SYNC_CLOCK_VERSION
#define SYNC_CLOCK_VERSION "dev"
#endif

namespace {

void handle_api_time(const http_request_t* req) {
  char json[4096];
  clock_build_state_json(json, sizeof(json));
  http_send_json(req->fd, json);
}

/*
 * Diagnostic endpoint: everything the trophy unlock path reads from
 * /dev/srtc, plus the timestamp value that would land in the trophy
 * file for an unlock happening right now.
 */
void handle_api_srtc(const http_request_t* req) {
  srtc_snapshot_t s = {};
  srtc_probe(&s);

  char human[40] = "-";
  if (s.ioctl_ok && s.status == 0 && s.unix_sec > 0) {
    struct timespec ts = {};
    ts.tv_sec = static_cast<time_t>(s.unix_sec);
    struct tm tm_utc = {};
    if (gmtime_r(&ts.tv_sec, &tm_utc) != nullptr) {
      strftime(human, sizeof(human), "%Y-%m-%d %H:%M:%S", &tm_utc);
    }
  }

  char json[512];
  snprintf(json, sizeof(json),
           "{\"opened\":%d,\"ioctl\":%d,\"errno\":%d,\"status\":%u,"
           "\"srtc_sec\":%lld,\"unix\":%lld,\"trophy_tick\":%llu,"
           "\"human\":\"%s\"}",
           s.opened, s.ioctl_ok, s.ioctl_errno, s.status,
           static_cast<long long>(s.srtc_sec),
           static_cast<long long>(s.unix_sec),
           (s.ioctl_ok && s.status == 0)
               ? static_cast<unsigned long long>(s.trophy_tick)
               : 0ULL,
           human);
  http_send_json(req->fd, json);
}

/*
 * Read-only kernel scan: srtc softc candidates with a field dump.
 * Uses kernel_copyout only, so it cannot panic the kernel.
 */
void handle_api_srtc_scan(const http_request_t* req) {
  char json[4096];
  if (!srtc_scan_report(json, sizeof(json))) {
    http_send_json(req->fd, json); /* {"count":0,...} is still useful */
    return;
  }
  http_send_json(req->fd, json);
}

/*
 * Explicit opt-in kernel write: fill the empty srtc boot-time cache.
 * Only call this after the /api/srtc/scan output has been verified.
 */
void handle_api_srtc_fix(const http_request_t* req) {
  char value[64] = {};
  long long seconds = 0;
  if (http_get_query_arg(req, "ts", value, sizeof(value)) && value[0]) {
    seconds = strtoll(value, nullptr, 10);
  } else {
    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    seconds = static_cast<long long>(now.tv_sec);
  }

  const char* error = nullptr;
  if (!srtc_kernel_fix_boot_cache(seconds, &error)) {
    char detail[128];
    snprintf(detail, sizeof(detail), "%s (ts=%lld)",
             error ? error : "fix failed", seconds);
    http_send_error_json(req->fd, detail);
    return;
  }

  srtc_snapshot_t s = {};
  srtc_probe(&s);
  char json[256];
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"ts\":%lld,\"status\":%u,\"srtc_sec\":%lld,"
           "\"unix\":%lld}",
           seconds, s.status, static_cast<long long>(s.srtc_sec),
           static_cast<long long>(s.unix_sec));
  http_send_json(req->fd, json);
}

void handle_api_set(const http_request_t* req) {
  char value[64] = {};
  if (!http_get_query_arg(req, "ts", value, sizeof(value)) || !value[0]) {
    http_send_error_json(req->fd, "missing ts parameter");
    return;
  }

  char* end = nullptr;
  const long long seconds = strtoll(value, &end, 10);
  const char* error = nullptr;
  if (!clock_set_from_unix_seconds(seconds, &error)) {
    char detail[128];
    snprintf(detail, sizeof(detail), "%s (%s)", error ? error : "invalid",
             value);
    http_send_error_json(req->fd, detail);
    return;
  }

  char detail[128];
  snprintf(detail, sizeof(detail),
           "set to %lld (domains 17/16 + realtime + SRTC)", seconds);
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"detail\":\"%s\"}", detail);
  http_send_json(req->fd, buf);
  notify_message("Sync Clock: time set to %lld", seconds);
}

void handle_api_ntp(const http_request_t* req) {
  struct timespec ts = {};
  if (!sntp_fetch_time(&ts)) {
    http_send_error_json(req->fd, "no valid NTP response");
    return;
  }

  if (!clock_apply_time(&ts)) {
    http_send_error_json(req->fd, "privilege escalation or clock write failed");
    return;
  }

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"status\":\"ok\",\"detail\":\"NTP %lld applied (domains 17/16 "
           "+ realtime)\"}",
           static_cast<long long>(ts.tv_sec));
  http_send_json(req->fd, buf);
  notify_message("Sync Clock: NTP time %lld applied",
                 static_cast<long long>(ts.tv_sec));
}

void handle_api_quit(const http_request_t* req) {
  http_send_json(req->fd, "{\"status\":\"ok\",\"detail\":\"payload stopping\"}");
  notify_message("Sync Clock: payload stopped");
  http_request_shutdown();
}

}  // namespace

void api_handle_request(const http_request_t* req) {
  if (strcmp(req->method, "GET") == 0 &&
      (strcmp(req->path, "/") == 0 || strcmp(req->path, "/index.html") == 0)) {
    asset_serve(req->fd, "/");
    return;
  }
  if (strcmp(req->path, "/api/time") == 0) {
    handle_api_time(req);
    return;
  }
  if (strncmp(req->path, "/api/set", 8) == 0) {
    handle_api_set(req);
    return;
  }
  if (strcmp(req->path, "/api/srtc") == 0) {
    handle_api_srtc(req);
    return;
  }
  if (strcmp(req->path, "/api/srtc/scan") == 0) {
    handle_api_srtc_scan(req);
    return;
  }
  if (strcmp(req->path, "/api/srtc/step") == 0) {
    char json[256];
    srtc_diag_step(json, sizeof(json));
    http_send_json(req->fd, json);
    return;
  }
  if (strcmp(req->path, "/api/kdump") == 0) {
    /* Dump kernel .data (+ optional extra bytes past allproc) to
     * /data/homebrew/. Text is XOM and is never read. */
    char extra_str[32] = {};
    long long extra = 0;
    if (http_get_query_arg(req, "extra", extra_str, sizeof(extra_str)) &&
        extra_str[0]) {
      extra = strtoll(extra_str, nullptr, 0);
    }
    const char* error = nullptr;
    if (!srtc_kdump_file(extra, &error)) {
      char detail[128];
      snprintf(detail, sizeof(detail), "%s", error ? error : "kdump failed");
      http_send_error_json(req->fd, detail);
      return;
    }
    http_send_json(req->fd,
                   "{\"ok\":true,\"path\":\"/data/homebrew/"
                   "kernel-11.20-data.bin\"}");
    return;
  }
  if (strcmp(req->path, "/api/srtc/fix") == 0) {
    handle_api_srtc_fix(req);
    return;
  }
  if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/ntp") == 0) {
    handle_api_ntp(req);
    return;
  }
  if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/quit") == 0) {
    handle_api_quit(req);
    return;
  }
  http_send(req->fd, "404 Not Found", "text/plain", "not found\n", 9);
}
