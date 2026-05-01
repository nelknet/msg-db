#include "msgdb_core.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void test_stream_name_parsing(void);
static void test_hash_vectors(void);
static void test_uuid_validation(void);

int main(void) {
  test_stream_name_parsing();
  test_hash_vectors();
  test_uuid_validation();
  return 0;
}

static void test_stream_name_parsing(void) {
  const char stream[] = "account-123+456";
  const char category[] = "account";
  msgdb_span span = msgdb_category_span(stream, strlen(stream));

  assert(span.present);
  assert(span.offset == 0U);
  assert(span.length == strlen(category));
  assert(strncmp(stream + span.offset, category, span.length) == 0);
  assert(!msgdb_is_category(stream, strlen(stream)));
  assert(msgdb_is_category(category, strlen(category)));

  span = msgdb_id_span(stream, strlen(stream));
  assert(span.present);
  assert(span.length == strlen("123+456"));
  assert(strncmp(stream + span.offset, "123+456", span.length) == 0);

  span = msgdb_cardinal_id_span(stream, strlen(stream));
  assert(span.present);
  assert(span.length == strlen("123"));
  assert(strncmp(stream + span.offset, "123", span.length) == 0);

  span = msgdb_id_span(category, strlen(category));
  assert(!span.present);

  span = msgdb_cardinal_id_span(category, strlen(category));
  assert(!span.present);
}

static void test_hash_vectors(void) {
  const unsigned char empty[] = "";
  const unsigned char stream[] = "someStream-123";
  const unsigned char category[] = "testStream";

  assert(msgdb_hash64(empty, 0U) == INT64_C(-3162216497309240828));
  assert(msgdb_hash64(stream, strlen((const char *)stream)) ==
         INT64_C(-6897031765186548867));
  assert(msgdb_hash64(category, strlen((const char *)category)) ==
         INT64_C(-8088774503009197942));
}

static void test_uuid_validation(void) {
  char out[37] = {0};

  assert(msgdb_uuid_v4_canonical("A11E9022-E741-4450-BF9C-C4CC5DDB6EA3", 36U, out));
  assert(strcmp(out, "a11e9022-e741-4450-bf9c-c4cc5ddb6ea3") == 0);

  assert(!msgdb_uuid_v4_canonical("a11e9022-e741-5450-bf9c-c4cc5ddb6ea3", 36U, out));
  assert(!msgdb_uuid_v4_canonical("a11e9022-e741-4450-7f9c-c4cc5ddb6ea3", 36U, out));
  assert(!msgdb_uuid_v4_canonical("not-a-uuid", strlen("not-a-uuid"), out));
}
