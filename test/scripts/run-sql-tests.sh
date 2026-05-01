#!/usr/bin/env bash

set -euo pipefail

root=${MSGDB_ROOT:?MSGDB_ROOT is required}
extension=${MSGDB_EXTENSION:?MSGDB_EXTENSION is required}
db=$(mktemp "${TMPDIR:-/tmp}/msgdb-sqlite.XXXXXX.sqlite3")
trap 'rm -f "$db" "$db-wal" "$db-shm"' EXIT

sqlite() {
  sqlite3 -batch -noheader "$db" <<SQL
.bail on
.load $extension
$*
SQL
}

assert_eq() {
  local expected=$1
  local sql=$2
  local actual

  actual=$(sqlite "$sql")
  if [[ "$actual" != "$expected" ]]; then
    printf 'Assertion failed\nSQL: %s\nExpected: %s\nActual: %s\n' \
      "$sql" "$expected" "$actual" >&2
    exit 1
  fi
}

assert_error() {
  local sql=$1

  if sqlite "$sql" >/dev/null 2>&1; then
    printf 'Expected SQL to fail, but it succeeded:\n%s\n' "$sql" >&2
    exit 1
  fi
}

sqlite "
.read $root/database/schema/message-store.sql
.read $root/database/tables/messages.sql
.read $root/database/indexes/messages-id.sql
.read $root/database/indexes/messages-stream.sql
.read $root/database/indexes/messages-category.sql
.read $root/database/views/category-type-summary.sql
.read $root/database/views/stream-summary.sql
.read $root/database/views/stream-type-summary.sql
.read $root/database/views/type-category-summary.sql
.read $root/database/views/type-stream-summary.sql
.read $root/database/views/type-summary.sql
"

assert_eq '1.3.0' "SELECT message_store_version();"
assert_eq 'account' "SELECT category('account-123+456');"
assert_eq '123+456' "SELECT id('account-123+456');"
assert_eq '123' "SELECT cardinal_id('account-123+456');"
assert_eq '1' "SELECT is_category('account');"
assert_eq '0' "SELECT is_category('account-123');"
assert_eq '-6897031765186548867' "SELECT hash_64('someStream-123');"
assert_eq '1' "SELECT gen_random_uuid() GLOB '????????-????-4???-[89ab]???-????????????';"

assert_eq '0' \
  "SELECT write_message('a11e9022-e741-4450-bf9c-c4cc5ddb6ea3', 'account-1', 'Deposited', '{\"amount\": 10}', '{\"correlationStreamName\":\"order-1\"}');"
assert_eq '1' \
  "SELECT write_message('b11e9022-e741-4450-bf9c-c4cc5ddb6ea3', 'account-1', 'Deposited', '{\"amount\": 20}', '{\"correlationStreamName\":\"order-1\"}', 0);"
assert_eq '0' \
  "SELECT write_message('c11e9022-e741-4450-bf9c-c4cc5ddb6ea3', 'account-2', 'Withdrawn', '{\"amount\": 5}', '{\"correlationStreamName\":\"order-2\"}', -1);"
assert_eq '0' \
  "SELECT write_message('d11e9022-e741-4450-bf9c-c4cc5ddb6ea3', 'invoice-1', 'Issued', '{\"amount\": 10}', NULL);"

assert_error \
  "SELECT write_message('e11e9022-e741-4450-bf9c-c4cc5ddb6ea3', 'account-1', 'Deposited', '{\"amount\": 30}', NULL, 0);"
assert_error \
  "SELECT write_message('a11e9022-e741-4450-bf9c-c4cc5ddb6ea3', 'account-3', 'Deposited', '{\"amount\": 1}', NULL);"
assert_error \
  "SELECT write_message('e11e9022-e741-4450-bf9c-c4cc5ddb6ea3', 'account-3', 'Deposited', '{broken}', NULL);"

assert_eq '1' "SELECT stream_version('account-1');"
assert_eq '' "SELECT stream_version('missing-1');"
assert_eq '0,1' "SELECT group_concat(position, ',') FROM get_stream_messages('account-1');"
assert_eq '1' "SELECT position FROM get_stream_messages('account-1', 1, 1000);"
assert_eq '1' "SELECT position FROM get_last_stream_message('account-1');"
assert_eq '1' "SELECT position FROM get_last_stream_message('account-1', 'Deposited');"
assert_error "SELECT * FROM get_stream_messages('account');"

assert_eq '1,2,3' \
  "SELECT group_concat(global_position, ',') FROM get_category_messages('account');"
assert_eq '1,2' \
  "SELECT group_concat(global_position, ',') FROM get_category_messages('account', 1, 1000, 'order');"
assert_error "SELECT * FROM get_category_messages('account-1');"
assert_error "SELECT * FROM get_category_messages('account', 1, 1000, 'order-1');"
assert_eq '3' \
  "SELECT (SELECT COUNT(*) FROM get_category_messages('account', 1, 1000, NULL, 0, 2)) + (SELECT COUNT(*) FROM get_category_messages('account', 1, 1000, NULL, 1, 2));"

assert_eq 'account-1|2|50.0' \
  "SELECT stream_name || '|' || message_count || '|' || percent FROM stream_summary WHERE stream_name = 'account-1';"
assert_eq 'Deposited|2|50.0' \
  "SELECT type || '|' || message_count || '|' || percent FROM type_summary WHERE type = 'Deposited';"

assert_error \
  "INSERT INTO messages (id, stream_name, position, type, data) VALUES (gen_random_uuid(), 'account-1', 1, 'Deposited', '{}');"

sqlite "
PRAGMA journal_mode = WAL;
PRAGMA busy_timeout = 5000;
"

for i in $(seq 1 20); do
  sqlite3 -batch "$db" <<SQL &
.bail on
.load $extension
SELECT write_message(gen_random_uuid(), 'concurrent-1', 'Concurrent', '{\"n\": $i}', NULL);
SQL
done
wait

assert_eq '20' "SELECT COUNT(*) FROM messages WHERE stream_name = 'concurrent-1';"
assert_eq '20' "SELECT COUNT(DISTINCT position) FROM messages WHERE stream_name = 'concurrent-1';"
assert_eq '0,19' "SELECT MIN(position) || ',' || MAX(position) FROM messages WHERE stream_name = 'concurrent-1';"
