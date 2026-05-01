#include "msgdb_core.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void require_property(bool condition);
static size_t first_index_of(const char *value, size_t size, char needle);
static bool has_no_dash(const char *value, size_t size);
static void require_valid_span(msgdb_span span, size_t size);
static void require_uuid_v4_canonical_output(const char out[37]);

static void require_property(bool condition) {
  if (!condition) {
    abort();
  }
}

static size_t first_index_of(const char *value, size_t size, char needle) {
  for (size_t i = 0U; i < size; i++) {
    if (value[i] == needle) {
      return i;
    }
  }

  return size;
}

static bool has_no_dash(const char *value, size_t size) {
  return first_index_of(value, size, '-') == size;
}

static void require_valid_span(msgdb_span span, size_t size) {
  if (!span.present) {
    return;
  }

  require_property(span.offset <= size);
  require_property(span.length <= size - span.offset);
}

static void require_uuid_v4_canonical_output(const char out[37]) {
  static const size_t hyphen_positions[] = {8U, 13U, 18U, 23U};

  for (size_t i = 0U; i < 36U; i++) {
    bool is_hyphen = false;

    for (size_t j = 0U; j < sizeof(hyphen_positions) / sizeof(hyphen_positions[0]); j++) {
      if (i == hyphen_positions[j]) {
        is_hyphen = true;
        break;
      }
    }

    if (is_hyphen) {
      require_property(out[i] == '-');
      continue;
    }

    require_property((out[i] >= '0' && out[i] <= '9') || (out[i] >= 'a' && out[i] <= 'f'));
  }

  require_property(out[14] == '4');
  require_property(out[19] == '8' || out[19] == '9' || out[19] == 'a' || out[19] == 'b');
  require_property(out[36] == '\0');
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *buffer = NULL;
  msgdb_span category_span;
  msgdb_span id_span;
  msgdb_span cardinal_id_span;
  char uuid[37] = {0};
  size_t first_dash = 0U;
  bool uuid_valid = false;

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

  first_dash = first_index_of(buffer, size, '-');

  category_span = msgdb_category_span(buffer, size);
  id_span = msgdb_id_span(buffer, size);
  cardinal_id_span = msgdb_cardinal_id_span(buffer, size);

  require_valid_span(category_span, size);
  require_valid_span(id_span, size);
  require_valid_span(cardinal_id_span, size);

  require_property(category_span.present);
  require_property(category_span.offset == 0U);
  require_property(category_span.length == first_dash);
  require_property(msgdb_is_category(buffer, size) == has_no_dash(buffer, size));

  if (first_dash == size) {
    require_property(!id_span.present);
    require_property(!cardinal_id_span.present);
  } else {
    size_t plus_after_dash = first_dash + 1U;

    require_property(id_span.present);
    require_property(id_span.offset == first_dash + 1U);
    require_property(id_span.length == size - id_span.offset);

    while (plus_after_dash < size && buffer[plus_after_dash] != '+') {
      plus_after_dash++;
    }

    require_property(cardinal_id_span.present);
    require_property(cardinal_id_span.offset == id_span.offset);
    require_property(cardinal_id_span.length == plus_after_dash - id_span.offset);
    require_property(cardinal_id_span.length <= id_span.length);
  }

  if (category_span.present) {
    (void)msgdb_hash64((const unsigned char *)buffer + category_span.offset,
                       category_span.length);
  }

  if (id_span.present) {
    (void)msgdb_hash64((const unsigned char *)buffer + id_span.offset, id_span.length);
  }

  if (cardinal_id_span.present) {
    (void)msgdb_hash64((const unsigned char *)buffer + cardinal_id_span.offset,
                       cardinal_id_span.length);
  }

  (void)msgdb_is_category(buffer, size);
  (void)msgdb_hash64(data, size);
  uuid_valid = msgdb_uuid_v4_canonical(buffer, size, uuid);
  if (uuid_valid) {
    char canonical_again[37] = {0};

    require_uuid_v4_canonical_output(uuid);
    require_property(msgdb_uuid_v4_canonical(uuid, 36U, canonical_again));
    require_property(strcmp(uuid, canonical_again) == 0);
  }

  free(buffer);
  return 0;
}

#ifdef MSGDB_STANDALONE_FUZZER
int main(int argc, char **argv);

int main(int argc, char **argv) {
  static const uint8_t seed[] = "account-123+456";

  (void)argc;
  (void)argv;
  return LLVMFuzzerTestOneInput(seed, sizeof(seed) - 1U);
}
#endif
