# <img src="docs/msg-db-logo.svg" alt="" width="34" height="34"> Msg DB

[![CI](https://github.com/nelknet/msg-db/actions/workflows/ci.yml/badge.svg)](https://github.com/nelknet/msg-db/actions/workflows/ci.yml)
[![CodeQL](https://github.com/nelknet/msg-db/actions/workflows/codeql.yml/badge.svg)](https://github.com/nelknet/msg-db/actions/workflows/codeql.yml)
[![Release](https://img.shields.io/github/v/release/nelknet/msg-db?display_name=tag&label=release)](https://github.com/nelknet/msg-db/releases/latest)

Msg DB gives you Message DB-style event streams in one SQLite database file. It
stores append-only messages, assigns stream positions, supports category reads,
checks optimistic concurrency, and keeps JSON message bodies and metadata in
plain SQLite tables.

Download a release package, install the schema into a `.sqlite3` file, load the
extension in each SQLite connection, and use SQL functions like `write_message`,
`get_stream_messages`, and `get_category_messages`. No Postgres server is needed.

## Quick Start

Download the release package for your platform:

| Platform | Asset |
| --- | --- |
| Linux x86_64 | `msg-db-v0.1.0-linux-x86_64.zip` |
| macOS Apple Silicon | `msg-db-v0.1.0-macos-arm64.zip` |
| macOS Intel | `msg-db-v0.1.0-macos-x86_64.zip` |
| Windows x86_64 | `msg-db-v0.1.0-windows-x86_64.zip` |

On Linux x86_64:

```sh
version=v0.1.0
asset=msg-db-$version-linux-x86_64

curl -LO "https://github.com/nelknet/msg-db/releases/download/$version/$asset.zip"
unzip "$asset.zip"
cd "$asset"

MSGDB_EXTENSION=extension/message_db.so database/install.sh
```

For macOS, change the `asset` value to `msg-db-v0.1.0-macos-arm64` or
`msg-db-v0.1.0-macos-x86_64`, then use
`MSGDB_EXTENSION=extension/message_db.dylib`. If the system `sqlite3` does not
show `.load` in `.help load`, install SQLite with Homebrew and put it on your
`PATH` before running the install script.

Write and read a couple of messages from the new `message_store.sqlite3` file:

```sh
sqlite3 message_store.sqlite3 <<'SQL'
.load extension/message_db

SELECT write_message(
  gen_random_uuid(),
  'account-123',
  'Deposited',
  '{"amount": 100}',
  '{"correlationStreamName": "order-456"}'
);

SELECT write_message(
  gen_random_uuid(),
  'account-123',
  'Withdrawn',
  '{"amount": 25}',
  '{"correlationStreamName": "order-456"}',
  0
);

.headers on
.mode column

SELECT position, type, data
FROM get_stream_messages('account-123');

SELECT global_position, stream_name, type
FROM get_category_messages('account');
SQL
```

The first `write_message` returns stream position `0`. The second passes expected
version `0`, writes position `1`, and would fail if another writer had already
advanced the stream.

Check the store:

```sh
sqlite3 message_store.sqlite3 "SELECT COUNT(*) FROM messages;"
```

On Windows PowerShell:

```powershell
$Version = "v0.1.0"
$Asset = "msg-db-$Version-windows-x86_64"

Invoke-WebRequest `
  "https://github.com/nelknet/msg-db/releases/download/$Version/$Asset.zip" `
  -OutFile "$Asset.zip"
Expand-Archive "$Asset.zip" -DestinationPath .
Set-Location $Asset

$env:MSGDB_EXTENSION = "extension/message_db.dll"
.\database\install.ps1
```

## Design Notes

The port keeps the upstream shape of `database/` and `test/`, but replaces
Postgres-specific behavior with a small SQLite loadable extension and SQLite-native
schema objects.

The port is intentionally conservative:

- SQLite owns write serialization through `BEGIN IMMEDIATE`, WAL, and `busy_timeout`.
- `global_position` is the ordered store position.
- per-stream `position` starts at `0`.
- message IDs are UUIDv4 text values.
- `data` and `metadata` are JSON text guarded by `json_valid` checks.
- advisory locks are not reimplemented; `acquire_lock` is a compatibility shim.

## Status

Implemented:

- scalar functions for stream parsing, hashing, UUID generation, stream versioning,
  version reporting, compatibility locking, and writing messages
- table-valued virtual tables for stream, category, and last-message reads
- SQLite schema, indexes, and summary views
- C unit tests, SQLite integration tests, concurrency tests, sanitizer builds, and
  fuzz harnesses
- GitHub release packages for Linux x86_64, macOS x86_64, macOS arm64, and Windows
  x86_64

Not implemented:

- the upstream SQL `condition` retrieval argument. This port rejects it instead of
  appending caller-provided SQL into a query string.

## Requirements

Release packages require:

- SQLite 3 with loadable extension support
- `sqlite3` on your `PATH` if you use the included install scripts

Source builds require:

- CMake 3.20+
- a C11 compiler
- SQLite 3 with development headers
- Ninja is recommended, but not required

Optional development tools:

- `clang-format`
- `clang-tidy`
- `cppcheck`
- Valgrind on Linux
- a Clang toolchain with libFuzzer

## Build

If Ninja is not installed, use the platform default generator:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On macOS source builds, prefer Homebrew SQLite so the extension is built against
headers that expose SQLite's loadable-extension API:

```sh
brew install sqlite
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix sqlite)"
```

The loadable extension is written under `build/extension`. Platform suffixes vary:

```text
build/extension/message_db.so     Linux and Unix-like systems that use .so
build/extension/message_db.dylib  macOS
build/extension/message_db.dll    Windows
```

SQLite often accepts the path without the suffix:

```text
build/extension/message_db
```

## Create A Store

From a release package, install the schema into a local SQLite database:

```sh
MSGDB_EXTENSION=extension/message_db.so database/install.sh
```

Use `extension/message_db.dylib` on macOS and `extension/message_db.dll` on Windows.

By default this creates `message_store.sqlite3` in the repository root. To choose a
different file:

```sh
DATABASE_NAME=/tmp/message_store.sqlite3 \
MSGDB_EXTENSION=extension/message_db.so \
database/install.sh
```

On Windows PowerShell:

```powershell
$env:MSGDB_EXTENSION = "extension/message_db.dll"
.\database\install.ps1
```

When working from a source build, use the extension path under `build/extension`
instead.

The extension must be loaded for each SQLite connection that uses the Message DB API.
For the SQLite CLI:

```sql
.load extension/message_db
```

Some SQLite builds disable loadable extensions in the CLI. Application code can load
the extension through the SQLite C API with `sqlite3_enable_load_extension` and
`sqlite3_load_extension`, or by statically registering `msgdb_register`.

## Example

```sql
.load extension/message_db

SELECT write_message(
  gen_random_uuid(),
  'account-123',
  'Deposited',
  '{"amount": 100}',
  '{"correlationStreamName": "order-456"}'
);

SELECT
  position,
  type,
  data
FROM get_stream_messages('account-123');

SELECT
  global_position,
  stream_name,
  type
FROM get_category_messages('account');
```

## API

Scalar functions:

| Function | Description |
| --- | --- |
| `category(stream_name)` | Returns the part before the first `-`. |
| `is_category(stream_name)` | Returns true when the value has no `-`. |
| `id(stream_name)` | Returns the part after the first `-`, or `NULL`. |
| `cardinal_id(stream_name)` | Returns the stream ID before `+`, or `NULL`. |
| `hash_64(value)` | Matches upstream MD5-derived signed 64-bit hashing. |
| `stream_version(stream_name)` | Returns the max stream position, or `NULL`. |
| `message_store_version()` | Returns the compatible Message DB schema version. |
| `gen_random_uuid()` | Returns a UUIDv4 text ID. |
| `acquire_lock(stream_name)` | Compatibility shim that returns the category hash. |
| `write_message(id, stream_name, type, data, metadata, expected_version)` | Writes a message and returns its stream position. |

Table-valued functions:

| Function | Ordering |
| --- | --- |
| `get_stream_messages(stream_name, position, batch_size, condition)` | `position ASC` |
| `get_category_messages(category, position, batch_size, correlation, consumer_group_member, consumer_group_size, condition)` | `global_position ASC` |
| `get_last_stream_message(stream_name, type)` | last matching stream position |

The `condition` argument is accepted for signature compatibility, but any non-`NULL`
value fails.

## Schema

The `messages` table stores:

- `global_position INTEGER PRIMARY KEY`
- `position INTEGER NOT NULL`
- `time TEXT NOT NULL`
- `stream_name TEXT NOT NULL`
- `type TEXT NOT NULL`
- `data TEXT`
- `metadata TEXT`
- `id TEXT NOT NULL`

Indexes enforce unique message IDs and unique `(stream_name, position)` pairs. An
expression index supports category and correlation reads.

## Concurrency

`write_message` opens `BEGIN IMMEDIATE` when it is called outside a transaction. That
lets SQLite serialize writers before stream version calculation and insert. For
multi-connection use, configure connections like this:

```sql
PRAGMA journal_mode = WAL;
PRAGMA busy_timeout = 5000;
```

For explicit write batches, start the transaction yourself:

```sql
BEGIN IMMEDIATE;
SELECT write_message(gen_random_uuid(), 'account-123', 'Deposited', '{}', NULL);
SELECT write_message(gen_random_uuid(), 'account-123', 'Deposited', '{}', NULL, 0);
COMMIT;
```

## Development

Useful local checks:

```sh
xcrun clang-format --dry-run --Werror extension/*.c extension/*.h test/c/*.c fuzz/*.c fuzz/*.h
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer build:

```sh
cmake -S . -B build-asan -DMSGDB_ENABLE_SANITIZERS=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Fuzzer smoke build:

```sh
cmake -S . -B build-fuzz -DMSGDB_BUILD_FUZZERS=ON
cmake --build build-fuzz --target fuzz_strings fuzz_sql_model fuzz_vtab_args

./build-fuzz/fuzz_strings -runs=1024 -max_len=4096
./build-fuzz/fuzz_sql_model -runs=512 -max_len=1024
./build-fuzz/fuzz_vtab_args -runs=512 -max_len=1024
```

The fuzz suite covers three layers:

- `fuzz_strings` checks stream/category/id/cardinal-id properties, hash safety, and
  UUIDv4 canonicalization.
- `fuzz_sql_model` runs bounded write/read operation sequences against SQLite and a
  small in-memory model, checking stream positions, global positions, expected
  versions, duplicate IDs, stream reads, category reads, and last-message reads.
- `fuzz_vtab_args` feeds mixed NULL, integer, text, and blob arguments into the
  table-valued read APIs and requires clean rows or clean SQLite errors.

When libFuzzer is available, these are built as libFuzzer targets. Otherwise they
fall back to sanitizer-backed standalone smoke executables. CI runs short fuzz smoke
checks on each push and pull request; scheduled CI runs longer fuzz campaigns.

## Repository Layout

```text
database/             SQLite schema, indexes, views, and install scripts
docs/                 README logo and documentation assets
extension/            C extension and reusable core helpers
fuzz/                 fuzz harnesses
test/c/               C unit and integration tests
test/scripts/         shell-oriented SQL test support
VERSION               project release version
.github/workflows/    CI, CodeQL, and scheduled safety checks
```
