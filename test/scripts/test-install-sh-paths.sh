#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  printf 'usage: %s REPO_ROOT EXTENSION_PATH\n' "$0" >&2
  exit 2
fi

repo_root=$1
extension=$2
sqlite=${SQLITE:-sqlite3}

if ! "$sqlite" -batch :memory: ".help load" | grep -q '^\.load '; then
  printf 'sqlite3 CLI does not support .load; skipping install.sh path test\n' >&2
  exit 77
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/msgdb install test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

stage="$tmp/release path with spaces"
stage_extension="$stage/extension/$(basename "$extension")"
database="$stage/message store.sqlite3"

mkdir -p "$stage/extension"
cp -R "$repo_root/database" "$stage/database"
cp "$extension" "$stage_extension"

SQLITE="$sqlite" DATABASE_NAME="$database" MSGDB_EXTENSION="$stage_extension" \
  "$stage/database/install.sh"

actual=$("$sqlite" -batch -noheader "$database" \
  "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name = 'messages';")

if [[ $actual != "1" ]]; then
  printf 'Expected install.sh to create messages table, got %s\n' "$actual" >&2
  exit 1
fi
