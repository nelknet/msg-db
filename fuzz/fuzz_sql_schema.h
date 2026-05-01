#ifndef MSGDB_FUZZ_SQL_SCHEMA_H
#define MSGDB_FUZZ_SQL_SCHEMA_H

#define MSGDB_FUZZ_SCHEMA_SQL                                                              \
  "PRAGMA foreign_keys = ON;"                                                              \
  "PRAGMA busy_timeout = 5000;"                                                            \
  "CREATE TABLE messages ("                                                                \
  "global_position INTEGER PRIMARY KEY AUTOINCREMENT,"                                     \
  "position INTEGER NOT NULL CHECK (position >= 0),"                                       \
  "time TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),"                     \
  "stream_name TEXT NOT NULL CHECK (length(stream_name) > 0),"                             \
  "type TEXT NOT NULL CHECK (length(type) > 0),"                                           \
  "data TEXT CHECK (data IS NULL OR json_valid(data)),"                                    \
  "metadata TEXT CHECK (metadata IS NULL OR json_valid(metadata)),"                        \
  "id TEXT NOT NULL CHECK ("                                                               \
  "length(id) = 36 AND "                                                                   \
  "id GLOB '[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]"            \
  "[0-9A-Fa-f][0-9A-Fa-f]-[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]-"                   \
  "4[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]-[89AaBb][0-9A-Fa-f][0-9A-Fa-f]"                      \
  "[0-9A-Fa-f]-[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]"                    \
  "[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]"                     \
  "[0-9A-Fa-f]')"                                                                          \
  ") STRICT;"                                                                              \
  "CREATE TRIGGER messages_append_only_no_update BEFORE UPDATE ON messages BEGIN "         \
  "SELECT RAISE(ABORT, 'messages is append-only'); END;"                                   \
  "CREATE TRIGGER messages_append_only_no_delete BEFORE DELETE ON messages BEGIN "         \
  "SELECT RAISE(ABORT, 'messages is append-only'); END;"                                   \
  "CREATE UNIQUE INDEX messages_id ON messages (id);"                                      \
  "CREATE UNIQUE INDEX messages_stream ON messages (stream_name, position);"               \
  "CREATE INDEX messages_category ON messages ("                                           \
  "category(stream_name), global_position, "                                               \
  "category(json_extract(metadata, '$.correlationStreamName')));"                          \
  "CREATE INDEX messages_category_correlation ON messages ("                               \
  "category(stream_name), category(json_extract(metadata, '$.correlationStreamName')), "   \
  "global_position);"                                                                      \
  "CREATE INDEX messages_stream_type ON messages (stream_name, type, position DESC);"

#endif
