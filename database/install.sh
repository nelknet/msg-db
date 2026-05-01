#!/usr/bin/env bash

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
database=${DATABASE_NAME:-"$root/message_store.sqlite3"}
extension=${MSGDB_EXTENSION:-"$root/build/extension/message_db"}
sqlite=${SQLITE:-sqlite3}

sqlite_dot_arg() {
  local value=$1

  if [[ $value == *$'\n'* || $value == *$'\r'* ]]; then
    printf 'sqlite dot-command paths cannot contain newlines: %s\n' "$value" >&2
    exit 1
  fi

  value=${value//\\/\\\\}
  value=${value//\"/\\\"}
  printf '"%s"' "$value"
}

{
  printf '.bail on\n'
  printf '.load %s\n' "$(sqlite_dot_arg "$extension")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/schema/message-store.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/tables/messages.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/indexes/messages-id.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/indexes/messages-stream.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/indexes/messages-category.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/views/category-type-summary.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/views/stream-summary.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/views/stream-type-summary.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/views/type-category-summary.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/views/type-stream-summary.sql")"
  printf '.read %s\n' "$(sqlite_dot_arg "$root/database/views/type-summary.sql")"
} | "$sqlite" "$database"
