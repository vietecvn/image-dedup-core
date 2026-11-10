#include "image_dedup.h"

#include <algorithm>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Ngưỡng so sánh (đơn vị: số bit lệch trên 64).
// Đo bằng test: cùng ảnh qua thu nhỏ / nhiễu nhẹ lệch 0-4 bit; ảnh khác nhau
// thường lệch 20-40 bit.
// ---------------------------------------------------------------------------
constexpr int kDuplicatePhash = 6;
constexpr int kDuplicateDhash = 10;
constexpr int kSimilarPhash = 12;
constexpr int kSimilarDhash = 16;

constexpr int kDctSize = 32;
constexpr int kDctKeep = 8;

// cos((2x + 1) * u * PI / 64) * 2^14, làm tròn. Nhúng cứng thay vì gọi std::cos
// vì mỗi libm (Emscripten, bionic, Apple) có thể lệch bit cuối, làm lật bit hash.
constexpr int32_t kCos[kDctKeep][kDctSize] = {
    {16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384,
     16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384,
     16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384},
    {16364,  16207,  15893,  15426,  14811,  14053,  13160,  12140,
     11003,  9760,   8423,   7005,   5520,   3981,   2404,   804,
     -804,   -2404,  -3981,  -5520,  -7005,  -8423,  -9760,  -11003,
     -12140, -13160, -14053, -14811, -15426, -15893, -16207, -16364},
    {16305,  15679,  14449,  12665,  10394,  7723,   4756,   1606,
     -1606,  -4756,  -7723,  -10394, -12665, -14449, -15679, -16305,
     -16305, -15679, -14449, -12665, -10394, -7723,  -4756,  -1606,
     1606,   4756,   7723,   10394,  12665,  14449,  15679,  16305},
    {16207, 14811,  12140,  8423,   3981,   -804,   -5520,  -9760,
     -13160, -15426, -16364, -15893, -14053, -11003, -7005, -2404,
     2404,  7005,   11003,  14053,  15893,  16364,  15426,  13160,
     9760,  5520,   804,    -3981,  -8423,  -12140, -14811, -16207},
    {16069,  13623,  9102,   3196,   -3196,  -9102,  -13623, -16069,
     -16069, -13623, -9102,  -3196,  3196,   9102,   13623,  16069,
     16069,  13623,  9102,   3196,   -3196,  -9102,  -13623, -16069,
     -16069, -13623, -9102,  -3196,  3196,   9102,   13623,  16069},
    {15893, 12140,  5520,   -2404,  -9760,  -14811, -16364, -14053,
     -8423, -804,   7005,   13160,  16207,  15426,  11003,  3981,
     -3981, -11003, -15426, -16207, -13160, -7005,  804,    8423,
     14053, 16364,  14811,  9760,   2404,   -5520,  -12140, -15893},
    {15679,  10394,  1606,   -7723,  -14449, -16305, -12665, -4756,
     4756,   12665,  16305,  14449,  7723,   -1606,  -10394, -15679,
     -15679, -10394, -1606,  7723,   14449,  16305,  12665,  4756,
     -4756,  -12665, -16305, -14449, -7723,  1606,   10394,  15679},
    {15426, 8423,   -2404,  -12140, -16364, -13160, -3981,  7005,
     14811, 15893,  9760,   -804,   -11003, -16207, -14053, -5520,
     5520,  14053,  16207,  11003,  804,    -9760,  -15893, -14811,
     -7005, 3981,   13160,  16364,  12140,  2404,   -8423,  -15426},
};

struct Image {
  const uint8_t *pixels;
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t channels;
};

// Độ sáng 0..255 của một pixel, số nguyên thuần (BT.601: 77/150/29 trên 256).
// Pixel trong suốt được phủ lên nền trắng để PNG nền trong và JPEG nền trắng
// của cùng một ảnh cho ra cùng hash.
inline uint32_t LumaAt(const Image &image, int32_t x, int32_t y) {
  const uint8_t *p = image.pixels + static_cast<size_t>(y) * image.stride +
                     static_cast<size_t>(x) * image.channels;
  uint32_t r = p[0];
  uint32_t g = p[1];
  uint32_t b = p[2];
  if (image.channels == 4) {
    const uint32_t a = p[3];
    if (a != 255) {
      const uint32_t white = 255 * (255 - a);
      r = (r * a + white + 127) / 255;
      g = (g * a + white + 127) / 255;
      b = (b * a + white + 127) / 255;
    }
  }
  return (77 * r + 150 * g + 29 * b + 128) >> 8;
}

// Thu nhỏ về out_w x out_h bằng trung bình vùng (area average), chỉ số nguyên.
// Mỗi ô lấy đúng hộp pixel [x0, x1) x [y0, y1); ảnh nhỏ hơn lưới thì ô rỗng
// được mở rộng thành 1 pixel (lấy pixel gần nhất).
void AreaDownsample(const Image &image, int32_t out_w, int32_t out_h,
                    uint32_t *out) {
  for (int32_t oy = 0; oy < out_h; ++oy) {
    int32_t y0 = static_cast<int32_t>(static_cast<int64_t>(oy) * image.height /
                                      out_h);
    int32_t y1 = static_cast<int32_t>(static_cast<int64_t>(oy + 1) *
                                      image.height / out_h);
    if (y1 <= y0) y1 = y0 + 1;
    for (int32_t ox = 0; ox < out_w; ++ox) {
      int32_t x0 = static_cast<int32_t>(static_cast<int64_t>(ox) * image.width /
                                        out_w);
      int32_t x1 = static_cast<int32_t>(static_cast<int64_t>(ox + 1) *
                                        image.width / out_w);
      if (x1 <= x0) x1 = x0 + 1;

      uint64_t sum = 0;
      for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
          sum += LumaAt(image, x, y);
        }
      }
      const uint64_t count = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
      out[oy * out_w + ox] = static_cast<uint32_t>((sum + count / 2) / count);
    }
  }
}

// pHash: ảnh xám 32x32 -> DCT 2 chiều (chỉ tính 8x8 tần số thấp) -> mỗi bit
// là "hệ số lớn hơn trung vị". Trung vị lấy trên 63 hệ số, bỏ thành phần DC
// vì nó chỉ phản ánh độ sáng trung bình.
uint64_t ComputePhash(const Image &image) {
  uint32_t gray[kDctSize * kDctSize];
  AreaDownsample(image, kDctSize, kDctSize, gray);

  // Biến đổi theo hàng: rows[y][u] = sum_x cos[u][x] * gray[y][x].
  int64_t rows[kDctSize][kDctKeep];
  for (int y = 0; y < kDctSize; ++y) {
    for (int u = 0; u < kDctKeep; ++u) {
      int64_t acc = 0;
      for (int x = 0; x < kDctSize; ++x) {
        acc += static_cast<int64_t>(kCos[u][x]) * gray[y * kDctSize + x];
      }
      rows[y][u] = acc;
    }
  }

  // Biến đổi theo cột: coef[v][u] = sum_y cos[v][y] * rows[y][u].
  int64_t coef[kDctKeep * kDctKeep];
  for (int v = 0; v < kDctKeep; ++v) {
    for (int u = 0; u < kDctKeep; ++u) {
      int64_t acc = 0;
      for (int y = 0; y < kDctSize; ++y) {
        acc += static_cast<int64_t>(kCos[v][y]) * rows[y][u];
      }
      coef[v * kDctKeep + u] = acc;
    }
  }

  int64_t sorted[kDctKeep * kDctKeep - 1];
  std::memcpy(sorted, coef + 1, sizeof(sorted));
  std::nth_element(sorted, sorted + 31, sorted + 63);
  const int64_t median = sorted[31];

  uint64_t hash = 0;
  for (int i = 0; i < kDctKeep * kDctKeep; ++i) {
    hash = (hash << 1) | (coef[i] > median ? 1u : 0u);
  }
  return hash;
}

// dHash: ảnh xám 9x8 -> mỗi bit là "pixel bên trái sáng hơn pixel bên phải".
// Bổ trợ pHash: nhạy với bố cục, ít bị đánh lừa bởi ảnh cùng tông màu.
uint64_t ComputeDhash(const Image &image) {
  uint32_t gray[9 * 8];
  AreaDownsample(image, 9, 8, gray);
  uint64_t hash = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      hash = (hash << 1) | (gray[y * 9 + x] > gray[y * 9 + x + 1] ? 1u : 0u);
    }
  }
  return hash;
}

void WriteBigEndian(uint64_t value, uint8_t *out) {
  for (int i = 7; i >= 0; --i) {
    out[i] = static_cast<uint8_t>(value & 0xff);
    value >>= 8;
  }
}

uint64_t ReadBigEndian(const uint8_t *in) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | in[i];
  return value;
}

int PopCount(uint64_t value) {
  int count = 0;
  while (value) {
    value &= value - 1;
    ++count;
  }
  return count;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), cài gọn để thư viện không phụ thuộc gì bên ngoài.
// ---------------------------------------------------------------------------
constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256Block(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
    const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

extern "C" {

int32_t ihd_algorithm_version(void) { return IHD_ALGORITHM_VERSION; }

int32_t ihd_fingerprint_rgba(const uint8_t *pixels, int32_t width,
                             int32_t height, int32_t stride, int32_t channels,
                             uint8_t *out) {
  if (!pixels || !out) return IHD_ERR_NULL_POINTER;
  if (width <= 0 || height <= 0) return IHD_ERR_INVALID_SIZE;
  if (channels != 3 && channels != 4) return IHD_ERR_INVALID_CHANNELS;
  if (static_cast<int64_t>(stride) < static_cast<int64_t>(width) * channels) {
    return IHD_ERR_INVALID_STRIDE;
  }

  const Image image{pixels, width, height, stride, channels};
  WriteBigEndian(ComputePhash(image), out);
  WriteBigEndian(ComputeDhash(image), out + 8);
  return IHD_OK;
}

int32_t ihd_fingerprint_to_hex(const uint8_t *fingerprint, char *out) {
  if (!fingerprint || !out) return IHD_ERR_NULL_POINTER;
  static const char kDigits[] = "0123456789abcdef";
  for (int i = 0; i < IHD_FINGERPRINT_SIZE; ++i) {
    out[i * 2] = kDigits[fingerprint[i] >> 4];
    out[i * 2 + 1] = kDigits[fingerprint[i] & 0x0f];
  }
  out[IHD_FINGERPRINT_HEX_LENGTH] = '\0';
  return IHD_OK;
}

int32_t ihd_fingerprint_from_hex(const char *hex, uint8_t *out) {
  if (!hex || !out) return IHD_ERR_NULL_POINTER;
  for (int i = 0; i < IHD_FINGERPRINT_SIZE; ++i) {
    const int hi = HexValue(hex[i * 2]);
    if (hi < 0) return IHD_ERR_INVALID_HEX;
    const int lo = HexValue(hex[i * 2 + 1]);
    if (lo < 0) return IHD_ERR_INVALID_HEX;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  if (hex[IHD_FINGERPRINT_HEX_LENGTH] != '\0') return IHD_ERR_INVALID_HEX;
  return IHD_OK;
}

int32_t ihd_compare(const uint8_t *a, const uint8_t *b, ihd_match *out) {
  if (!a || !b || !out) return IHD_ERR_NULL_POINTER;
  const int phash = PopCount(ReadBigEndian(a) ^ ReadBigEndian(b));
  const int dhash = PopCount(ReadBigEndian(a + 8) ^ ReadBigEndian(b + 8));
  out->phash_distance = phash;
  out->dhash_distance = dhash;
  if (phash <= kDuplicatePhash && dhash <= kDuplicateDhash) {
    out->verdict = IHD_VERDICT_DUPLICATE;
  } else if (phash <= kSimilarPhash && dhash <= kSimilarDhash) {
    out->verdict = IHD_VERDICT_SIMILAR;
  } else {
    out->verdict = IHD_VERDICT_DIFFERENT;
  }
  return IHD_OK;
}

int32_t ihd_compare_into(const uint8_t *a, const uint8_t *b, int32_t *out3) {
  if (!out3) return IHD_ERR_NULL_POINTER;
  ihd_match match;
  const int32_t status = ihd_compare(a, b, &match);
  if (status != IHD_OK) return status;
  out3[0] = match.phash_distance;
  out3[1] = match.dhash_distance;
  out3[2] = match.verdict;
  return IHD_OK;
}

int32_t ihd_sha256(const uint8_t *data, size_t length, uint8_t *out) {
  if (!out || (!data && length > 0)) return IHD_ERR_NULL_POINTER;
  uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

  size_t offset = 0;
  while (length - offset >= 64) {
    Sha256Block(state, data + offset);
    offset += 64;
  }

  uint8_t tail[128] = {0};
  const size_t rest = length - offset;
  if (rest > 0) std::memcpy(tail, data + offset, rest);
  tail[rest] = 0x80;
  const size_t tail_size = rest < 56 ? 64 : 128;
  const uint64_t bits = static_cast<uint64_t>(length) * 8;
  for (int i = 0; i < 8; ++i) {
    tail[tail_size - 1 - i] = static_cast<uint8_t>(bits >> (i * 8));
  }
  Sha256Block(state, tail);
  if (tail_size == 128) Sha256Block(state, tail + 64);

  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>(state[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(state[i]);
  }
  return IHD_OK;
}

}  // extern "C"
