#include <sqlite3.h>

#include "message_db.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

typedef struct writer_arg {
  const char *database_path;
  int index;
  int rc;
} writer_arg;

static void fail(sqlite3 *db, const char *message);
static void require_true(bool condition, const char *message);
static char *create_temp_database_path(void);
static void remove_database_files(const char *database_path);
static sqlite3 *open_db(const char *path);
static void exec_sql(sqlite3 *db, const char *sql);
static void exec_file(sqlite3 *db, const char *root, const char *relative_path);
static char *read_file(const char *path);
static char *root_path(const char *root, const char *relative_path);
static char *scalar(sqlite3 *db, const char *sql);
static void assert_scalar_eq(sqlite3 *db, const char *expected, const char *sql);
static void assert_error(sqlite3 *db, const char *sql);
#ifdef _WIN32
static DWORD WINAPI writer_thread(LPVOID data);
#else
static void *writer_thread(void *data);
#endif
static void run_concurrency_test(const char *database_path, sqlite3 *db);

int main(int argc, char **argv) {
  char *db_path = NULL;
  sqlite3 *db = NULL;

  if (argc != 2) {
    fprintf(stderr, "usage: %s REPO_ROOT\n", argv[0]);
    return 2;
  }

  db_path = create_temp_database_path();

  db = open_db(db_path);
  exec_file(db, argv[1], "database/schema/message-store.sql");
  exec_file(db, argv[1], "database/tables/messages.sql");
  exec_file(db, argv[1], "database/indexes/messages-id.sql");
  exec_file(db, argv[1], "database/indexes/messages-stream.sql");
  exec_file(db, argv[1], "database/indexes/messages-category.sql");
  exec_file(db, argv[1], "database/views/category-type-summary.sql");
  exec_file(db, argv[1], "database/views/stream-summary.sql");
  exec_file(db, argv[1], "database/views/stream-type-summary.sql");
  exec_file(db, argv[1], "database/views/type-category-summary.sql");
  exec_file(db, argv[1], "database/views/type-stream-summary.sql");
  exec_file(db, argv[1], "database/views/type-summary.sql");

  assert_scalar_eq(db, "1.3.0", "SELECT message_store_version()");
  assert_scalar_eq(db, "account", "SELECT category('account-123+456')");
  assert_scalar_eq(db, "123+456", "SELECT id('account-123+456')");
  assert_scalar_eq(db, "123", "SELECT cardinal_id('account-123+456')");
  assert_scalar_eq(db, "1", "SELECT is_category('account')");
  assert_scalar_eq(db, "0", "SELECT is_category('account-123')");
  assert_scalar_eq(db, "-6897031765186548867", "SELECT hash_64('someStream-123')");
  assert_scalar_eq(db, "1",
                   "WITH generated(uuid) AS (SELECT gen_random_uuid()) "
                   "SELECT length(uuid) = 36 AND substr(uuid, 15, 1) = '4' AND "
                   "lower(substr(uuid, 20, 1)) IN ('8', '9', 'a', 'b') FROM generated");

  assert_scalar_eq(db, "0",
                   "SELECT write_message('a11e9022-e741-4450-bf9c-c4cc5ddb6ea3', "
                   "'account-1', 'Deposited', '{\"amount\": 10}', "
                   "'{\"correlationStreamName\":\"order-1\"}')");
  assert_scalar_eq(db, "1",
                   "SELECT write_message('b11e9022-e741-4450-bf9c-c4cc5ddb6ea3', "
                   "'account-1', 'Deposited', '{\"amount\": 20}', "
                   "'{\"correlationStreamName\":\"order-1\"}', 0)");
  assert_scalar_eq(db, "0",
                   "SELECT write_message('c11e9022-e741-4450-bf9c-c4cc5ddb6ea3', "
                   "'account-2', 'Withdrawn', '{\"amount\": 5}', "
                   "'{\"correlationStreamName\":\"shipment-2\"}', -1)");
  assert_scalar_eq(db, "0",
                   "SELECT write_message('d11e9022-e741-4450-bf9c-c4cc5ddb6ea3', "
                   "'invoice-1', 'Issued', '{\"amount\": 10}', NULL)");

  assert_error(db, "SELECT write_message('e11e9022-e741-4450-bf9c-c4cc5ddb6ea3', "
                   "'account-1', 'Deposited', '{\"amount\": 30}', NULL, 0)");
  assert_error(db, "SELECT write_message('a11e9022-e741-4450-bf9c-c4cc5ddb6ea3', "
                   "'account-3', 'Deposited', '{\"amount\": 1}', NULL)");
  assert_error(db, "SELECT write_message('e11e9022-e741-4450-bf9c-c4cc5ddb6ea3', "
                   "'account-3', 'Deposited', '{broken}', NULL)");

  assert_scalar_eq(db, "1", "SELECT stream_version('account-1')");
  assert_scalar_eq(db, "", "SELECT stream_version('missing-1')");
  assert_scalar_eq(
      db, "0,1",
      "SELECT group_concat(position, ',') FROM get_stream_messages('account-1')");
  assert_scalar_eq(db, "1",
                   "SELECT position FROM get_stream_messages('account-1', 1, 1000)");
  assert_scalar_eq(db, "1", "SELECT position FROM get_last_stream_message('account-1')");
  assert_scalar_eq(db, "1",
                   "SELECT position FROM get_last_stream_message('account-1', "
                   "'Deposited')");
  assert_error(db, "SELECT * FROM get_stream_messages('account')");

  assert_scalar_eq(
      db, "1,2,3",
      "SELECT group_concat(global_position, ',') FROM get_category_messages('account')");
  assert_scalar_eq(db, "1,2",
                   "SELECT group_concat(global_position, ',') FROM "
                   "get_category_messages('account', 1, 1000, 'order')");
  assert_error(db, "SELECT * FROM get_category_messages('account-1')");
  assert_error(db, "SELECT * FROM get_category_messages('account', 1, 1000, 'order-1')");
  assert_scalar_eq(
      db, "3",
      "SELECT (SELECT COUNT(*) FROM get_category_messages('account', 1, 1000, "
      "NULL, 0, 2)) + (SELECT COUNT(*) FROM get_category_messages('account', 1, "
      "1000, NULL, 1, 2))");

  assert_scalar_eq(db, "account-1|2|50.0",
                   "SELECT stream_name || '|' || message_count || '|' || percent "
                   "FROM stream_summary WHERE stream_name = 'account-1'");
  assert_scalar_eq(db, "Deposited|2|50.0",
                   "SELECT type || '|' || message_count || '|' || percent "
                   "FROM type_summary WHERE type = 'Deposited'");
  assert_error(db, "INSERT INTO messages (id, stream_name, position, type, data) "
                   "VALUES (gen_random_uuid(), 'account-1', 1, 'Deposited', '{}')");

  run_concurrency_test(db_path, db);

  sqlite3_close(db);
  remove_database_files(db_path);
  sqlite3_free(db_path);
  return 0;
}

static void fail(sqlite3 *db, const char *message) {
  if (db != NULL) {
    fprintf(stderr, "%s: %s\n", message, sqlite3_errmsg(db));
  } else {
    fprintf(stderr, "%s\n", message);
  }
  exit(1);
}

static void require_true(bool condition, const char *message) {
  if (!condition) {
    fail(NULL, message);
  }
}

static char *create_temp_database_path(void) {
#ifdef _WIN32
  char temp_dir[MAX_PATH + 1U] = {0};
  char temp_file[MAX_PATH + 1U] = {0};
  DWORD length = GetTempPathA((DWORD)sizeof(temp_dir), temp_dir);
  char *path = NULL;

  if (length == 0U || length > (DWORD)sizeof(temp_dir)) {
    fail(NULL, "GetTempPathA failed");
  }
  if (GetTempFileNameA(temp_dir, "mdb", 0U, temp_file) == 0U) {
    fail(NULL, "GetTempFileNameA failed");
  }

  path = sqlite3_mprintf("%s", temp_file);
  if (path == NULL) {
    fail(NULL, "sqlite3_mprintf failed");
  }

  return path;
#else
  char db_template[] = "/tmp/msgdb-sqlite-test.XXXXXX";
  int fd = mkstemp(db_template);
  char *path = NULL;

  if (fd < 0) {
    fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
    exit(1);
  }
  (void)close(fd);

  path = sqlite3_mprintf("%s", db_template);
  if (path == NULL) {
    fail(NULL, "sqlite3_mprintf failed");
  }

  return path;
#endif
}

static void remove_database_files(const char *database_path) {
  const char *suffixes[] = {"", "-wal", "-shm", "-journal"};

  for (size_t i = 0U; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
    char *path = sqlite3_mprintf("%s%s", database_path, suffixes[i]);

    if (path == NULL) {
      fail(NULL, "sqlite3_mprintf failed");
    }
#ifdef _WIN32
    (void)DeleteFileA(path);
#else
    (void)unlink(path);
#endif
    sqlite3_free(path);
  }
}

static sqlite3 *open_db(const char *path) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);

  if (rc != SQLITE_OK) {
    fail(db, "sqlite3_open_v2 failed");
  }

  rc = msgdb_register(db);
  if (rc != SQLITE_OK) {
    fail(db, "msgdb_register failed");
  }

  exec_sql(db, "PRAGMA busy_timeout = 5000");
  exec_sql(db, "PRAGMA journal_mode = WAL");
  return db;
}

static void exec_sql(sqlite3 *db, const char *sql) {
  char *error = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &error);

  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL failed: %s\nSQL: %s\n", error != NULL ? error : sqlite3_errmsg(db),
            sql);
    sqlite3_free(error);
    exit(1);
  }

  sqlite3_free(error);
}

static void exec_file(sqlite3 *db, const char *root, const char *relative_path) {
  char *path = root_path(root, relative_path);
  char *sql = read_file(path);

  exec_sql(db, sql);
  sqlite3_free(sql);
  sqlite3_free(path);
}

static char *read_file(const char *path) {
  FILE *file = fopen(path, "rb");
  long file_size = 0;
  size_t file_size_bytes = 0U;
  size_t bytes_read = 0U;
  char *buffer = NULL;

  if (file == NULL) {
    fprintf(stderr, "fopen failed for %s: %s\n", path, strerror(errno));
    exit(1);
  }

  if (fseek(file, 0L, SEEK_END) != 0) {
    fclose(file);
    fail(NULL, "fseek failed");
  }
  file_size = ftell(file);
  if (file_size < 0) {
    fclose(file);
    fail(NULL, "ftell failed");
  }
  if (fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    fail(NULL, "fseek reset failed");
  }
  file_size_bytes = (size_t)file_size;

  buffer = sqlite3_malloc64((sqlite3_uint64)file_size_bytes + 1U);
  if (buffer == NULL) {
    fclose(file);
    fail(NULL, "sqlite3_malloc64 failed");
  }

  bytes_read = fread(buffer, 1U, file_size_bytes, file);
  if (bytes_read != file_size_bytes) {
    sqlite3_free(buffer);
    fclose(file);
    fail(NULL, "fread failed");
  }
  buffer[file_size_bytes] = '\0';

  fclose(file);
  return buffer;
}

static char *root_path(const char *root, const char *relative_path) {
  char *path = sqlite3_mprintf("%s/%s", root, relative_path);

  if (path == NULL) {
    fail(NULL, "sqlite3_mprintf failed");
  }

  return path;
}

static char *scalar(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  const unsigned char *text = NULL;
  char *result = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    fail(db, "sqlite3_prepare_v2 failed");
  }

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    fail(db, "sqlite3_step did not return a row");
  }

  text = sqlite3_column_text(stmt, 0);
  result = sqlite3_mprintf("%s", text != NULL ? (const char *)text : "");
  if (result == NULL) {
    sqlite3_finalize(stmt);
    fail(NULL, "sqlite3_mprintf failed");
  }

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_free(result);
    fail(db, "sqlite3_step returned extra rows or error");
  }

  rc = sqlite3_finalize(stmt);
  if (rc != SQLITE_OK) {
    sqlite3_free(result);
    fail(db, "sqlite3_finalize failed");
  }

  return result;
}

static void assert_scalar_eq(sqlite3 *db, const char *expected, const char *sql) {
  char *actual = scalar(db, sql);

  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "Assertion failed\nSQL: %s\nExpected: %s\nActual: %s\n", sql, expected,
            actual);
    sqlite3_free(actual);
    exit(1);
  }

  sqlite3_free(actual);
}

static void assert_error(sqlite3 *db, const char *sql) {
  char *error = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &error);

  if (rc == SQLITE_OK) {
    fprintf(stderr, "Expected SQL to fail, but it succeeded:\n%s\n", sql);
    sqlite3_free(error);
    exit(1);
  }

  sqlite3_free(error);
}

#ifdef _WIN32
static DWORD WINAPI writer_thread(LPVOID data) {
#else
static void *writer_thread(void *data) {
#endif
  writer_arg *arg = (writer_arg *)data;
  sqlite3 *db = open_db(arg->database_path);
  char *sql = sqlite3_mprintf(
      "SELECT write_message(gen_random_uuid(), 'concurrent-1', 'Concurrent', "
      "'{\"n\": %d}', NULL)",
      arg->index);

  if (sql == NULL) {
    arg->rc = SQLITE_NOMEM;
    sqlite3_close(db);
#ifdef _WIN32
    return 0U;
#else
    return NULL;
#endif
  }

  arg->rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  sqlite3_free(sql);
  sqlite3_close(db);
#ifdef _WIN32
  return 0U;
#else
  return NULL;
#endif
}

static void run_concurrency_test(const char *database_path, sqlite3 *db) {
  enum { writer_count = 20 };
#ifdef _WIN32
  HANDLE threads[writer_count];
#else
  pthread_t threads[writer_count];
#endif
  writer_arg args[writer_count];

  for (int i = 0; i < writer_count; i++) {
    args[i].database_path = database_path;
    args[i].index = i + 1;
    args[i].rc = SQLITE_OK;
#ifdef _WIN32
    threads[i] = CreateThread(NULL, 0U, writer_thread, &args[i], 0U, NULL);
    require_true(threads[i] != NULL, "CreateThread failed");
#else
    require_true(pthread_create(&threads[i], NULL, writer_thread, &args[i]) == 0,
                 "pthread_create failed");
#endif
  }

  for (int i = 0; i < writer_count; i++) {
#ifdef _WIN32
    require_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                 "WaitForSingleObject failed");
    require_true(CloseHandle(threads[i]) != 0, "CloseHandle failed");
#else
    require_true(pthread_join(threads[i], NULL) == 0, "pthread_join failed");
#endif
    if (args[i].rc != SQLITE_OK) {
      fprintf(stderr, "writer %d failed with rc %d\n", args[i].index, args[i].rc);
      exit(1);
    }
  }

  assert_scalar_eq(db, "20",
                   "SELECT COUNT(*) FROM messages WHERE stream_name = 'concurrent-1'");
  assert_scalar_eq(db, "20",
                   "SELECT COUNT(DISTINCT position) FROM messages WHERE stream_name = "
                   "'concurrent-1'");
  assert_scalar_eq(db, "0,19",
                   "SELECT MIN(position) || ',' || MAX(position) FROM messages WHERE "
                   "stream_name = 'concurrent-1'");
}
