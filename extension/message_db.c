#include "msgdb_core.h"

#ifdef MSGDB_STATIC
#include <sqlite3.h>
#define SQLITE_EXTENSION_INIT1
#define SQLITE_EXTENSION_INIT2(api) (void)(api)
#else
#include <sqlite3ext.h>
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "message_db.h"

SQLITE_EXTENSION_INIT1

#if defined(_WIN32) && !defined(MSGDB_STATIC)
#define MSGDB_EXTENSION_EXPORT __declspec(dllexport)
#else
#define MSGDB_EXTENSION_EXPORT
#endif

typedef enum msgdb_vtab_kind {
  MSGDB_VTAB_STREAM = 1,
  MSGDB_VTAB_CATEGORY = 2,
  MSGDB_VTAB_LAST_STREAM = 3
} msgdb_vtab_kind;

typedef struct msgdb_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  msgdb_vtab_kind kind;
} msgdb_vtab;

typedef struct msgdb_cursor {
  sqlite3_vtab_cursor base;
  sqlite3_stmt *stmt;
  bool eof;
} msgdb_cursor;

#ifndef MSGDB_STATIC
MSGDB_EXTENSION_EXPORT int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg,
                                                  const sqlite3_api_routines *pApi);
MSGDB_EXTENSION_EXPORT int sqlite3_messagedb_init(sqlite3 *db, char **pzErrMsg,
                                                  const sqlite3_api_routines *pApi);
#endif

static void category_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void is_category_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void id_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void cardinal_id_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hash64_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void message_store_version_func(sqlite3_context *ctx, int argc,
                                       sqlite3_value **argv);
static void gen_random_uuid_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void stream_version_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void acquire_lock_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void write_message_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static int msgdb_vtab_connect(sqlite3 *db, void *pAux, int argc, const char *const *argv,
                              sqlite3_vtab **ppVtab, char **pzErr);
static int msgdb_vtab_disconnect(sqlite3_vtab *pVtab);
static int msgdb_vtab_best_index(sqlite3_vtab *pVTab, sqlite3_index_info *pIdxInfo);
static int msgdb_vtab_open(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCursor);
static int msgdb_vtab_close(sqlite3_vtab_cursor *cur);
static int msgdb_vtab_filter(sqlite3_vtab_cursor *pVtabCursor, int idxNum,
                             const char *idxStr, int argc, sqlite3_value **argv);
static int msgdb_vtab_next(sqlite3_vtab_cursor *cur);
static int msgdb_vtab_eof(sqlite3_vtab_cursor *cur);
static int msgdb_vtab_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int column);
static int msgdb_vtab_rowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid);

static int register_function(sqlite3 *db, const char *name, int argc, int flags,
                             void (*func)(sqlite3_context *, int, sqlite3_value **));
static void result_text_span(sqlite3_context *ctx, const char *value, msgdb_span span);
static bool value_text(sqlite3_value *value, const char **text, int *bytes);
static bool value_text_size(sqlite3_value *value, const char **text, size_t *bytes);
static int query_stream_version(sqlite3 *db, const char *stream_name, int stream_name_bytes,
                                bool *found, sqlite3_int64 *version);
static int sql_exec(sqlite3 *db, const char *sql, char **error);
static void set_context_sql_error(sqlite3_context *ctx, const char *prefix, int rc);
static int vtab_error(sqlite3_vtab *vtab, const char *format, ...);
static bool vtab_value_text(sqlite3_value *value, const char **text, int *bytes);
static bool optional_i64(sqlite3_value *value, sqlite3_int64 default_value,
                         sqlite3_int64 *result);
static bool optional_is_present(sqlite3_value *value);
static int bind_text_value(sqlite3_stmt *stmt, int index, const char *text, int bytes);
static int cursor_step(msgdb_cursor *cursor);
static int filter_stream(msgdb_cursor *cursor, int idxNum, int argc, sqlite3_value **argv);
static int filter_category(msgdb_cursor *cursor, int idxNum, int argc,
                           sqlite3_value **argv);
static int filter_last_stream(msgdb_cursor *cursor, int idxNum, int argc,
                              sqlite3_value **argv);

static const msgdb_vtab_kind msgdb_stream_vtab_kind = MSGDB_VTAB_STREAM;
static const msgdb_vtab_kind msgdb_category_vtab_kind = MSGDB_VTAB_CATEGORY;
static const msgdb_vtab_kind msgdb_last_stream_vtab_kind = MSGDB_VTAB_LAST_STREAM;

static const sqlite3_module msgdb_module = {.iVersion = 0,
                                            .xCreate = NULL,
                                            .xConnect = msgdb_vtab_connect,
                                            .xBestIndex = msgdb_vtab_best_index,
                                            .xDisconnect = msgdb_vtab_disconnect,
                                            .xDestroy = NULL,
                                            .xOpen = msgdb_vtab_open,
                                            .xClose = msgdb_vtab_close,
                                            .xFilter = msgdb_vtab_filter,
                                            .xNext = msgdb_vtab_next,
                                            .xEof = msgdb_vtab_eof,
                                            .xColumn = msgdb_vtab_column,
                                            .xRowid = msgdb_vtab_rowid};

#ifndef MSGDB_STATIC
MSGDB_EXTENSION_EXPORT int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg,
                                                  const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;
  return msgdb_register(db);
}

MSGDB_EXTENSION_EXPORT int sqlite3_messagedb_init(sqlite3 *db, char **pzErrMsg,
                                                  const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;
  return msgdb_register(db);
}
#endif

int msgdb_register(sqlite3 *db) {
  int rc = SQLITE_OK;

  rc = register_function(db, "category", 1,
                         SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                         category_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(db, "is_category", 1,
                         SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                         is_category_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(db, "id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                         id_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(db, "cardinal_id", 1,
                         SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                         cardinal_id_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(
      db, "hash_64", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, hash64_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(db, "message_store_version", 0,
                         SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                         message_store_version_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(db, "gen_random_uuid", 0, SQLITE_UTF8, gen_random_uuid_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(db, "stream_version", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY,
                         stream_version_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(db, "acquire_lock", 1,
                         SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                         acquire_lock_func);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = register_function(db, "write_message", -1, SQLITE_UTF8 | SQLITE_DIRECTONLY,
                         write_message_func);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = sqlite3_create_module(db, "get_stream_messages", &msgdb_module,
                             (void *)&msgdb_stream_vtab_kind);
  if (rc != SQLITE_OK) {
    return rc;
  }
  rc = sqlite3_create_module(db, "get_category_messages", &msgdb_module,
                             (void *)&msgdb_category_vtab_kind);
  if (rc != SQLITE_OK) {
    return rc;
  }
  return sqlite3_create_module(db, "get_last_stream_message", &msgdb_module,
                               (void *)&msgdb_last_stream_vtab_kind);
}

static int register_function(sqlite3 *db, const char *name, int argc, int flags,
                             void (*func)(sqlite3_context *, int, sqlite3_value **)) {
  return sqlite3_create_function_v2(db, name, argc, flags, NULL, func, NULL, NULL, NULL);
}

static void category_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *text = NULL;
  size_t bytes = 0U;

  (void)argc;

  if (!value_text_size(argv[0], &text, &bytes)) {
    sqlite3_result_null(ctx);
    return;
  }

  result_text_span(ctx, text, msgdb_category_span(text, bytes));
}

static void is_category_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *text = NULL;
  size_t bytes = 0U;

  (void)argc;

  if (!value_text_size(argv[0], &text, &bytes)) {
    sqlite3_result_null(ctx);
    return;
  }

  sqlite3_result_int(ctx, msgdb_is_category(text, bytes) ? 1 : 0);
}

static void id_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *text = NULL;
  size_t bytes = 0U;
  msgdb_span span;

  (void)argc;

  if (!value_text_size(argv[0], &text, &bytes)) {
    sqlite3_result_null(ctx);
    return;
  }

  span = msgdb_id_span(text, bytes);
  if (!span.present) {
    sqlite3_result_null(ctx);
    return;
  }

  result_text_span(ctx, text, span);
}

static void cardinal_id_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const char *text = NULL;
  size_t bytes = 0U;
  msgdb_span span;

  (void)argc;

  if (!value_text_size(argv[0], &text, &bytes)) {
    sqlite3_result_null(ctx);
    return;
  }

  span = msgdb_cardinal_id_span(text, bytes);
  if (!span.present) {
    sqlite3_result_null(ctx);
    return;
  }

  result_text_span(ctx, text, span);
}

static void hash64_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const unsigned char *text = NULL;
  int bytes = 0;

  (void)argc;

  if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  text = sqlite3_value_text(argv[0]);
  bytes = sqlite3_value_bytes(argv[0]);
  if (text == NULL || bytes < 0) {
    sqlite3_result_error_nomem(ctx);
    return;
  }

  sqlite3_result_int64(ctx, msgdb_hash64(text, (size_t)bytes));
}

static void message_store_version_func(sqlite3_context *ctx, int argc,
                                       sqlite3_value **argv) {
  (void)argc;
  (void)argv;
  sqlite3_result_text(ctx, MSGDB_VERSION, -1, SQLITE_STATIC);
}

static void gen_random_uuid_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  unsigned char bytes[16] = {0U};
  char uuid[37] = {0};

  (void)argc;
  (void)argv;

  sqlite3_randomness((int)sizeof(bytes), bytes);
  msgdb_uuid_v4_from_bytes(bytes, uuid);
  sqlite3_result_text(ctx, uuid, 36, SQLITE_TRANSIENT);
}

static void stream_version_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *stream_name = NULL;
  int stream_name_bytes = 0;
  bool found = false;
  sqlite3_int64 version = 0;
  int rc = SQLITE_OK;

  (void)argc;

  if (!value_text(argv[0], &stream_name, &stream_name_bytes)) {
    sqlite3_result_null(ctx);
    return;
  }

  rc = query_stream_version(db, stream_name, stream_name_bytes, &found, &version);
  if (rc != SQLITE_OK) {
    set_context_sql_error(ctx, "stream_version", rc);
    return;
  }

  if (!found) {
    sqlite3_result_null(ctx);
    return;
  }

  sqlite3_result_int64(ctx, version);
}

static void acquire_lock_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  const unsigned char *text = NULL;
  int bytes = 0;
  msgdb_span category;

  (void)argc;

  if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  text = sqlite3_value_text(argv[0]);
  bytes = sqlite3_value_bytes(argv[0]);
  if (text == NULL || bytes < 0) {
    sqlite3_result_error_nomem(ctx);
    return;
  }

  category = msgdb_category_span((const char *)text, (size_t)bytes);
  sqlite3_result_int64(ctx, msgdb_hash64(text + category.offset, category.length));
}

static void write_message_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  sqlite3_stmt *insert_stmt = NULL;
  const char *id_text = NULL;
  const char *stream_name = NULL;
  const char *type = NULL;
  int id_bytes = 0;
  int stream_name_bytes = 0;
  int type_bytes = 0;
  char canonical_id[37] = {0};
  bool found = false;
  bool began = false;
  bool committed = false;
  bool has_expected_version = false;
  sqlite3_int64 current_version = -1;
  sqlite3_int64 expected_version = 0;
  sqlite3_int64 next_position = 0;
  int rc = SQLITE_OK;
  char *exec_error = NULL;

  if (argc < 4 || argc > 6) {
    sqlite3_result_error(ctx,
                         "write_message expects id, stream_name, type, data, optional "
                         "metadata, and optional expected_version",
                         -1);
    return;
  }

  if (!value_text(argv[0], &id_text, &id_bytes) ||
      !value_text(argv[1], &stream_name, &stream_name_bytes) ||
      !value_text(argv[2], &type, &type_bytes)) {
    sqlite3_result_error(ctx, "write_message id, stream_name, and type are required", -1);
    return;
  }

  if (!msgdb_uuid_v4_canonical(id_text, (size_t)id_bytes, canonical_id)) {
    sqlite3_result_error(ctx, "write_message id must be a UUIDv4 string", -1);
    return;
  }

  if (argc >= 6 && sqlite3_value_type(argv[5]) != SQLITE_NULL) {
    if (sqlite3_value_type(argv[5]) != SQLITE_INTEGER) {
      sqlite3_result_error(ctx, "write_message expected_version must be an integer or NULL",
                           -1);
      return;
    }
    has_expected_version = true;
    expected_version = sqlite3_value_int64(argv[5]);
  }

  if (sqlite3_get_autocommit(db) != 0) {
    rc = sql_exec(db, "BEGIN IMMEDIATE", &exec_error);
    if (rc != SQLITE_OK) {
      sqlite3_result_error(ctx, exec_error != NULL ? exec_error : sqlite3_errmsg(db), -1);
      sqlite3_free(exec_error);
      return;
    }
    began = true;
  }

  rc = query_stream_version(db, stream_name, stream_name_bytes, &found, &current_version);
  if (rc != SQLITE_OK) {
    set_context_sql_error(ctx, "write_message stream_version", rc);
    goto cleanup;
  }
  if (!found) {
    current_version = -1;
  }

  if (has_expected_version && expected_version != current_version) {
    sqlite3_result_error(ctx, "Wrong expected version", -1);
    rc = SQLITE_MISMATCH;
    goto cleanup;
  }

  next_position = current_version + 1;

  rc = sqlite3_prepare_v2(
      db,
      "INSERT INTO messages (id, stream_name, position, type, data, metadata) "
      "VALUES (?1, ?2, ?3, ?4, CASE WHEN ?5 IS NULL THEN NULL ELSE json(?5) END, "
      "CASE WHEN ?6 IS NULL THEN NULL ELSE json(?6) END)",
      -1, &insert_stmt, NULL);
  if (rc != SQLITE_OK) {
    set_context_sql_error(ctx, "write_message insert prepare", rc);
    goto cleanup;
  }

  rc = sqlite3_bind_text(insert_stmt, 1, canonical_id, 36, SQLITE_TRANSIENT);
  if (rc == SQLITE_OK) {
    rc = bind_text_value(insert_stmt, 2, stream_name, stream_name_bytes);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(insert_stmt, 3, next_position);
  }
  if (rc == SQLITE_OK) {
    rc = bind_text_value(insert_stmt, 4, type, type_bytes);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_value(insert_stmt, 5, argv[3]);
  }
  if (rc == SQLITE_OK) {
    if (argc < 5) {
      rc = sqlite3_bind_null(insert_stmt, 6);
    } else {
      rc = sqlite3_bind_value(insert_stmt, 6, argv[4]);
    }
  }
  if (rc != SQLITE_OK) {
    set_context_sql_error(ctx, "write_message insert bind", rc);
    goto cleanup;
  }

  rc = sqlite3_step(insert_stmt);
  if (rc != SQLITE_DONE) {
    set_context_sql_error(ctx, "write_message insert", rc);
    goto cleanup;
  }
  rc = SQLITE_OK;

  if (began) {
    rc = sql_exec(db, "COMMIT", &exec_error);
    if (rc != SQLITE_OK) {
      sqlite3_result_error(ctx, exec_error != NULL ? exec_error : sqlite3_errmsg(db), -1);
      sqlite3_free(exec_error);
      exec_error = NULL;
      goto cleanup;
    }
    committed = true;
  }

  sqlite3_result_int64(ctx, next_position);

cleanup:
  if (insert_stmt != NULL) {
    int finalize_rc = sqlite3_finalize(insert_stmt);
    if (rc == SQLITE_OK && finalize_rc != SQLITE_OK) {
      rc = finalize_rc;
      set_context_sql_error(ctx, "write_message insert finalize", rc);
    }
  }

  if (began && !committed) {
    sqlite3_free(exec_error);
    exec_error = NULL;
    (void)sql_exec(db, "ROLLBACK", &exec_error);
    sqlite3_free(exec_error);
  }
}

// cppcheck-suppress constParameterCallback
static int msgdb_vtab_connect(sqlite3 *db, void *pAux, int argc, const char *const *argv,
                              sqlite3_vtab **ppVtab, char **pzErr) {
  msgdb_vtab *vtab = NULL;
  const msgdb_vtab_kind *kind = (const msgdb_vtab_kind *)pAux;
  const char *schema = NULL;
  int rc = SQLITE_OK;

  (void)argc;
  (void)argv;
  (void)pzErr;

  if (kind == NULL) {
    return SQLITE_MISUSE;
  }

  switch (*kind) {
  case MSGDB_VTAB_STREAM:
    schema = "CREATE TABLE x(id TEXT, stream_name TEXT, type TEXT, position INTEGER, "
             "global_position INTEGER, data TEXT, metadata TEXT, time TEXT, "
             "arg_stream_name HIDDEN, arg_position HIDDEN, arg_batch_size HIDDEN, "
             "arg_condition HIDDEN)";
    break;
  case MSGDB_VTAB_CATEGORY:
    schema = "CREATE TABLE x(id TEXT, stream_name TEXT, type TEXT, position INTEGER, "
             "global_position INTEGER, data TEXT, metadata TEXT, time TEXT, "
             "arg_category HIDDEN, arg_position HIDDEN, arg_batch_size HIDDEN, "
             "arg_correlation HIDDEN, arg_consumer_group_member HIDDEN, "
             "arg_consumer_group_size HIDDEN, arg_condition HIDDEN)";
    break;
  case MSGDB_VTAB_LAST_STREAM:
    schema = "CREATE TABLE x(id TEXT, stream_name TEXT, type TEXT, position INTEGER, "
             "global_position INTEGER, data TEXT, metadata TEXT, time TEXT, "
             "arg_stream_name HIDDEN, arg_type HIDDEN)";
    break;
  }

  rc = sqlite3_declare_vtab(db, schema);
  if (rc != SQLITE_OK) {
    return rc;
  }

  vtab = sqlite3_malloc64((sqlite3_uint64)sizeof(*vtab));
  if (vtab == NULL) {
    return SQLITE_NOMEM;
  }

  memset(vtab, 0, sizeof(*vtab));
  vtab->db = db;
  vtab->kind = *kind;
  *ppVtab = &vtab->base;

  return SQLITE_OK;
}

static int msgdb_vtab_disconnect(sqlite3_vtab *pVtab) {
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

static int msgdb_vtab_best_index(sqlite3_vtab *pVTab, sqlite3_index_info *pIdxInfo) {
  const msgdb_vtab *vtab = (const msgdb_vtab *)pVTab;
  int argv_index = 1;
  int idx_num = 0;
  int hidden_start = 8;
  int hidden_end = 0;
  const char *required_parameter = NULL;
  bool required_seen = false;
  bool required_usable = false;

  switch (vtab->kind) {
  case MSGDB_VTAB_STREAM:
    hidden_end = 11;
    required_parameter = "stream name";
    break;
  case MSGDB_VTAB_CATEGORY:
    hidden_end = 14;
    required_parameter = "category";
    break;
  case MSGDB_VTAB_LAST_STREAM:
    hidden_end = 9;
    required_parameter = "stream name";
    break;
  }

  for (int i = 0; i < pIdxInfo->nConstraint; i++) {
    const struct sqlite3_index_constraint *constraint = &pIdxInfo->aConstraint[i];

    if (constraint->iColumn != hidden_start ||
        constraint->op != SQLITE_INDEX_CONSTRAINT_EQ) {
      continue;
    }

    required_seen = true;
    if (constraint->usable != 0) {
      required_usable = true;
    }
    break;
  }

  if (!required_seen) {
    return vtab_error(pVTab, "Message DB table-valued function requires a %s",
                      required_parameter);
  }
  if (!required_usable) {
    return SQLITE_CONSTRAINT;
  }

  for (int column = hidden_start; column <= hidden_end; column++) {
    for (int i = 0; i < pIdxInfo->nConstraint; i++) {
      const struct sqlite3_index_constraint *constraint = &pIdxInfo->aConstraint[i];

      if (constraint->usable == 0 || constraint->iColumn != column ||
          constraint->op != SQLITE_INDEX_CONSTRAINT_EQ) {
        continue;
      }

      pIdxInfo->aConstraintUsage[i].argvIndex = argv_index;
      pIdxInfo->aConstraintUsage[i].omit = 1;
      idx_num |= 1 << (column - hidden_start);
      argv_index++;
      break;
    }
  }

  pIdxInfo->idxNum = idx_num;
  pIdxInfo->estimatedCost = (idx_num & 1) != 0 ? 20.0 : 1000000.0;
  pIdxInfo->estimatedRows = (idx_num & 1) != 0 ? 1000 : 1000000;
  return SQLITE_OK;
}

static int msgdb_vtab_open(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCursor) {
  msgdb_cursor *cursor = NULL;

  (void)pVTab;

  cursor = sqlite3_malloc64((sqlite3_uint64)sizeof(*cursor));
  if (cursor == NULL) {
    return SQLITE_NOMEM;
  }

  memset(cursor, 0, sizeof(*cursor));
  cursor->eof = true;
  *ppCursor = &cursor->base;
  return SQLITE_OK;
}

static int msgdb_vtab_close(sqlite3_vtab_cursor *cur) {
  msgdb_cursor *cursor = (msgdb_cursor *)cur;

  if (cursor->stmt != NULL) {
    (void)sqlite3_finalize(cursor->stmt);
  }
  sqlite3_free(cursor);
  return SQLITE_OK;
}

static int msgdb_vtab_filter(sqlite3_vtab_cursor *pVtabCursor, int idxNum,
                             const char *idxStr, int argc, sqlite3_value **argv) {
  msgdb_cursor *cursor = (msgdb_cursor *)pVtabCursor;
  msgdb_vtab *vtab = (msgdb_vtab *)pVtabCursor->pVtab;

  (void)idxStr;

  if (cursor->stmt != NULL) {
    (void)sqlite3_finalize(cursor->stmt);
    cursor->stmt = NULL;
  }
  cursor->eof = true;

  switch (vtab->kind) {
  case MSGDB_VTAB_STREAM:
    return filter_stream(cursor, idxNum, argc, argv);
  case MSGDB_VTAB_CATEGORY:
    return filter_category(cursor, idxNum, argc, argv);
  case MSGDB_VTAB_LAST_STREAM:
    return filter_last_stream(cursor, idxNum, argc, argv);
  }

  return vtab_error(&vtab->base, "unknown Message DB virtual table");
}

static int msgdb_vtab_next(sqlite3_vtab_cursor *cur) {
  return cursor_step((msgdb_cursor *)cur);
}

static int msgdb_vtab_eof(sqlite3_vtab_cursor *cur) {
  msgdb_cursor *cursor = (msgdb_cursor *)cur;
  return cursor->eof ? 1 : 0;
}

static int msgdb_vtab_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int column) {
  msgdb_cursor *cursor = (msgdb_cursor *)cur;

  if (column >= 0 && column < 8 && cursor->stmt != NULL) {
    sqlite3_result_value(ctx, sqlite3_column_value(cursor->stmt, column));
    return SQLITE_OK;
  }

  sqlite3_result_null(ctx);
  return SQLITE_OK;
}

static int msgdb_vtab_rowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid) {
  msgdb_cursor *cursor = (msgdb_cursor *)cur;

  if (cursor->stmt == NULL) {
    *pRowid = 0;
    return SQLITE_OK;
  }

  *pRowid = sqlite3_column_int64(cursor->stmt, 4);
  return SQLITE_OK;
}

static int filter_stream(msgdb_cursor *cursor, int idxNum, int argc, sqlite3_value **argv) {
  static const char sql[] =
      "SELECT id, stream_name, type, position, global_position, data, metadata, time "
      "FROM messages "
      "WHERE stream_name = ?1 AND position >= ?2 "
      "ORDER BY position ASC "
      "LIMIT ?3";
  msgdb_vtab *vtab = (msgdb_vtab *)cursor->base.pVtab;
  const char *stream_name = NULL;
  int stream_name_bytes = 0;
  sqlite3_int64 position = 0;
  sqlite3_int64 batch_size = 1000;
  int arg = 0;
  int rc = SQLITE_OK;

  if ((idxNum & 1) == 0 || arg >= argc ||
      !vtab_value_text(argv[arg], &stream_name, &stream_name_bytes)) {
    return vtab_error(&vtab->base, "get_stream_messages requires a stream name");
  }
  arg++;

  if ((idxNum & 2) != 0 && arg < argc) {
    if (!optional_i64(argv[arg], 0, &position)) {
      return vtab_error(&vtab->base, "position must be an integer or NULL");
    }
    arg++;
  }
  if ((idxNum & 4) != 0 && arg < argc) {
    if (!optional_i64(argv[arg], 1000, &batch_size)) {
      return vtab_error(&vtab->base, "batch_size must be an integer or NULL");
    }
    arg++;
  }
  if ((idxNum & 8) != 0 && arg < argc && optional_is_present(argv[arg])) {
    return vtab_error(&vtab->base,
                      "Retrieval with SQL condition is not supported by this SQLite port");
  }

  if (msgdb_is_category(stream_name, (size_t)stream_name_bytes)) {
    return vtab_error(&vtab->base, "Must be a stream name: %.*s", stream_name_bytes,
                      stream_name);
  }
  if (batch_size < -1) {
    return vtab_error(&vtab->base, "batch_size must be -1 or greater");
  }

  rc = sqlite3_prepare_v2(vtab->db, sql, -1, &cursor->stmt, NULL);
  if (rc != SQLITE_OK) {
    return vtab_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
  }

  rc = bind_text_value(cursor->stmt, 1, stream_name, stream_name_bytes);
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(cursor->stmt, 2, position);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(cursor->stmt, 3, batch_size);
  }
  if (rc != SQLITE_OK) {
    return vtab_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
  }

  return cursor_step(cursor);
}

static int filter_category(msgdb_cursor *cursor, int idxNum, int argc,
                           sqlite3_value **argv) {
  static const char no_filters_sql[] =
      "SELECT id, stream_name, type, position, global_position, data, metadata, time "
      "FROM messages "
      "WHERE category(stream_name) = ?1 AND global_position >= ?2 "
      "ORDER BY global_position ASC "
      "LIMIT ?3";
  static const char correlation_sql[] =
      "SELECT id, stream_name, type, position, global_position, data, metadata, time "
      "FROM messages "
      "WHERE category(stream_name) = ?1 AND global_position >= ?2 "
      "AND category(json_extract(metadata, '$.correlationStreamName')) = ?3 "
      "ORDER BY global_position ASC "
      "LIMIT ?4";
  static const char consumer_group_sql[] =
      "SELECT id, stream_name, type, position, global_position, data, metadata, time "
      "FROM messages "
      "WHERE category(stream_name) = ?1 AND global_position >= ?2 "
      "AND ((CASE hash_64(cardinal_id(stream_name)) "
      "WHEN (-9223372036854775807 - 1) THEN 9223372036854775807 "
      "ELSE abs(hash_64(cardinal_id(stream_name))) END) % ?3) = ?4 "
      "ORDER BY global_position ASC "
      "LIMIT ?5";
  static const char correlation_consumer_group_sql[] =
      "SELECT id, stream_name, type, position, global_position, data, metadata, time "
      "FROM messages "
      "WHERE category(stream_name) = ?1 AND global_position >= ?2 "
      "AND category(json_extract(metadata, '$.correlationStreamName')) = ?3 "
      "AND ((CASE hash_64(cardinal_id(stream_name)) "
      "WHEN (-9223372036854775807 - 1) THEN 9223372036854775807 "
      "ELSE abs(hash_64(cardinal_id(stream_name))) END) % ?4) = ?5 "
      "ORDER BY global_position ASC "
      "LIMIT ?6";
  msgdb_vtab *vtab = (msgdb_vtab *)cursor->base.pVtab;
  const char *category = NULL;
  const char *correlation = NULL;
  const char *sql = no_filters_sql;
  int category_bytes = 0;
  int correlation_bytes = 0;
  sqlite3_int64 position = 1;
  sqlite3_int64 batch_size = 1000;
  sqlite3_int64 consumer_group_member = 0;
  sqlite3_int64 consumer_group_size = 0;
  bool has_correlation = false;
  bool has_member = false;
  bool has_size = false;
  int arg = 0;
  int bind_index = 1;
  int rc = SQLITE_OK;

  if ((idxNum & 1) == 0 || arg >= argc ||
      !vtab_value_text(argv[arg], &category, &category_bytes)) {
    return vtab_error(&vtab->base, "get_category_messages requires a category");
  }
  arg++;

  if ((idxNum & 2) != 0 && arg < argc) {
    if (!optional_i64(argv[arg], 1, &position)) {
      return vtab_error(&vtab->base, "position must be an integer or NULL");
    }
    arg++;
  }
  if ((idxNum & 4) != 0 && arg < argc) {
    if (!optional_i64(argv[arg], 1000, &batch_size)) {
      return vtab_error(&vtab->base, "batch_size must be an integer or NULL");
    }
    arg++;
  }
  if ((idxNum & 8) != 0 && arg < argc) {
    has_correlation = optional_is_present(argv[arg]);
    if (has_correlation && !vtab_value_text(argv[arg], &correlation, &correlation_bytes)) {
      return vtab_error(&vtab->base, "correlation must be text or NULL");
    }
    arg++;
  }
  if ((idxNum & 16) != 0 && arg < argc) {
    has_member = optional_is_present(argv[arg]);
    if (!optional_i64(argv[arg], 0, &consumer_group_member)) {
      return vtab_error(&vtab->base, "consumer_group_member must be an integer or NULL");
    }
    arg++;
  }
  if ((idxNum & 32) != 0 && arg < argc) {
    has_size = optional_is_present(argv[arg]);
    if (!optional_i64(argv[arg], 0, &consumer_group_size)) {
      return vtab_error(&vtab->base, "consumer_group_size must be an integer or NULL");
    }
    arg++;
  }
  if ((idxNum & 64) != 0 && arg < argc && optional_is_present(argv[arg])) {
    return vtab_error(&vtab->base,
                      "Retrieval with SQL condition is not supported by this SQLite port");
  }

  if (!msgdb_is_category(category, (size_t)category_bytes)) {
    return vtab_error(&vtab->base, "Must be a category: %.*s", category_bytes, category);
  }
  if (has_correlation && !msgdb_is_category(correlation, (size_t)correlation_bytes)) {
    return vtab_error(&vtab->base, "Correlation must be a category (Correlation: %.*s)",
                      correlation_bytes, correlation);
  }
  if (has_member != has_size) {
    return vtab_error(&vtab->base, "Consumer group member and size must both be specified");
  }
  if (has_size && consumer_group_size < 1) {
    return vtab_error(&vtab->base, "Consumer group size must not be less than 1");
  }
  if (has_member && consumer_group_member < 0) {
    return vtab_error(&vtab->base, "Consumer group member must not be less than 0");
  }
  if (has_member && consumer_group_member >= consumer_group_size) {
    return vtab_error(&vtab->base,
                      "Consumer group member must be less than the group size");
  }
  if (batch_size < -1) {
    return vtab_error(&vtab->base, "batch_size must be -1 or greater");
  }

  if (has_correlation && has_member) {
    sql = correlation_consumer_group_sql;
  } else if (has_correlation) {
    sql = correlation_sql;
  } else if (has_member) {
    sql = consumer_group_sql;
  }

  rc = sqlite3_prepare_v2(vtab->db, sql, -1, &cursor->stmt, NULL);
  if (rc != SQLITE_OK) {
    return vtab_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
  }

  rc = bind_text_value(cursor->stmt, bind_index, category, category_bytes);
  bind_index++;
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(cursor->stmt, bind_index, position);
    bind_index++;
  }
  if (rc == SQLITE_OK && has_correlation) {
    rc = bind_text_value(cursor->stmt, bind_index, correlation, correlation_bytes);
    bind_index++;
  }
  if (rc == SQLITE_OK && has_member) {
    rc = sqlite3_bind_int64(cursor->stmt, bind_index, consumer_group_size);
    bind_index++;
  }
  if (rc == SQLITE_OK && has_member) {
    rc = sqlite3_bind_int64(cursor->stmt, bind_index, consumer_group_member);
    bind_index++;
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(cursor->stmt, bind_index, batch_size);
  }
  if (rc != SQLITE_OK) {
    return vtab_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
  }

  return cursor_step(cursor);
}

static int filter_last_stream(msgdb_cursor *cursor, int idxNum, int argc,
                              sqlite3_value **argv) {
  static const char sql[] =
      "SELECT id, stream_name, type, position, global_position, data, metadata, time "
      "FROM messages "
      "WHERE stream_name = ?1 "
      "ORDER BY position DESC "
      "LIMIT 1";
  static const char type_sql[] =
      "SELECT id, stream_name, type, position, global_position, data, metadata, time "
      "FROM messages "
      "WHERE stream_name = ?1 AND type = ?2 "
      "ORDER BY position DESC "
      "LIMIT 1";
  msgdb_vtab *vtab = (msgdb_vtab *)cursor->base.pVtab;
  const char *stream_name = NULL;
  const char *type = NULL;
  int stream_name_bytes = 0;
  int type_bytes = 0;
  int arg = 0;
  int rc = SQLITE_OK;
  bool has_type = false;

  if ((idxNum & 1) == 0 || arg >= argc ||
      !vtab_value_text(argv[arg], &stream_name, &stream_name_bytes)) {
    return vtab_error(&vtab->base, "get_last_stream_message requires a stream name");
  }
  arg++;

  if ((idxNum & 2) != 0 && arg < argc) {
    has_type = optional_is_present(argv[arg]);
    if (has_type && !vtab_value_text(argv[arg], &type, &type_bytes)) {
      return vtab_error(&vtab->base, "type must be text or NULL");
    }
  }

  rc = sqlite3_prepare_v2(vtab->db, has_type ? type_sql : sql, -1, &cursor->stmt, NULL);
  if (rc != SQLITE_OK) {
    return vtab_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
  }

  rc = bind_text_value(cursor->stmt, 1, stream_name, stream_name_bytes);
  if (rc == SQLITE_OK && has_type) {
    rc = bind_text_value(cursor->stmt, 2, type, type_bytes);
  }
  if (rc != SQLITE_OK) {
    return vtab_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
  }

  return cursor_step(cursor);
}

static void result_text_span(sqlite3_context *ctx, const char *value, msgdb_span span) {
  if (!span.present || value == NULL) {
    sqlite3_result_null(ctx);
    return;
  }

  if (span.length > (size_t)INT32_MAX) {
    sqlite3_result_error_toobig(ctx);
    return;
  }

  sqlite3_result_text(ctx, value + span.offset, (int)span.length, SQLITE_TRANSIENT);
}

static bool value_text(sqlite3_value *value, const char **text, int *bytes) {
  const unsigned char *raw = NULL;
  int raw_bytes = 0;

  if (sqlite3_value_type(value) == SQLITE_NULL) {
    return false;
  }

  raw = sqlite3_value_text(value);
  raw_bytes = sqlite3_value_bytes(value);
  if (raw == NULL || raw_bytes < 0) {
    return false;
  }

  *text = (const char *)raw;
  *bytes = raw_bytes;
  return true;
}

static bool value_text_size(sqlite3_value *value, const char **text, size_t *bytes) {
  int raw_bytes = 0;

  if (!value_text(value, text, &raw_bytes)) {
    return false;
  }

  *bytes = (size_t)raw_bytes;
  return true;
}

static int query_stream_version(sqlite3 *db, const char *stream_name, int stream_name_bytes,
                                bool *found, sqlite3_int64 *version) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(
      db, "SELECT max(position) FROM messages WHERE stream_name = ?1", -1, &stmt, NULL);

  *found = false;
  *version = 0;

  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = bind_text_value(stmt, 1, stream_name, stream_name_bytes);
  if (rc != SQLITE_OK) {
    (void)sqlite3_finalize(stmt);
    return rc;
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      *found = true;
      *version = sqlite3_column_int64(stmt, 0);
    }
    rc = SQLITE_OK;
  } else if (rc == SQLITE_DONE) {
    rc = SQLITE_OK;
  }

  {
    int finalize_rc = sqlite3_finalize(stmt);
    if (rc == SQLITE_OK && finalize_rc != SQLITE_OK) {
      rc = finalize_rc;
    }
  }

  return rc;
}

static int sql_exec(sqlite3 *db, const char *sql, char **error) {
  return sqlite3_exec(db, sql, NULL, NULL, error);
}

static void set_context_sql_error(sqlite3_context *ctx, const char *prefix, int rc) {
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  char *message = sqlite3_mprintf("%s: %s", prefix, sqlite3_errstr(rc));

  if (message == NULL) {
    sqlite3_result_error_nomem(ctx);
    return;
  }

  if (db != NULL && sqlite3_errmsg(db) != NULL) {
    sqlite3_free(message);
    message = sqlite3_mprintf("%s: %s", prefix, sqlite3_errmsg(db));
    if (message == NULL) {
      sqlite3_result_error_nomem(ctx);
      return;
    }
  }

  sqlite3_result_error(ctx, message, -1);
  sqlite3_free(message);
}

static int vtab_error(sqlite3_vtab *vtab, const char *format, ...) {
  va_list args;

  sqlite3_free(vtab->zErrMsg);
  va_start(args, format);
  vtab->zErrMsg = sqlite3_vmprintf(format, args);
  va_end(args);

  return vtab->zErrMsg == NULL ? SQLITE_NOMEM : SQLITE_ERROR;
}

static bool vtab_value_text(sqlite3_value *value, const char **text, int *bytes) {
  return value_text(value, text, bytes);
}

static bool optional_i64(sqlite3_value *value, sqlite3_int64 default_value,
                         sqlite3_int64 *result) {
  if (sqlite3_value_type(value) == SQLITE_NULL) {
    *result = default_value;
    return true;
  }

  if (sqlite3_value_type(value) != SQLITE_INTEGER) {
    return false;
  }

  *result = sqlite3_value_int64(value);
  return true;
}

static bool optional_is_present(sqlite3_value *value) {
  return sqlite3_value_type(value) != SQLITE_NULL;
}

static int bind_text_value(sqlite3_stmt *stmt, int index, const char *text, int bytes) {
  return sqlite3_bind_text(stmt, index, text, bytes, SQLITE_TRANSIENT);
}

static int cursor_step(msgdb_cursor *cursor) {
  int rc = sqlite3_step(cursor->stmt);
  msgdb_vtab *vtab = (msgdb_vtab *)cursor->base.pVtab;

  if (rc == SQLITE_ROW) {
    cursor->eof = false;
    return SQLITE_OK;
  }
  if (rc == SQLITE_DONE) {
    cursor->eof = true;
    return SQLITE_OK;
  }

  cursor->eof = true;
  return vtab_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
}
