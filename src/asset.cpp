/*
 * PS5 Sync Clock - embedded web assets.
 */

#include "asset.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "websrv.h"

#define PAGE_404 \
  "<!doctype html><html><head><title>Not found</title></head>" \
  "<body>Not found</body></html>"

typedef struct asset {
  const char* path;
  const char* mime;
  void* data;
  size_t size;
  struct asset* next;
} asset_t;

static asset_t* g_asset_head = nullptr;

void asset_register(const char* path, void* data, size_t size,
                    const char* mime) {
  asset_t* a = static_cast<asset_t*>(calloc(1, sizeof(*a)));
  if (!a) {
    return;
  }
  a->path = path;
  a->mime = mime;
  a->data = data;
  a->size = size;
  a->next = g_asset_head;
  g_asset_head = a;
}

int asset_serve(int fd, const char* url) {
  char path[PATH_MAX];

  /* collapse duplicate slashes */
  char* ptr = path;
  char* end = path + sizeof(path) - 1;
  for (size_t i = 0; url && url[i] && ptr < end; i++) {
    if (url[i] == '/' && url[i + 1] == '/') continue;
    *ptr++ = url[i];
  }
  *ptr = '\0';

  for (asset_t* a = g_asset_head; a; a = a->next) {
    if (strcmp(path, a->path) != 0) {
      continue;
    }
    const char* mime = a->mime ? a->mime : "application/octet-stream";
    char mime_buf[64];
    if (strncmp(mime, "text/html", 9) == 0) {
      snprintf(mime_buf, sizeof(mime_buf), "%s; charset=utf-8", mime);
      mime = mime_buf;
    }
    return http_send(fd, "200 OK", mime, a->data, a->size);
  }

  return http_send(fd, "404 Not Found", "text/html", PAGE_404,
                   strlen(PAGE_404));
}
