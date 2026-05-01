# SQLite Extension Functions

The SQLite port implements Message DB's function surface in the loadable C extension
instead of per-function SQL files:

- `category`
- `is_category`
- `id`
- `cardinal_id`
- `hash_64`
- `stream_version`
- `message_store_version`
- `gen_random_uuid`
- `write_message`
- `acquire_lock`
- `get_stream_messages`
- `get_category_messages`
- `get_last_stream_message`

Load the compiled extension before running the install SQL so expression indexes can
reference deterministic functions such as `category`.
