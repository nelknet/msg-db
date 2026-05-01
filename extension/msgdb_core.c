#include "msgdb_core.h"

#include <limits.h>
#include <string.h>

typedef struct msgdb_md5_ctx {
  uint32_t state[4];
  uint64_t total_length;
  unsigned char buffer[64];
  size_t buffer_length;
} msgdb_md5_ctx;

static uint32_t rotl32(uint32_t value, unsigned int count);
static uint32_t read_le32(const unsigned char *bytes);
static void write_le64(unsigned char *bytes, uint64_t value);
static void md5_init(msgdb_md5_ctx *ctx);
static void md5_update(msgdb_md5_ctx *ctx, const unsigned char *data, size_t length);
static void md5_final(msgdb_md5_ctx *ctx, unsigned char digest[16]);
static void md5_transform(msgdb_md5_ctx *ctx, const unsigned char block[64]);
static bool is_hex(char value);
static char lower_hex(char value);
static int64_t signed_i64_from_u64(uint64_t value);

msgdb_span msgdb_category_span(const char *value, size_t length) {
  msgdb_span span = {0U, 0U, false};

  if (value == NULL) {
    return span;
  }

  span.present = true;
  span.length = length;

  for (size_t i = 0U; i < length; i++) {
    if (value[i] == '-') {
      span.length = i;
      break;
    }
  }

  return span;
}

bool msgdb_is_category(const char *value, size_t length) {
  if (value == NULL) {
    return false;
  }

  for (size_t i = 0U; i < length; i++) {
    if (value[i] == '-') {
      return false;
    }
  }

  return true;
}

msgdb_span msgdb_id_span(const char *value, size_t length) {
  msgdb_span span = {0U, 0U, false};

  if (value == NULL) {
    return span;
  }

  for (size_t i = 0U; i < length; i++) {
    if (value[i] == '-') {
      span.offset = i + 1U;
      span.length = length - span.offset;
      span.present = true;
      return span;
    }
  }

  return span;
}

msgdb_span msgdb_cardinal_id_span(const char *value, size_t length) {
  msgdb_span span = msgdb_id_span(value, length);

  if (!span.present || value == NULL) {
    return span;
  }

  for (size_t i = span.offset; i < length; i++) {
    if (value[i] == '+') {
      span.length = i - span.offset;
      break;
    }
  }

  return span;
}

int64_t msgdb_hash64(const unsigned char *value, size_t length) {
  unsigned char digest[16] = {0U};
  uint64_t high_bits = 0U;
  msgdb_md5_ctx ctx;

  md5_init(&ctx);
  md5_update(&ctx, value, length);
  md5_final(&ctx, digest);

  for (size_t i = 0U; i < 8U; i++) {
    high_bits = (high_bits << 8U) | (uint64_t)digest[i];
  }

  return signed_i64_from_u64(high_bits);
}

bool msgdb_uuid_v4_canonical(const char *value, size_t length, char out[37]) {
  static const size_t hyphen_positions[] = {8U, 13U, 18U, 23U};

  if (value == NULL || out == NULL || length != 36U) {
    return false;
  }

  for (size_t i = 0U; i < length; i++) {
    bool is_hyphen = false;

    for (size_t j = 0U; j < sizeof(hyphen_positions) / sizeof(hyphen_positions[0]); j++) {
      if (i == hyphen_positions[j]) {
        is_hyphen = true;
        break;
      }
    }

    if (is_hyphen) {
      if (value[i] != '-') {
        return false;
      }
      out[i] = '-';
      continue;
    }

    if (!is_hex(value[i])) {
      return false;
    }

    out[i] = lower_hex(value[i]);
  }

  if (out[14] != '4') {
    return false;
  }

  if (out[19] != '8' && out[19] != '9' && out[19] != 'a' && out[19] != 'b') {
    return false;
  }

  out[36] = '\0';
  return true;
}

void msgdb_uuid_v4_from_bytes(const unsigned char bytes[16], char out[37]) {
  static const char hex[] = "0123456789abcdef";
  unsigned char uuid[16] = {0U};
  size_t out_index = 0U;

  if (bytes == NULL || out == NULL) {
    return;
  }

  memcpy(uuid, bytes, sizeof(uuid));
  uuid[6] = (unsigned char)((uuid[6] & 0x0FU) | 0x40U);
  uuid[8] = (unsigned char)((uuid[8] & 0x3FU) | 0x80U);

  for (size_t i = 0U; i < sizeof(uuid); i++) {
    if (i == 4U || i == 6U || i == 8U || i == 10U) {
      out[out_index] = '-';
      out_index++;
    }

    out[out_index] = hex[(uuid[i] >> 4U) & 0x0FU];
    out_index++;
    out[out_index] = hex[uuid[i] & 0x0FU];
    out_index++;
  }

  out[out_index] = '\0';
}

static uint32_t rotl32(uint32_t value, unsigned int count) {
  return (value << count) | (value >> (32U - count));
}

static uint32_t read_le32(const unsigned char *bytes) {
  return ((uint32_t)bytes[0]) | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
         ((uint32_t)bytes[3] << 24U);
}

static void write_le64(unsigned char *bytes, uint64_t value) {
  for (size_t i = 0U; i < 8U; i++) {
    bytes[i] = (unsigned char)((value >> (8U * i)) & 0xFFU);
  }
}

static void md5_init(msgdb_md5_ctx *ctx) {
  ctx->state[0] = 0x67452301U;
  ctx->state[1] = 0xefcdab89U;
  ctx->state[2] = 0x98badcfeU;
  ctx->state[3] = 0x10325476U;
  ctx->total_length = 0U;
  ctx->buffer_length = 0U;
  memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void md5_update(msgdb_md5_ctx *ctx, const unsigned char *data, size_t length) {
  size_t offset = 0U;

  if (length == 0U) {
    return;
  }

  ctx->total_length += (uint64_t)length;

  if (ctx->buffer_length > 0U) {
    size_t needed = sizeof(ctx->buffer) - ctx->buffer_length;
    size_t to_copy = length < needed ? length : needed;

    memcpy(ctx->buffer + ctx->buffer_length, data, to_copy);
    ctx->buffer_length += to_copy;
    offset += to_copy;

    if (ctx->buffer_length == sizeof(ctx->buffer)) {
      md5_transform(ctx, ctx->buffer);
      ctx->buffer_length = 0U;
    }
  }

  while (offset + sizeof(ctx->buffer) <= length) {
    md5_transform(ctx, data + offset);
    offset += sizeof(ctx->buffer);
  }

  if (offset < length) {
    ctx->buffer_length = length - offset;
    memcpy(ctx->buffer, data + offset, ctx->buffer_length);
  }
}

static void md5_final(msgdb_md5_ctx *ctx, unsigned char digest[16]) {
  unsigned char length_bytes[8] = {0U};
  unsigned char pad[64] = {0x80U};
  size_t pad_length;
  uint64_t bit_length = ctx->total_length * 8U;

  write_le64(length_bytes, bit_length);

  if (ctx->buffer_length < 56U) {
    pad_length = 56U - ctx->buffer_length;
  } else {
    pad_length = 120U - ctx->buffer_length;
  }

  md5_update(ctx, pad, pad_length);
  md5_update(ctx, length_bytes, sizeof(length_bytes));

  for (size_t i = 0U; i < 4U; i++) {
    digest[(i * 4U)] = (unsigned char)(ctx->state[i] & 0xFFU);
    digest[(i * 4U) + 1U] = (unsigned char)((ctx->state[i] >> 8U) & 0xFFU);
    digest[(i * 4U) + 2U] = (unsigned char)((ctx->state[i] >> 16U) & 0xFFU);
    digest[(i * 4U) + 3U] = (unsigned char)((ctx->state[i] >> 24U) & 0xFFU);
  }
}

static void md5_transform(msgdb_md5_ctx *ctx, const unsigned char block[64]) {
  static const uint32_t shift[] = {
      7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U,
      5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U,
      4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U,
      6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U};
  static const uint32_t sine[] = {
      0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU,
      0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
      0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U,
      0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
      0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U,
      0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
      0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
      0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
      0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U,
      0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
      0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};
  uint32_t words[16] = {0U};
  uint32_t a = ctx->state[0];
  uint32_t b = ctx->state[1];
  uint32_t c = ctx->state[2];
  uint32_t d = ctx->state[3];

  for (size_t i = 0U; i < 16U; i++) {
    words[i] = read_le32(block + (i * 4U));
  }

  for (size_t i = 0U; i < 64U; i++) {
    uint32_t f;
    size_t g;
    uint32_t tmp;

    if (i < 16U) {
      f = (b & c) | ((~b) & d);
      g = i;
    } else if (i < 32U) {
      f = (d & b) | ((~d) & c);
      g = ((5U * i) + 1U) % 16U;
    } else if (i < 48U) {
      f = b ^ c ^ d;
      g = ((3U * i) + 5U) % 16U;
    } else {
      f = c ^ (b | (~d));
      g = (7U * i) % 16U;
    }

    tmp = d;
    d = c;
    c = b;
    b += rotl32(a + f + sine[i] + words[g], shift[i]);
    a = tmp;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
}

static bool is_hex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

static char lower_hex(char value) {
  if (value >= 'A' && value <= 'F') {
    return (char)(value + ('a' - 'A'));
  }

  return value;
}

static int64_t signed_i64_from_u64(uint64_t value) {
  if (value <= (uint64_t)INT64_MAX) {
    return (int64_t)value;
  }

  return -1 - (int64_t)(UINT64_MAX - value);
}
