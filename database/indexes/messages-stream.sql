CREATE UNIQUE INDEX IF NOT EXISTS messages_stream ON messages (stream_name, position);
