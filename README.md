# Msg DB

Msg DB is a SQLite port of the core
[Message DB](https://github.com/message-db/message-db) database API. It keeps the
upstream shape of `database/` and `test/`, but replaces Postgres-specific behavior
with a small SQLite loadable extension and SQLite-native schema objects.

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
- C unit tests, SQLite integration tests, concurrency tests, sanitizer builds, and a
  fuzz harness

Not implemented:

- the upstream SQL `condition` retrieval argument. This port rejects it instead of
  appending caller-provided SQL into a query string.
- packaged releases. Build artifacts are produced locally by CMake.

## Requirements

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

## Quick Start

Build and test:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

If Ninja is not installed:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The loadable extension is written to:

```text
build/extension/message_db.so
```

Platform suffixes vary:

```text
build/extension/message_db.so     Linux and other Unix-like systems
build/extension/message_db.dylib  macOS
build/extension/message_db.dll    Windows
```

SQLite often accepts the path without the suffix:

```text
build/extension/message_db
```

## Create A Store

Install the schema into a local SQLite database:

```sh
MSGDB_EXTENSION=build/extension/message_db database/install.sh
```

By default this creates `message_store.sqlite3` in the repository root. To choose a
different file:

```sh
DATABASE_NAME=/tmp/message_store.sqlite3 \
MSGDB_EXTENSION=build/extension/message_db \
database/install.sh
```

On Windows PowerShell:

```powershell
$env:MSGDB_EXTENSION = "build/extension/message_db.dll"
.\database\install.ps1
```

The extension must be loaded for each SQLite connection that uses the Message DB API.
For the SQLite CLI:

```sql
.load build/extension/message_db
```

Some SQLite builds disable loadable extensions in the CLI. Application code can load
the extension through the SQLite C API with `sqlite3_enable_load_extension` and
`sqlite3_load_extension`, or by statically registering `msgdb_register`.

## Example

```sql
.load build/extension/message_db

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
xcrun clang-format --dry-run --Werror extension/*.c extension/*.h test/c/*.c fuzz/*.c
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
cmake --build build-fuzz --target fuzz_strings
./build-fuzz/fuzz_strings
```

When libFuzzer is available, `fuzz_strings` is built as a libFuzzer target. Otherwise
it falls back to a sanitizer-backed standalone smoke executable.

## Repository Layout

```text
database/             SQLite schema, indexes, views, and install scripts
extension/            C extension and reusable core helpers
fuzz/                 fuzz harnesses
test/c/               C unit and integration tests
test/scripts/         shell-oriented SQL test support
.github/workflows/    CI, CodeQL, and scheduled safety checks
```
