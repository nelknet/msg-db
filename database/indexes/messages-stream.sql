CREATE UNIQUE INDEX IF NOT EXISTS messages_stream ON messages (stream_name, position);

CREATE INDEX IF NOT EXISTS messages_stream_type ON messages (
  stream_name,
  type,
  position DESC
);
