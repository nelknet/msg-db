CREATE VIEW IF NOT EXISTS stream_type_summary AS
WITH
  type_count AS (
    SELECT
      stream_name,
      type,
      COUNT(id) AS message_count
    FROM messages
    GROUP BY stream_name, type
  ),
  total_count AS (
    SELECT COUNT(id) AS total_count
    FROM messages
  )
SELECT
  stream_name,
  type,
  message_count,
  ROUND((message_count * 100.0) / total_count, 2) AS percent
FROM type_count, total_count
ORDER BY stream_name, type;
