#include <sqlite3.h>

#include "fuzz_sql_schema.h"
#include "message_db.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  MSGDB_MODEL_MAX_MESSAGES = 64,
  MSGDB_MODEL_MAX_STEPS = 128,
  MSGDB_MODEL_ID_SLOTS = 32,
  MSGDB_MODEL_STREAMS = 6,
  MSGDB_MODEL_TYPES = 4,
  MSGDB_MODEL_METADATA = 3
};

typedef struct fuzz_input {
  const uint8_t *data;
  size_t size;
  size_t offset;
} fuzz_input;

typedef struct model_message {
  int stream_index;
  int type_index;
  sqlite3_int64 global_position;
  sqlite3_int64 position;
} model_message;

typedef struct model_store {
  model_message messages[MSGDB_MODEL_MAX_MESSAGES];
  bool used_ids[MSGDB_MODEL_ID_SLOTS];
  size_t count;
} model_store;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void require_property(bool condition);
static uint8_t next_byte(fuzz_input *input);
static sqlite3_int64 choose_i64(fuzz_input *input, const sqlite3_int64 *values,
                                size_t value_count);
static sqlite3 *open_store(void);
static void close_store(sqlite3 *db);
static void exec_required(sqlite3 *db, const char *sql);
static void make_id(int slot, char out[37]);
static sqlite3_int64 model_stream_version(const model_store *model, int stream_index);
static const char *stream_name(int stream_index);
static const char *stream_category(int stream_index);
static const char *message_type(int type_index);
static const char *metadata_value(int metadata_index);
static int write_sql(sqlite3 *db, int id_slot, int stream_index, int type_index,
                     int metadata_index, bool has_expected, sqlite3_int64 expected_version,
                     sqlite3_int64 *actual_position);
static void run_write(fuzz_input *input, sqlite3 *db, model_store *model);
static void run_stream_version(fuzz_input *input, sqlite3 *db, const model_store *model);
static void run_stream_read(fuzz_input *input, sqlite3 *db, const model_store *model);
static void run_category_read(fuzz_input *input, sqlite3 *db, const model_store *model);
static void run_last_stream_read(fuzz_input *input, sqlite3 *db, const model_store *model);
static void verify_invariants(sqlite3 *db, const model_store *model);

static const char *const msgdb_model_stream_names[MSGDB_MODEL_STREAMS] = {
    "account-1", "account-2", "invoice-1", "order-1", "account-3", "invoice-2"};

static const char *const msgdb_model_stream_categories[MSGDB_MODEL_STREAMS] = {
    "account", "account", "invoice", "order", "account", "invoice"};

static const char *const msgdb_model_types[MSGDB_MODEL_TYPES] = {"Deposited", "Withdrawn",
                                                                 "Issued", "Confirmed"};

static const char *const msgdb_model_metadata[MSGDB_MODEL_METADATA] = {
    NULL, "{\"correlationStreamName\":\"order-1\"}",
    "{\"correlationStreamName\":\"account-1\"}"};

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

static sqlite3_int64 choose_i64(fuzz_input *input, const sqlite3_int64 *values,
                                size_t value_count) {
  uint8_t index = next_byte(input);

  require_property(value_count > 0U);
  return values[index % value_count];
}

static sqlite3 *open_store(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);

  require_property(rc == SQLITE_OK);
  rc = msgdb_register(db);
  require_property(rc == SQLITE_OK);
  exec_required(db, MSGDB_FUZZ_SCHEMA_SQL);
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

static void make_id(int slot, char out[37]) {
  (void)sqlite3_snprintf(37, out, "00000000-0000-4000-8000-%012x", slot);
  require_property(strlen(out) == 36U);
}

static sqlite3_int64 model_stream_version(const model_store *model, int stream_index) {
  sqlite3_int64 version = -1;

  for (size_t i = 0U; i < model->count; i++) {
    if (model->messages[i].stream_index == stream_index &&
        model->messages[i].position > version) {
      version = model->messages[i].position;
    }
  }

  return version;
}

static const char *stream_name(int stream_index) {
  require_property(stream_index >= 0 && stream_index < MSGDB_MODEL_STREAMS);
  return msgdb_model_stream_names[stream_index];
}

static const char *stream_category(int stream_index) {
  require_property(stream_index >= 0 && stream_index < MSGDB_MODEL_STREAMS);
  return msgdb_model_stream_categories[stream_index];
}

static const char *message_type(int type_index) {
  require_property(type_index >= 0 && type_index < MSGDB_MODEL_TYPES);
  return msgdb_model_types[type_index];
}

static const char *metadata_value(int metadata_index) {
  require_property(metadata_index >= 0 && metadata_index < MSGDB_MODEL_METADATA);
  return msgdb_model_metadata[metadata_index];
}

static int write_sql(sqlite3 *db, int id_slot, int stream_index, int type_index,
                     int metadata_index, bool has_expected, sqlite3_int64 expected_version,
                     sqlite3_int64 *actual_position) {
  const char *sql = has_expected ? "SELECT write_message(?1, ?2, ?3, ?4, ?5, ?6)"
                                 : "SELECT write_message(?1, ?2, ?3, ?4, ?5)";
  sqlite3_stmt *stmt = NULL;
  const char *metadata = metadata_value(metadata_index);
  char id[37] = {0};
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

  require_property(rc == SQLITE_OK);
  make_id(id_slot, id);

  rc = sqlite3_bind_text(stmt, 1, id, 36, SQLITE_TRANSIENT);
  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_text(stmt, 2, stream_name(stream_index), -1, SQLITE_STATIC);
  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_text(stmt, 3, message_type(type_index), -1, SQLITE_STATIC);
  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_text(stmt, 4, "{}", -1, SQLITE_STATIC);
  require_property(rc == SQLITE_OK);
  if (metadata == NULL) {
    rc = sqlite3_bind_null(stmt, 5);
  } else {
    rc = sqlite3_bind_text(stmt, 5, metadata, -1, SQLITE_STATIC);
  }
  require_property(rc == SQLITE_OK);

  if (has_expected) {
    rc = sqlite3_bind_int64(stmt, 6, expected_version);
    require_property(rc == SQLITE_OK);
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    int finalize_rc = SQLITE_OK;

    *actual_position = sqlite3_column_int64(stmt, 0);
    finalize_rc = sqlite3_finalize(stmt);
    require_property(finalize_rc == SQLITE_OK);
    return SQLITE_OK;
  }

  {
    int step_rc = rc;
    int finalize_rc = sqlite3_finalize(stmt);

    require_property(finalize_rc == SQLITE_OK || finalize_rc == step_rc);
    return step_rc;
  }
}

static void run_write(fuzz_input *input, sqlite3 *db, model_store *model) {
  int id_slot = (int)(next_byte(input) % MSGDB_MODEL_ID_SLOTS);
  int stream_index = (int)(next_byte(input) % MSGDB_MODEL_STREAMS);
  int type_index = (int)(next_byte(input) % MSGDB_MODEL_TYPES);
  int metadata_index = (int)(next_byte(input) % MSGDB_MODEL_METADATA);
  int expected_mode = (int)(next_byte(input) % 4U);
  sqlite3_int64 current_version = model_stream_version(model, stream_index);
  sqlite3_int64 expected_version = 0;
  sqlite3_int64 actual_position = -1;
  bool has_expected = expected_mode != 0;
  bool should_succeed = false;
  int rc = SQLITE_OK;

  if (model->count >= MSGDB_MODEL_MAX_MESSAGES) {
    return;
  }

  if (expected_mode == 1) {
    expected_version = current_version;
  } else if (expected_mode == 2) {
    expected_version = current_version + 1;
  } else {
    expected_version = -1;
  }

  should_succeed =
      !model->used_ids[id_slot] && (!has_expected || expected_version == current_version);

  rc = write_sql(db, id_slot, stream_index, type_index, metadata_index, has_expected,
                 expected_version, &actual_position);

  if (!should_succeed) {
    require_property(rc != SQLITE_OK);
    return;
  }

  require_property(rc == SQLITE_OK);
  require_property(actual_position == current_version + 1);

  model->used_ids[id_slot] = true;
  model->messages[model->count].stream_index = stream_index;
  model->messages[model->count].type_index = type_index;
  model->messages[model->count].global_position = (sqlite3_int64)model->count + 1;
  model->messages[model->count].position = actual_position;
  model->count++;
}

static void run_stream_version(fuzz_input *input, sqlite3 *db, const model_store *model) {
  sqlite3_stmt *stmt = NULL;
  int stream_index = (int)(next_byte(input) % MSGDB_MODEL_STREAMS);
  sqlite3_int64 version = model_stream_version(model, stream_index);
  int rc = sqlite3_prepare_v2(db, "SELECT stream_version(?1)", -1, &stmt, NULL);

  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_text(stmt, 1, stream_name(stream_index), -1, SQLITE_STATIC);
  require_property(rc == SQLITE_OK);
  rc = sqlite3_step(stmt);
  require_property(rc == SQLITE_ROW);

  if (version < 0) {
    require_property(sqlite3_column_type(stmt, 0) == SQLITE_NULL);
  } else {
    require_property(sqlite3_column_int64(stmt, 0) == version);
  }

  rc = sqlite3_finalize(stmt);
  require_property(rc == SQLITE_OK);
}

static void run_stream_read(fuzz_input *input, sqlite3 *db, const model_store *model) {
  static const sqlite3_int64 positions[] = {-1, 0, 1, 2, 8};
  static const sqlite3_int64 batch_sizes[] = {-2, -1, 0, 1, 2, 1000};
  sqlite3_int64 expected_positions[MSGDB_MODEL_MAX_MESSAGES] = {0};
  sqlite3_stmt *stmt = NULL;
  int stream_index = (int)(next_byte(input) % MSGDB_MODEL_STREAMS);
  sqlite3_int64 position =
      choose_i64(input, positions, sizeof(positions) / sizeof(positions[0]));
  sqlite3_int64 batch_size =
      choose_i64(input, batch_sizes, sizeof(batch_sizes) / sizeof(batch_sizes[0]));
  bool expect_error = batch_size < -1;
  size_t expected_count = 0U;
  size_t actual_count = 0U;
  int rc = sqlite3_prepare_v2(db, "SELECT position FROM get_stream_messages(?1, ?2, ?3)",
                              -1, &stmt, NULL);

  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_text(stmt, 1, stream_name(stream_index), -1, SQLITE_STATIC);
  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_int64(stmt, 2, position);
  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_int64(stmt, 3, batch_size);
  require_property(rc == SQLITE_OK);

  if (!expect_error) {
    for (size_t i = 0U; i < model->count; i++) {
      bool under_limit = batch_size < 0 || expected_count < (size_t)batch_size;

      if (model->messages[i].stream_index == stream_index &&
          model->messages[i].position >= position && under_limit) {
        expected_positions[expected_count] = model->messages[i].position;
        expected_count++;
      }
    }
  }

  for (;;) {
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      require_property(!expect_error);
      require_property(actual_count < expected_count);
      require_property(sqlite3_column_int64(stmt, 0) == expected_positions[actual_count]);
      actual_count++;
      continue;
    }
    break;
  }

  if (expect_error) {
    require_property(rc != SQLITE_DONE);
  } else {
    require_property(rc == SQLITE_DONE);
    require_property(actual_count == expected_count);
  }

  {
    int finalize_rc = sqlite3_finalize(stmt);

    require_property(finalize_rc == SQLITE_OK || expect_error);
  }
}

static void run_category_read(fuzz_input *input, sqlite3 *db, const model_store *model) {
  static const sqlite3_int64 positions[] = {-1, 1, 2, 8};
  static const sqlite3_int64 batch_sizes[] = {-2, -1, 0, 1, 3, 1000};
  static const char *const categories[] = {"account", "invoice", "order"};
  sqlite3_int64 expected_globals[MSGDB_MODEL_MAX_MESSAGES] = {0};
  sqlite3_stmt *stmt = NULL;
  const char *category = categories[next_byte(input) % 3U];
  sqlite3_int64 position =
      choose_i64(input, positions, sizeof(positions) / sizeof(positions[0]));
  sqlite3_int64 batch_size =
      choose_i64(input, batch_sizes, sizeof(batch_sizes) / sizeof(batch_sizes[0]));
  bool expect_error = batch_size < -1;
  size_t expected_count = 0U;
  size_t actual_count = 0U;
  int rc = sqlite3_prepare_v2(
      db, "SELECT global_position FROM get_category_messages(?1, ?2, ?3)", -1, &stmt, NULL);

  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_text(stmt, 1, category, -1, SQLITE_STATIC);
  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_int64(stmt, 2, position);
  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_int64(stmt, 3, batch_size);
  require_property(rc == SQLITE_OK);

  if (!expect_error) {
    for (size_t i = 0U; i < model->count; i++) {
      bool under_limit = batch_size < 0 || expected_count < (size_t)batch_size;

      if (strcmp(stream_category(model->messages[i].stream_index), category) == 0 &&
          model->messages[i].global_position >= position && under_limit) {
        expected_globals[expected_count] = model->messages[i].global_position;
        expected_count++;
      }
    }
  }

  for (;;) {
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      require_property(!expect_error);
      require_property(actual_count < expected_count);
      require_property(sqlite3_column_int64(stmt, 0) == expected_globals[actual_count]);
      actual_count++;
      continue;
    }
    break;
  }

  if (expect_error) {
    require_property(rc != SQLITE_DONE);
  } else {
    require_property(rc == SQLITE_DONE);
    require_property(actual_count == expected_count);
  }

  {
    int finalize_rc = sqlite3_finalize(stmt);

    require_property(finalize_rc == SQLITE_OK || expect_error);
  }
}

static void run_last_stream_read(fuzz_input *input, sqlite3 *db, const model_store *model) {
  sqlite3_stmt *stmt = NULL;
  int stream_index = (int)(next_byte(input) % MSGDB_MODEL_STREAMS);
  int type_index = (int)(next_byte(input) % MSGDB_MODEL_TYPES);
  bool has_type = (next_byte(input) & 1U) != 0U;
  sqlite3_int64 expected_position = -1;
  int rc =
      sqlite3_prepare_v2(db,
                         has_type ? "SELECT position FROM get_last_stream_message(?1, ?2)"
                                  : "SELECT position FROM get_last_stream_message(?1)",
                         -1, &stmt, NULL);

  require_property(rc == SQLITE_OK);
  rc = sqlite3_bind_text(stmt, 1, stream_name(stream_index), -1, SQLITE_STATIC);
  require_property(rc == SQLITE_OK);
  if (has_type) {
    rc = sqlite3_bind_text(stmt, 2, message_type(type_index), -1, SQLITE_STATIC);
    require_property(rc == SQLITE_OK);
  }

  for (size_t i = 0U; i < model->count; i++) {
    if (model->messages[i].stream_index == stream_index &&
        (!has_type || model->messages[i].type_index == type_index) &&
        model->messages[i].position > expected_position) {
      expected_position = model->messages[i].position;
    }
  }

  rc = sqlite3_step(stmt);
  if (expected_position < 0) {
    require_property(rc == SQLITE_DONE);
  } else {
    require_property(rc == SQLITE_ROW);
    require_property(sqlite3_column_int64(stmt, 0) == expected_position);
    rc = sqlite3_step(stmt);
    require_property(rc == SQLITE_DONE);
  }

  rc = sqlite3_finalize(stmt);
  require_property(rc == SQLITE_OK);
}

static void verify_invariants(sqlite3 *db, const model_store *model) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(
      db,
      "SELECT COUNT(*), COALESCE(MIN(global_position), 0), "
      "COALESCE(MAX(global_position), 0), COUNT(DISTINCT global_position), "
      "COUNT(DISTINCT id) FROM messages",
      -1, &stmt, NULL);

  require_property(rc == SQLITE_OK);
  rc = sqlite3_step(stmt);
  require_property(rc == SQLITE_ROW);
  require_property(sqlite3_column_int64(stmt, 0) == (sqlite3_int64)model->count);
  require_property(sqlite3_column_int64(stmt, 1) == (model->count == 0U ? 0 : 1));
  require_property(sqlite3_column_int64(stmt, 2) == (sqlite3_int64)model->count);
  require_property(sqlite3_column_int64(stmt, 3) == (sqlite3_int64)model->count);
  require_property(sqlite3_column_int64(stmt, 4) == (sqlite3_int64)model->count);
  rc = sqlite3_finalize(stmt);
  require_property(rc == SQLITE_OK);

  for (int stream_index = 0; stream_index < MSGDB_MODEL_STREAMS; stream_index++) {
    sqlite3_int64 expected_position = 0;

    rc = sqlite3_prepare_v2(
        db, "SELECT position FROM messages WHERE stream_name = ?1 ORDER BY position", -1,
        &stmt, NULL);
    require_property(rc == SQLITE_OK);
    rc = sqlite3_bind_text(stmt, 1, stream_name(stream_index), -1, SQLITE_STATIC);
    require_property(rc == SQLITE_OK);

    for (;;) {
      rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        require_property(sqlite3_column_int64(stmt, 0) == expected_position);
        expected_position++;
        continue;
      }
      break;
    }

    require_property(rc == SQLITE_DONE);
    require_property(expected_position == model_stream_version(model, stream_index) + 1);
    rc = sqlite3_finalize(stmt);
    require_property(rc == SQLITE_OK);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fuzz_input input = {data, size, 0U};
  model_store model;
  sqlite3 *db = NULL;
  size_t steps = size < MSGDB_MODEL_MAX_STEPS ? size : MSGDB_MODEL_MAX_STEPS;

  if (size > 1024U) {
    return 0;
  }

  memset(&model, 0, sizeof(model));
  db = open_store();

  for (size_t i = 0U; i < steps && input.offset < input.size; i++) {
    switch (next_byte(&input) % 5U) {
    case 0:
      run_write(&input, db, &model);
      break;
    case 1:
      run_stream_version(&input, db, &model);
      break;
    case 2:
      run_stream_read(&input, db, &model);
      break;
    case 3:
      run_category_read(&input, db, &model);
      break;
    default:
      run_last_stream_read(&input, db, &model);
      break;
    }

    verify_invariants(db, &model);
  }

  close_store(db);
  return 0;
}

#ifdef MSGDB_STANDALONE_FUZZER
int main(int argc, char **argv);

int main(int argc, char **argv) {
  static const uint8_t seed[] = {0U, 0U, 0U, 0U, 1U, 1U, 2U, 0U, 0U,
                                 5U, 3U, 0U, 1U, 5U, 4U, 0U, 0U};

  (void)argc;
  (void)argv;
  return LLVMFuzzerTestOneInput(seed, sizeof(seed));
}
#endif
