/*
 * PS5 Sync Clock - logging and console notifications.
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>

extern "C" {
struct NotifyRequest {
  char useless[45];
  char message[3075];
};

int sceKernelSendNotificationRequest(int device, NotifyRequest* request,
                                     size_t size, int unused);
}

void log_message(const char* format, ...) {
  char line[768] = {};
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (written <= 0) {
    return;
  }
  size_t length = static_cast<size_t>(written);
  if (length >= sizeof(line)) {
    length = sizeof(line) - 1;
  }
  fwrite(line, 1, length, stdout);
  fflush(stdout);
}

void notify_message(const char* format, ...) {
  NotifyRequest request = {};
  va_list ap;
  va_start(ap, format);
  vsnprintf(request.message, sizeof(request.message), format, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}
