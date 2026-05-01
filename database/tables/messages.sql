CREATE TABLE IF NOT EXISTS messages (
  global_position INTEGER PRIMARY KEY AUTOINCREMENT,
  position INTEGER NOT NULL CHECK (position >= 0),
  time TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')),
  stream_name TEXT NOT NULL CHECK (length(stream_name) > 0),
  type TEXT NOT NULL CHECK (length(type) > 0),
  data TEXT CHECK (data IS NULL OR json_valid(data)),
  metadata TEXT CHECK (metadata IS NULL OR json_valid(metadata)),
  id TEXT NOT NULL CHECK (
    length(id) = 36 AND
    id GLOB
      '[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]-[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]-4[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]-[89AaBb][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]-[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]'
  )
) STRICT;

CREATE TRIGGER IF NOT EXISTS messages_append_only_no_update
BEFORE UPDATE ON messages
BEGIN
  SELECT RAISE(ABORT, 'messages is append-only');
END;

CREATE TRIGGER IF NOT EXISTS messages_append_only_no_delete
BEFORE DELETE ON messages
BEGIN
  SELECT RAISE(ABORT, 'messages is append-only');
END;
