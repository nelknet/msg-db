#include <sqlite3.h>

#include "fuzz_sql_schema.h"
#include "message_db.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  MSGDB_VTAB_FUZZ_MAX_SIZE = 1024,
  MSGDB_VTAB_FUZZ_MAX_STEPS = 64,
  MSGDB_VTAB_FUZZ_MAX_BLOB = 32
};

typedef struct fuzz_input {
  const uint8_t *data;
  size_t size;
  size_t offset;
} fuzz_input;

typedef struct query_case {
  const char *sql;
  int arg_count;
} query_case;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void require_property(bool condition);
static uint8_t next_byte(fuzz_input *input);
static sqlite3_int64 next_i64(fuzz_input *input);
static sqlite3 *open_store(void);
static void close_store(sqlite3 *db);
static void exec_required(sqlite3 *db, const char *sql);
static void seed_store(sqlite3 *db);
static void bind_fuzz_value(sqlite3_stmt *stmt, int index, fuzz_input *input);
static void run_query(sqlite3 *db, const query_case *query, fuzz_input *input);

static const query_case msgdb_vtab_queries[] = {
    {"SELECT id, stream_name, type, position, global_position, data, metadata, time "
     "FROM get_stream_messages(?1)",
     1},
    {"SELECT id, stream_name, type, position, global_position, data, metadata, time "
     "FROM get_stream_messages(?1, ?2, ?3, ?4)",
     4},
    {"SELECT id, stream_name, type, position, global_position, data, metadata, time "
     "FROM get_category_messages(?1)",
     1},
    {"SELECT id, stream_name, type, position, global_position, data, metadata, time "
     "FROM get_category_messages(?1, ?2, ?3, ?4, ?5, ?6, ?7)",
     7},
    {"SELECT id, stream_name, type, position, global_position, data, metadata, time "
     "FROM get_last_stream_message(?1)",
     1},
    {"SELECT id, stream_name, type, position, global_position, data, metadata, time "
     "FROM get_last_stream_message(?1, ?2)",
     2}};

static const char *const msgdb_vtab_texts[] = {"",
                                               "account",
                                               "account-1",
                                               "account-2",
                                               "invoice",
                                               "invoice-1",
                                               "order",
                                               "order-1",
                                               "Deposited",
                                               "Withdrawn",
                                               "{\"correlationStreamName\":\"order-1\"}",
                                               "unsupported condition"};

static void require_property(bool condition) {
  if (!condition) {
    abort();
  }
}

static uint8_t next_byte(fuzz_input *input) {
  if (input->offset >= input->size) {
    return 0U;
  }

  input->offset++;
  return input->data[input->offset - 1U];
}

static sqlite3_int64 next_i64(fuzz_input *input) {
  uint64_t value = 0U;
  uint64_t magnitude = 0U;
  sqlite3_int64 signed_value = 0;

  for (size_t i = 0U; i < 8U; i++) {
    value = (value << 8U) | (uint64_t)next_byte(input);
  }

  magnitude = value & UINT64_C(0x7fffffffffffffff);
  signed_value = (sqlite3_int64)magnitude;
  return (value & (UINT64_C(1) << 63U)) != 0U ? -signed_value : signed_value;
}

static sqlite3 *open_store(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);

  require_property(rc == SQLITE_OK);
  rc = msgdb_register(db);
  require_property(rc == SQLITE_OK);
  exec_required(db, MSGDB_FUZZ_SCHEMA_SQL);
  seed_store(db);
  return db;
}

static void close_store(sqlite3 *db) {
  int rc = sqlite3_close(db);

  require_property(rc == SQLITE_OK);
}

static void exec_required(sqlite3 *db, const char *sql) {
  char *error = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &error);

  if (rc != SQLITE_OK) {
    sqlite3_free(error);
    abort();
  }

  sqlite3_free(error);
}

static void seed_store(sqlite3 *db) {
  exec_required(db, "SELECT write_message('00000000-0000-4000-8000-000000000001', "
                    "'account-1', 'Deposited', '{\"amount\":10}', "
                    "'{\"correlationStreamName\":\"order-1\"}');");
  exec_required(db, "SELECT write_message('00000000-0000-4000-8000-000000000002', "
                    "'account-1', 'Withdrawn', '{\"amount\":2}', "
                    "'{\"correlationStreamName\":\"order-1\"}', 0);");
  exec_required(db, "SELECT write_message('00000000-0000-4000-8000-000000000003', "
                    "'invoice-1', 'Issued', '{}', NULL);");
}

static void bind_fuzz_value(sqlite3_stmt *stmt, int index, fuzz_input *input) {
  uint8_t selector = next_byte(input) % 6U;
  int rc = SQLITE_OK;

  switch (selector) {
  case 0:
    rc = sqlite3_bind_null(stmt, index);
    break;
  case 1:
    rc = sqlite3_bind_int64(stmt, index, next_i64(input));
    break;
  case 2: {
    size_t text_index =
        next_byte(input) % (sizeof(msgdb_vtab_texts) / sizeof(msgdb_vtab_texts[0]));

    rc = sqlite3_bind_text(stmt, index, msgdb_vtab_texts[text_index], -1, SQLITE_STATIC);
    break;
  }
  case 3: {
    size_t remaining = input->offset <= input->size ? input->size - input->offset : 0U;
    size_t length = next_byte(input) % (MSGDB_VTAB_FUZZ_MAX_BLOB + 1U);

    if (length > remaining) {
      length = remaining;
    }
    rc = sqlite3_bind_text(stmt, index, (const char *)(input->data + input->offset),
                           (int)length, SQLITE_TRANSIENT);
    input->offset += length;
    break;
  }
  default: {
    size_t remaining = input->offset <= input->size ? input->size - input->offset : 0U;
    size_t length = next_byte(input) % (MSGDB_VTAB_FUZZ_MAX_BLOB + 1U);

    if (length > remaining) {
      length = remaining;
    }
    rc = sqlite3_bind_blob(stmt, index, input->data + input->offset, (int)length,
                           SQLITE_TRANSIENT);
    input->offset += length;
    break;
  }
  }

  require_property(rc == SQLITE_OK);
}

static void run_query(sqlite3 *db, const query_case *query, fuzz_input *input) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, query->sql, -1, &stmt, NULL);
  int final_rc = SQLITE_OK;

  require_property(rc == SQLITE_OK);

  for (int i = 0; i < query->arg_count; i++) {
    bind_fuzz_value(stmt, i + 1, input);
  }

  for (int row = 0; row < 128; row++) {
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      break;
    }

    for (int column = 0; column < sqlite3_column_count(stmt); column++) {
      switch (sqlite3_column_type(stmt, column)) {
      case SQLITE_INTEGER:
        (void)sqlite3_column_int64(stmt, column);
        break;
      case SQLITE_TEXT:
      case SQLITE_BLOB:
        (void)sqlite3_column_bytes(stmt, column);
        (void)sqlite3_column_blob(stmt, column);
        break;
      case SQLITE_FLOAT:
        (void)sqlite3_column_double(stmt, column);
        break;
      case SQLITE_NULL:
      default:
        break;
      }
    }
  }

  require_property(rc == SQLITE_DONE || rc == SQLITE_ERROR || rc == SQLITE_CONSTRAINT);
  final_rc = sqlite3_finalize(stmt);
  require_property(final_rc == SQLITE_OK || final_rc == rc);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz_input input = {data, size, 0U};
  sqlite3 *db = NULL;
  size_t steps = size < MSGDB_VTAB_FUZZ_MAX_STEPS ? size : MSGDB_VTAB_FUZZ_MAX_STEPS;

  if (size > MSGDB_VTAB_FUZZ_MAX_SIZE) {
    return 0;
  }

  db = open_store();

  for (size_t i = 0U; i < steps && input.offset < input.size; i++) {
    size_t query_index =
        next_byte(&input) % (sizeof(msgdb_vtab_queries) / sizeof(msgdb_vtab_queries[0]));

    run_query(db, &msgdb_vtab_queries[query_index], &input);
  }

  close_store(db);
  return 0;
}

#ifdef MSGDB_STANDALONE_FUZZER
int main(int argc, char **argv);

int main(int argc, char **argv) {
  static const uint8_t seed[] = {1U, 2U, 1U, 0U, 3U, 2U, 4U, 5U, 1U, 0U};

  (void)argc;
  (void)argv;
  return LLVMFuzzerTestOneInput(seed, sizeof(seed));
}
#endif
