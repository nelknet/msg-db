CREATE INDEX IF NOT EXISTS messages_category ON messages (
  category(stream_name),
  global_position,
  category(json_extract(metadata, '$.correlationStreamName'))
);
