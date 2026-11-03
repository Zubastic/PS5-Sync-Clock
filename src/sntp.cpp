/*
 * PS5 Sync Clock - console-side SNTP client.
 */

#include "sntp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "log.h"

extern "C" {
int sceNetInit();
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
}

namespace {

constexpr uint16_t kNtpPort = 123;
constexpr size_t kNtpPacketSize = 48;
constexpr size_t kMaxNtpResponseSize = 512;
constexpr size_t kNtpOriginateTimestampOffset = 24;
constexpr size_t kNtpTransmitTimestampOffset = 40;
constexpr size_t kNtpTimestampSize = 8;
constexpr uint8_t kNtpVersion = 4;
constexpr uint8_t kNtpClientMode = 3;
constexpr uint8_t kNtpServerMode = 4;
constexpr uint8_t kNtpClientFlags = (kNtpVersion << 3) | kNtpClientMode;
constexpr int kNetworkPoolSize = 64 * 1024;
constexpr int kSocketTimeoutSeconds = 3;
constexpr int kAttemptsPerServer = 2;
constexpr unsigned int kRetryDelaySeconds = 1;
constexpr uint32_t kNtpToUnixEpoch = 2208988800U;

const char* const kNtpServers[] = {
    "162.159.200.1",     "162.159.200.123", "time.cloudflare.com",
    "pool.ntp.org",      "time.google.com",
};

int g_pool_id = -1;

bool monotonic_microseconds(int64_t* output) {
  struct timespec value = {};
  if (!output || clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return false;
  }
  *output = static_cast<int64_t>(value.tv_sec) * 1000000LL +
            static_cast<int64_t>(value.tv_nsec / 1000);
  return true;
}

uint32_t read_big_endian_u32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

void write_big_endian_u32(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value >> 24);
  bytes[1] = static_cast<uint8_t>(value >> 16);
  bytes[2] = static_cast<uint8_t>(value >> 8);
  bytes[3] = static_cast<uint8_t>(value);
}

bool write_client_transmit_timestamp(uint8_t* packet) {
  struct timeval current = {};
  if (gettimeofday(&current, nullptr) != 0) {
    return false;
  }
  const int64_t unix_seconds = static_cast<int64_t>(current.tv_sec);
  if (unix_seconds < -static_cast<int64_t>(kNtpToUnixEpoch)) {
    return false;
  }
  const uint64_t full_ntp_seconds =
      static_cast<uint64_t>(unix_seconds + static_cast<int64_t>(kNtpToUnixEpoch));
  const uint32_t wire_seconds = static_cast<uint32_t>(full_ntp_seconds);
  const uint32_t wire_fraction = static_cast<uint32_t>(
      (static_cast<uint64_t>(current.tv_usec) << 32) / 1000000ULL);
  uint32_t fraction = wire_fraction;
  if (wire_seconds == 0 && fraction == 0) {
    fraction = 1;
  }
  write_big_endian_u32(packet + kNtpTransmitTimestampOffset, wire_seconds);
  write_big_endian_u32(packet + kNtpTransmitTimestampOffset + 4, fraction);
  return true;
}

bool configure_socket_timeouts(int socket_fd) {
  struct timeval timeout = {};
  timeout.tv_sec = kSocketTimeoutSeconds;
  return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                    sizeof(timeout)) == 0 &&
         setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                    sizeof(timeout)) == 0;
}

bool resolve_server(const char* hostname, struct sockaddr_in* address) {
  *address = {};
  if (inet_pton(AF_INET, hostname, &address->sin_addr) == 1) {
    address->sin_family = AF_INET;
    address->sin_port = htons(kNtpPort);
    return true;
  }

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  struct addrinfo* results = nullptr;
  if (getaddrinfo(hostname, nullptr, &hints, &results) != 0) {
    return false;
  }
  bool resolved = false;
  for (struct addrinfo* current = results; current; current = current->ai_next) {
    if (!current->ai_addr || current->ai_addrlen < sizeof(struct sockaddr_in)) {
      continue;
    }
    memcpy(address, current->ai_addr, sizeof(struct sockaddr_in));
    address->sin_port = htons(kNtpPort);
    resolved = true;
    break;
  }
  freeaddrinfo(results);
  return resolved;
}

bool request_time(const struct sockaddr_in& server, struct timespec* output) {
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) {
    return false;
  }
  if (!configure_socket_timeouts(socket_fd)) {
    close(socket_fd);
    return false;
  }

  uint8_t request[kNtpPacketSize] = {};
  request[0] = kNtpClientFlags;
  if (!write_client_transmit_timestamp(request)) {
    close(socket_fd);
    return false;
  }

  int64_t start_us = 0;
  int64_t end_us = 0;
  const bool have_start = monotonic_microseconds(&start_us);

  const ssize_t sent =
      sendto(socket_fd, request, sizeof(request), 0,
             reinterpret_cast<const struct sockaddr*>(&server), sizeof(server));
  if (sent != static_cast<ssize_t>(sizeof(request))) {
    close(socket_fd);
    return false;
  }

  uint8_t response[kMaxNtpResponseSize] = {};
  struct sockaddr_in source = {};
  socklen_t source_length = sizeof(source);
  const ssize_t received =
      recvfrom(socket_fd, response, sizeof(response), 0,
               reinterpret_cast<struct sockaddr*>(&source), &source_length);
  const bool have_end = monotonic_microseconds(&end_us);
  close(socket_fd);

  if (received < static_cast<ssize_t>(kNtpPacketSize)) {
    return false;
  }
  if (source.sin_family != AF_INET ||
      source.sin_addr.s_addr != server.sin_addr.s_addr ||
      memcmp(response + kNtpOriginateTimestampOffset,
             request + kNtpTransmitTimestampOffset, kNtpTimestampSize) != 0) {
    return false;
  }

  const uint8_t flags = response[0];
  const uint8_t stratum = response[1];
  if ((flags >> 6) == 3 || (flags & 0x07) != kNtpServerMode ||
      ((flags >> 3) & 0x07) != kNtpVersion || stratum == 0 || stratum >= 16) {
    return false;
  }

  const uint32_t ntp_seconds = read_big_endian_u32(response + 40);
  const uint32_t ntp_fraction = read_big_endian_u32(response + 44);
  if (ntp_seconds == 0 && ntp_fraction == 0) {
    return false;
  }

  uint64_t full_ntp_seconds = ntp_seconds;
  if (full_ntp_seconds < kNtpToUnixEpoch) {
    full_ntp_seconds += (1ULL << 32); /* NTP era 2036 */
  }
  const int64_t unix_seconds = static_cast<int64_t>(
      full_ntp_seconds - static_cast<uint64_t>(kNtpToUnixEpoch));
  if (unix_seconds < kMinimumUnixTime || unix_seconds > kMaximumUnixTime) {
    return false;
  }

  int64_t microseconds =
      static_cast<int64_t>((static_cast<uint64_t>(ntp_fraction) * 1000000ULL) >> 32);
  if (have_start && have_end && end_us > start_us) {
    microseconds += (end_us - start_us) / 2; /* half RTT correction */
  }

  output->tv_sec = unix_seconds;
  output->tv_nsec = static_cast<long>(microseconds * 1000LL);
  return true;
}

}  // namespace

bool sntp_init_network(void) {
  const int result = sceNetInit();
  if (result != 0) {
    log_message("sceNetInit failed: rc=%d\n", result);
    return false;
  }
  g_pool_id = sceNetPoolCreate("ps5-sync-clock", kNetworkPoolSize, 0);
  if (g_pool_id < 0) {
    log_message("sceNetPoolCreate failed: rc=%d\n", g_pool_id);
    return false;
  }
  return true;
}

bool sntp_fetch_time(struct timespec* output) {
  if (g_pool_id < 0 && !sntp_init_network()) {
    return false;
  }

  for (const char* server_name : kNtpServers) {
    struct sockaddr_in server = {};
    if (!resolve_server(server_name, &server)) {
      continue;
    }
    for (int attempt = 0; attempt < kAttemptsPerServer; ++attempt) {
      log_message("querying %s (attempt %d/%d)\n", server_name, attempt + 1,
                  kAttemptsPerServer);
      if (request_time(server, output)) {
        return true;
      }
      if (attempt + 1 < kAttemptsPerServer) {
        sleep(kRetryDelaySeconds);
      }
    }
  }
  log_message("no valid NTP response was received\n");
  return false;
}
