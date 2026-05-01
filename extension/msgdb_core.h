#ifndef MSGDB_CORE_H
#define MSGDB_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MSGDB_VERSION "1.3.0"

typedef struct msgdb_span {
  size_t offset;
  size_t length;
  bool present;
} msgdb_span;

msgdb_span msgdb_category_span(const char *value, size_t length);
bool msgdb_is_category(const char *value, size_t length);
msgdb_span msgdb_id_span(const char *value, size_t length);
msgdb_span msgdb_cardinal_id_span(const char *value, size_t length);
int64_t msgdb_hash64(const unsigned char *value, size_t length);
bool msgdb_uuid_v4_canonical(const char *value, size_t length, char out[37]);
void msgdb_uuid_v4_from_bytes(const unsigned char bytes[16], char out[37]);

#endif
