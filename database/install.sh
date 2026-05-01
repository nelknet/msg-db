#!/usr/bin/env bash

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
database=${DATABASE_NAME:-"$root/message_store.sqlite3"}
extension=${MSGDB_EXTENSION:-"$root/build/extension/message_db"}

sqlite3 "$database" <<SQL
.bail on
.load $extension
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
SQL
