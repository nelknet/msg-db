#include "msgdb_core.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *buffer = NULL;
  msgdb_span span;
  char uuid[37] = {0};

  if (size > 4096U) {
    return 0;
  }

  buffer = malloc(size + 1U);
  if (buffer == NULL) {
    return 0;
  }

  if (size > 0U) {
    memcpy(buffer, data, size);
  }
  buffer[size] = '\0';

  span = msgdb_category_span(buffer, size);
  if (span.present && span.offset + span.length <= size) {
    (void)msgdb_hash64((const unsigned char *)buffer + span.offset, span.length);
  }

  span = msgdb_id_span(buffer, size);
  if (span.present && span.offset + span.length <= size) {
    (void)msgdb_hash64((const unsigned char *)buffer + span.offset, span.length);
  }

  span = msgdb_cardinal_id_span(buffer, size);
  if (span.present && span.offset + span.length <= size) {
    (void)msgdb_hash64((const unsigned char *)buffer + span.offset, span.length);
  }

  (void)msgdb_is_category(buffer, size);
  (void)msgdb_hash64(data, size);
  (void)msgdb_uuid_v4_canonical(buffer, size, uuid);

  free(buffer);
  return 0;
}

#ifdef MSGDB_STANDALONE_FUZZER
int main(void);

int main(void) {
  static const uint8_t seed[] = "account-123+456";

  return LLVMFuzzerTestOneInput(seed, sizeof(seed) - 1U);
}
#endif
