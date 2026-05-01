CREATE VIEW IF NOT EXISTS type_category_summary AS
WITH
  type_count AS (
    SELECT
      type,
      category(stream_name) AS category,
      COUNT(id) AS message_count
    FROM messages
    GROUP BY type, category
  ),
  total_count AS (
    SELECT COUNT(id) AS total_count
    FROM messages
  )
SELECT
  type,
  category,
  message_count,
  ROUND((message_count * 100.0) / total_count, 2) AS percent
FROM type_count, total_count
ORDER BY type, category;
