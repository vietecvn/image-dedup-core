// Pipeline xử lý ảnh: đọc byte -> metadata (EXIF, ICC) -> giải mã -> thu nhỏ ->
// chuyển màu P3 -> xoay EXIF -> JPEG -> dấu vân tay.
//
// Mọi phép xử lý pixel ở đây là số nguyên (kể cả chuyển màu: bảng tra nhúng
// sẵn, ma trận dấu phẩy tĩnh); phần float duy nhất nằm trong bộ mã hoá stb (xem
// stb_impl.cpp về cờ chống FMA). Không dùng exception: bản WASM tắt exception,
// nên mọi cấp phát đều kiểm tra NULL và trả mã lỗi.
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "image_dedup.h"
#include "src/webp/decode.h"  // third_party/libwebp (cần -Ithird_party/libwebp)
#include "stb_bridge.h"

struct ihd_result {
  int32_t status;
  uint8_t *jpeg;
  size_t jpeg_size;
  int32_t width;
  int32_t height;
  int32_t quality;
  int32_t source_width;
  int32_t source_height;
  int32_t orientation;
  int32_t color_converted;
  size_t source_size;
  char fingerprint_hex[IHD_FINGERPRINT_HEX_LENGTH + 1];
  char source_sha256_hex[IHD_SHA256_SIZE * 2 + 1];
  char output_sha256_hex[IHD_SHA256_SIZE * 2 + 1];
  char capture_time[20];
  char capture_offset[8];
};

namespace {

// Giống compressImage.ts.
constexpr int kQualitySteps[] = {82, 72, 62, 50};
constexpr int kMaxEdgeReductions = 3;
// Dấu vân tay tính trên bản thu nhỏ cạnh dài 512: đủ chi tiết cho lưới 32x32,
// rẻ hơn nhiều so với tính trên ảnh gốc.
constexpr int32_t kFingerprintMaxEdge = 512;

// Bộ đệm tự giải phóng (malloc/free, không exception).
class Buffer {
 public:
  Buffer() = default;
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  ~Buffer() { std::free(data_); }

  bool Reserve(size_t size) {
    if (size <= capacity_) return true;
    void *next = std::realloc(data_, size);
    if (!next) return false;
    data_ = static_cast<uint8_t *>(next);
    capacity_ = size;
    return true;
  }
  uint8_t *data() { return data_; }
  uint8_t *Release() {
    uint8_t *out = data_;
    data_ = nullptr;
    capacity_ = 0;
    return out;
  }

 private:
  uint8_t *data_ = nullptr;
  size_t capacity_ = 0;
};

void ToHex(const uint8_t *bytes, size_t length, char *out) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    out[i * 2] = kDigits[bytes[i] >> 4];
    out[i * 2 + 1] = kDigits[bytes[i] & 0x0f];
  }
  out[length * 2] = '\0';
}

// --- Nhận định dạng từ vài byte đầu ----------------------------------------
enum class Format { kUnknown, kJpeg, kPng, kGif, kBmp, kWebp, kUnsupported };

Format SniffFormat(const uint8_t *d, size_t n) {
  if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) return Format::kJpeg;
  if (n >= 8 && d[0] == 0x89 && std::memcmp(d + 1, "PNG", 3) == 0) return Format::kPng;
  if (n >= 3 && std::memcmp(d, "GIF", 3) == 0) return Format::kGif;
  if (n >= 2 && d[0] == 'B' && d[1] == 'M') return Format::kBmp;
  if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) {
    return Format::kWebp;
  }
  if (n >= 12 && std::memcmp(d + 4, "ftyp", 4) == 0) {
    static const char *kImageBrands[] = {"heic", "heix", "heim", "heis", "hevc",
                                         "hevx", "mif1", "msf1", "avif", "avis"};
    for (const char *brand : kImageBrands) {
      if (std::memcmp(d + 8, brand, 4) == 0) return Format::kUnsupported;
    }
  }
  return Format::kUnknown;
}

uint32_t ReadU16(const uint8_t *p, bool little) {
  return little ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]);
}

uint32_t ReadU32(const uint8_t *p, bool little) {
  return little ? (p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24))
                : ((static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

// --- Metadata: EXIF (xoay, ngày chụp) + ICC (nhận Display P3) --------------
struct Metadata {
  int orientation = 1;
  char capture_time[20] = {0};
  char capture_offset[8] = {0};
  bool display_p3 = false;
};

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// "YYYY:MM:DD HH:MM:SS" (không nhận "0000:..." của máy chưa đặt giờ).
bool IsExifDateTime(const char *s) {
  static const char kPattern[] = "dddd:dd:dd dd:dd:dd";
  for (int i = 0; i < 19; ++i) {
    if (kPattern[i] == 'd' ? !IsDigit(s[i]) : s[i] != kPattern[i]) return false;
  }
  return std::memcmp(s, "0000", 4) != 0;
}

// "+07:00" / "-05:30".
bool IsExifOffset(const char *s) {
  return (s[0] == '+' || s[0] == '-') && IsDigit(s[1]) && IsDigit(s[2]) && s[3] == ':' &&
         IsDigit(s[4]) && IsDigit(s[5]);
}

// Đọc giá trị ASCII (kiểu 2) của một entry IFD vào out, chỉ khi hợp lệ.
void ReadExifAscii(const uint8_t *t, size_t n, bool little, size_t entry, char *out,
                   size_t out_size, bool (*valid)(const char *)) {
  if (ReadU16(t + entry + 2, little) != 2) return;
  const uint32_t count = ReadU32(t + entry + 4, little);
  const size_t offset = count <= 4 ? entry + 8 : ReadU32(t + entry + 8, little);
  const size_t needed = out_size - 1;
  if (count < needed || offset > n || n - offset < needed) return;
  char value[32];
  std::memcpy(value, t + offset, needed);
  value[needed] = '\0';
  if (valid(value)) std::memcpy(out, value, needed + 1);
}

// Gọi visit(tag, entry_offset) cho mọi entry của IFD tại ifd.
template <typename Visit>
void ForEachIfdEntry(const uint8_t *t, size_t n, bool little, uint32_t ifd, Visit visit) {
  if (ifd > n || n - ifd < 2) return;
  const uint32_t count = ReadU16(t + ifd, little);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t entry = static_cast<size_t>(ifd) + 2 + static_cast<size_t>(i) * 12;
    if (entry + 12 > n) return;
    visit(ReadU16(t + entry, little), entry);
  }
}

void ParseTiff(const uint8_t *t, size_t n, Metadata *meta) {
  if (n < 8) return;
  bool little;
  if (t[0] == 'I' && t[1] == 'I') {
    little = true;
  } else if (t[0] == 'M' && t[1] == 'M') {
    little = false;
  } else {
    return;
  }
  if (ReadU16(t + 2, little) != 42) return;

  uint32_t exif_ifd = 0;
  char fallback_time[20] = {0};
  ForEachIfdEntry(t, n, little, ReadU32(t + 4, little), [&](uint32_t tag, size_t entry) {
    if (tag == 0x0112) {  // Orientation
      const uint32_t value = ReadU16(t + entry + 8, little);
      if (value >= 1 && value <= 8) meta->orientation = static_cast<int>(value);
    } else if (tag == 0x0132) {  // DateTime (giờ sửa file, dùng khi thiếu giờ chụp)
      ReadExifAscii(t, n, little, entry, fallback_time, sizeof(fallback_time), IsExifDateTime);
    } else if (tag == 0x8769) {  // con trỏ tới Exif IFD
      exif_ifd = ReadU32(t + entry + 8, little);
    }
  });
  if (exif_ifd) {
    ForEachIfdEntry(t, n, little, exif_ifd, [&](uint32_t tag, size_t entry) {
      if (tag == 0x9003) {  // DateTimeOriginal
        ReadExifAscii(t, n, little, entry, meta->capture_time, sizeof(meta->capture_time),
                      IsExifDateTime);
      } else if (tag == 0x9011) {  // OffsetTimeOriginal
        ReadExifAscii(t, n, little, entry, meta->capture_offset, sizeof(meta->capture_offset),
                      IsExifOffset);
      }
    });
  }
  if (!meta->capture_time[0] && fallback_time[0]) {
    std::memcpy(meta->capture_time, fallback_time, sizeof(fallback_time));
  }
}

// Display P3 (profile của Apple): primaries rXYZ/gXYZ/bXYZ dạng s15Fixed16,
// đã thích nghi D50. Đọc từ /System/Library/ColorSync/Profiles/Display P3.icc.
constexpr int32_t kP3Primaries[3][3] = {
    {33759, 15807, -69}, {19135, 45367, 2745}, {10296, 4363, 51385}};
constexpr int32_t kPrimaryTolerance = 655;  // 0.01

bool IsDisplayP3(const uint8_t *icc, size_t n) {
  if (n < 132) return false;
  static const char *kTags[3] = {"rXYZ", "gXYZ", "bXYZ"};
  bool matched[3] = {false, false, false};
  const uint32_t count = ReadU32(icc + 128, false);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = 132 + static_cast<size_t>(i) * 12;
    if (at + 12 > n) break;
    for (int c = 0; c < 3; ++c) {
      if (std::memcmp(icc + at, kTags[c], 4) != 0) continue;
      const uint32_t offset = ReadU32(icc + at + 4, false);
      if (offset > n || n - offset < 20 || std::memcmp(icc + offset, "XYZ ", 4) != 0) return false;
      for (int k = 0; k < 3; ++k) {
        const int32_t value = static_cast<int32_t>(ReadU32(icc + offset + 8 + k * 4, false));
        const int32_t diff = value - kP3Primaries[c][k];
        if (diff > kPrimaryTolerance || diff < -kPrimaryTolerance) return false;
      }
      matched[c] = true;
    }
  }
  return matched[0] && matched[1] && matched[2];
}

void ReadJpegMetadata(const uint8_t *d, size_t n, Metadata *meta) {
  // ICC có thể chia thành nhiều chunk APP2 ("ICC_PROFILE\0" + số thứ tự + tổng).
  const uint8_t *chunks[256] = {nullptr};
  size_t chunk_sizes[256] = {0};
  int chunk_total = 0;
  bool exif_done = false;

  size_t p = 2;
  while (p + 4 <= n) {
    if (d[p] != 0xff) break;
    const uint8_t marker = d[p + 1];
    if (marker == 0xff) {
      ++p;
      continue;
    }
    if (marker == 0xd9 || marker == 0xda) break;
    if ((marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) {
      p += 2;
      continue;
    }
    const size_t length = (static_cast<size_t>(d[p + 2]) << 8) | d[p + 3];
    if (length < 2 || p + 2 + length > n) break;
    const uint8_t *body = d + p + 4;
    const size_t body_size = length - 2;
    if (marker == 0xe1 && !exif_done && body_size >= 14 && std::memcmp(body, "Exif\0\0", 6) == 0) {
      ParseTiff(body + 6, body_size - 6, meta);
      exif_done = true;
    } else if (marker == 0xe2 && body_size > 14 && std::memcmp(body, "ICC_PROFILE\0", 12) == 0) {
      const int seq = body[12];
      const int total = body[13];
      if (seq >= 1 && total >= 1 && seq <= total && (chunk_total == 0 || chunk_total == total)) {
        chunk_total = total;
        chunks[seq] = body + 14;
        chunk_sizes[seq] = body_size - 14;
      }
    }
    p += 2 + length;
  }

  if (chunk_total == 0) return;
  size_t icc_size = 0;
  for (int i = 1; i <= chunk_total; ++i) {
    if (!chunks[i]) return;  // thiếu chunk -> bỏ qua ICC
    icc_size += chunk_sizes[i];
  }
  Buffer icc;
  if (!icc.Reserve(icc_size)) return;
  size_t offset = 0;
  for (int i = 1; i <= chunk_total; ++i) {
    std::memcpy(icc.data() + offset, chunks[i], chunk_sizes[i]);
    offset += chunk_sizes[i];
  }
  meta->display_p3 = IsDisplayP3(icc.data(), icc_size);
}

void ReadPngMetadata(const uint8_t *d, size_t n, Metadata *meta) {
  size_t p = 8;
  while (p + 12 <= n) {
    const size_t length = ReadU32(d + p, false);
    if (length > n - p - 12) return;
    const uint8_t *type = d + p + 4;
    const uint8_t *body = d + p + 8;
    if (std::memcmp(type, "IDAT", 4) == 0 || std::memcmp(type, "IEND", 4) == 0) return;
    if (std::memcmp(type, "iCCP", 4) == 0) {
      // tên profile (1..79 byte) + '\0' + phương thức nén (0) + dữ liệu zlib.
      const void *nul = std::memchr(body, 0, length < 80 ? length : 80);
      if (nul) {
        const size_t header = static_cast<size_t>(static_cast<const uint8_t *>(nul) - body) + 2;
        if (header < length && body[header - 1] == 0) {
          int icc_size = 0;
          char *icc = ihd_stb_inflate(reinterpret_cast<const char *>(body + header),
                                      static_cast<int>(length - header), &icc_size);
          if (icc) {
            meta->display_p3 =
                IsDisplayP3(reinterpret_cast<uint8_t *>(icc), static_cast<size_t>(icc_size));
            ihd_stb_free(icc);
          }
        }
      }
    } else if (std::memcmp(type, "eXIf", 4) == 0) {
      ParseTiff(body, length, meta);
    }
    p += 12 + length;
  }
}

void ReadWebpMetadata(const uint8_t *d, size_t n, Metadata *meta) {
  size_t p = 12;
  while (p + 8 <= n) {
    const size_t length = ReadU32(d + p + 4, true);
    if (length > n - p - 8) return;
    const uint8_t *body = d + p + 8;
    if (std::memcmp(d + p, "ICCP", 4) == 0) {
      meta->display_p3 = IsDisplayP3(body, length);
    } else if (std::memcmp(d + p, "EXIF", 4) == 0) {
      // Một số trình ghi để kèm tiền tố "Exif\0\0".
      if (length >= 6 && std::memcmp(body, "Exif\0\0", 6) == 0) {
        ParseTiff(body + 6, length - 6, meta);
      } else {
        ParseTiff(body, length, meta);
      }
    }
    p += 8 + length + (length & 1);
  }
}

Metadata ReadMetadata(Format format, const uint8_t *d, size_t n) {
  Metadata meta;
  if (format == Format::kJpeg) ReadJpegMetadata(d, n, &meta);
  if (format == Format::kPng) ReadPngMetadata(d, n, &meta);
  if (format == Format::kWebp) ReadWebpMetadata(d, n, &meta);
  return meta;
}

// --- JPEG có đầy đủ tới marker EOI không --------------------------------
// Không thể chỉ xem 2 byte cuối: ảnh "Motion Photo" của Samsung/Google gắn cả
// video MP4 SAU marker EOI. Nên phải duyệt cấu trúc: bỏ qua các segment theo độ
// dài, đi qua dữ liệu entropy (FF luôn đi kèm 00 hoặc RSTn), tới khi gặp EOI.
// Ảnh nhỏ nhúng trong APP1 có EOI riêng nhưng nằm trong segment nên bị bỏ qua.
bool IsJpegComplete(const uint8_t *d, size_t n) {
  if (n < 4 || d[0] != 0xff || d[1] != 0xd8) return true;  // không phải JPEG
  size_t p = 2;
  bool in_scan = false;
  while (p < n) {
    if (in_scan) {
      if (d[p] != 0xff) {
        ++p;
        continue;
      }
      if (p + 1 >= n) return false;
      const uint8_t next = d[p + 1];
      if (next == 0x00 || (next >= 0xd0 && next <= 0xd7) || next == 0xff) {
        p += (next == 0xff) ? 1 : 2;
        continue;
      }
      in_scan = false;
    }
    if (p + 1 >= n || d[p] != 0xff) return false;
    const uint8_t marker = d[p + 1];
    if (marker == 0xff) {
      ++p;
      continue;
    }
    if (marker == 0xd9) return true;  // EOI
    if ((marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) {
      p += 2;
      continue;
    }
    if (p + 4 > n) return false;
    const size_t length = (static_cast<size_t>(d[p + 2]) << 8) | d[p + 3];
    if (length < 2 || p + 2 + length > n) return false;
    p += 2 + length;
    if (marker == 0xda) in_scan = true;
  }
  return false;
}

// --- Giải mã ----------------------------------------------------------------
struct Decoded {
  uint8_t *pixels = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  int channels = 0;
  bool from_webp = false;

  Decoded() = default;
  Decoded(const Decoded &) = delete;
  Decoded &operator=(const Decoded &) = delete;
  ~Decoded() {
    if (!pixels) return;
    if (from_webp) {
      WebPFree(pixels);
    } else {
      ihd_stb_free(pixels);
    }
  }
};

int32_t DecodeImage(Format format, const uint8_t *d, size_t n, int64_t max_pixels, Decoded *out,
                    int32_t *source_w, int32_t *source_h) {
  if (n == 0) return IHD_ERR_FILE_UNREADABLE;
  if (n > IHD_MAX_INPUT_BYTES) return IHD_ERR_FILE_TOO_LARGE;
  if (format == Format::kUnsupported) return IHD_ERR_UNSUPPORTED_FORMAT;

  if (format == Format::kWebp) {
    WebPBitstreamFeatures features;
    const VP8StatusCode status = WebPGetFeatures(d, n, &features);
    if (status == VP8_STATUS_NOT_ENOUGH_DATA) return IHD_ERR_FILE_TRUNCATED;
    if (status != VP8_STATUS_OK) return IHD_ERR_DECODE_FAILED;
    if (features.has_animation) return IHD_ERR_UNSUPPORTED_FORMAT;  // WebP động
    *source_w = features.width;
    *source_h = features.height;
    if (static_cast<int64_t>(features.width) * features.height > max_pixels) {
      return IHD_ERR_TOO_MANY_PIXELS;
    }
    int w = 0, h = 0;
    out->from_webp = true;
    out->channels = features.has_alpha ? 4 : 3;
    out->pixels = features.has_alpha ? WebPDecodeRGBA(d, n, &w, &h) : WebPDecodeRGB(d, n, &w, &h);
    if (!out->pixels || w != features.width || h != features.height) {
      return IHD_ERR_DECODE_FAILED;
    }
    out->width = w;
    out->height = h;
    return IHD_OK;
  }

  // Đọc header trước, CHƯA giải mã: chặn ảnh quá lớn mà không tốn RAM.
  int width = 0, height = 0, components = 0;
  if (!ihd_stb_info(d, static_cast<int>(n), &width, &height, &components)) {
    if (format == Format::kUnknown) return IHD_ERR_NOT_AN_IMAGE;
    if (format == Format::kJpeg && !IsJpegComplete(d, n)) return IHD_ERR_FILE_TRUNCATED;
    return IHD_ERR_DECODE_FAILED;
  }
  *source_w = width;
  *source_h = height;
  if (static_cast<int64_t>(width) * height > max_pixels) return IHD_ERR_TOO_MANY_PIXELS;
  if (format == Format::kJpeg && !IsJpegComplete(d, n)) return IHD_ERR_FILE_TRUNCATED;

  out->channels = (components == 2 || components == 4) ? 4 : 3;
  int w = 0, h = 0;
  out->pixels = ihd_stb_load(d, static_cast<int>(n), &w, &h, out->channels);
  if (!out->pixels || w != width || h != height) return IHD_ERR_DECODE_FAILED;
  out->width = w;
  out->height = h;
  return IHD_OK;
}

// --- Thu nhỏ trung bình vùng, số nguyên ----------------------------------
// Ô đầu ra o phủ đoạn [o*src, (o+1)*src), pixel nguồn x phủ [x*dst, (x+1)*dst)
// trên cùng một thang -> trọng số = độ dài phần giao, tổng trọng số = src.
struct Spans {
  Buffer first;   // int32_t[dst]: pixel nguồn đầu tiên
  Buffer count;   // int32_t[dst]: số pixel nguồn
  Buffer offset;  // int32_t[dst]: vị trí trong mảng weight
  Buffer weight;  // int32_t[src + dst]
  int32_t total = 0;

  const int32_t *First() { return reinterpret_cast<int32_t *>(first.data()); }
  const int32_t *Count() { return reinterpret_cast<int32_t *>(count.data()); }
  const int32_t *Offset() { return reinterpret_cast<int32_t *>(offset.data()); }
  const int32_t *Weight() { return reinterpret_cast<int32_t *>(weight.data()); }
};

bool BuildSpans(int32_t src, int32_t dst, Spans *spans) {
  const size_t d = static_cast<size_t>(dst);
  if (!spans->first.Reserve(d * 4) || !spans->count.Reserve(d * 4) ||
      !spans->offset.Reserve(d * 4) ||
      !spans->weight.Reserve((static_cast<size_t>(src) + d) * 4)) {
    return false;
  }
  auto *first = reinterpret_cast<int32_t *>(spans->first.data());
  auto *count = reinterpret_cast<int32_t *>(spans->count.data());
  auto *offset = reinterpret_cast<int32_t *>(spans->offset.data());
  auto *weight = reinterpret_cast<int32_t *>(spans->weight.data());
  int32_t cursor = 0;
  for (int32_t o = 0; o < dst; ++o) {
    const int64_t lo = static_cast<int64_t>(o) * src;
    const int64_t hi = lo + src;
    const int32_t x0 = static_cast<int32_t>(lo / dst);
    const int32_t x1 = static_cast<int32_t>((hi + dst - 1) / dst);
    first[o] = x0;
    count[o] = x1 - x0;
    offset[o] = cursor;
    for (int32_t x = x0; x < x1; ++x) {
      const int64_t a = static_cast<int64_t>(x) * dst;
      const int64_t b = a + dst;
      weight[cursor++] = static_cast<int32_t>((b < hi ? b : hi) - (a > lo ? a : lo));
    }
  }
  spans->total = src;
  return true;
}

// Pixel nguồn -> RGB, phủ nền trắng nếu có alpha.
inline void SourceRgb(const uint8_t *p, int channels, uint32_t rgb[3]) {
  if (channels == 4 && p[3] != 255) {
    const uint32_t a = p[3];
    const uint32_t white = 255 * (255 - a);
    for (int c = 0; c < 3; ++c) rgb[c] = (p[c] * a + white + 127) / 255;
  } else {
    rgb[0] = p[0];
    rgb[1] = p[1];
    rgb[2] = p[2];
  }
}

bool Resize(const uint8_t *src, int32_t sw, int32_t sh, int channels, uint8_t *dst,
            int32_t dw, int32_t dh, Buffer *scratch) {
  Spans xs, ys;
  if (!BuildSpans(sw, dw, &xs) || !BuildSpans(sh, dh, &ys)) return false;
  if (!scratch->Reserve(static_cast<size_t>(dw) * sh * 3)) return false;
  uint8_t *tmp = scratch->data();

  for (int32_t y = 0; y < sh; ++y) {
    const uint8_t *row = src + static_cast<size_t>(y) * sw * channels;
    uint8_t *out = tmp + static_cast<size_t>(y) * dw * 3;
    for (int32_t ox = 0; ox < dw; ++ox) {
      uint64_t sum[3] = {0, 0, 0};
      const int32_t *w = xs.Weight() + xs.Offset()[ox];
      for (int32_t i = 0; i < xs.Count()[ox]; ++i) {
        uint32_t rgb[3];
        SourceRgb(row + static_cast<size_t>(xs.First()[ox] + i) * channels, channels, rgb);
        for (int c = 0; c < 3; ++c) sum[c] += static_cast<uint64_t>(rgb[c]) * w[i];
      }
      for (int c = 0; c < 3; ++c) {
        out[ox * 3 + c] = static_cast<uint8_t>((sum[c] + xs.total / 2) / xs.total);
      }
    }
  }

  for (int32_t oy = 0; oy < dh; ++oy) {
    const int32_t *w = ys.Weight() + ys.Offset()[oy];
    uint8_t *out = dst + static_cast<size_t>(oy) * dw * 3;
    for (int32_t x = 0; x < dw * 3; ++x) {
      uint64_t sum = 0;
      for (int32_t i = 0; i < ys.Count()[oy]; ++i) {
        sum += static_cast<uint64_t>(tmp[static_cast<size_t>(ys.First()[oy] + i) * dw * 3 + x]) * w[i];
      }
      out[x] = static_cast<uint8_t>((sum + ys.total / 2) / ys.total);
    }
  }
  return true;
}

void CopyRgb(const uint8_t *src, int32_t w, int32_t h, int channels, uint8_t *dst) {
  const size_t pixels = static_cast<size_t>(w) * h;
  for (size_t i = 0; i < pixels; ++i) {
    uint32_t rgb[3];
    SourceRgb(src + i * channels, channels, rgb);
    dst[i * 3] = static_cast<uint8_t>(rgb[0]);
    dst[i * 3 + 1] = static_cast<uint8_t>(rgb[1]);
    dst[i * 3 + 2] = static_cast<uint8_t>(rgb[2]);
  }
}

// --- Display P3 -> sRGB -----------------------------------------------------
// Hai không gian dùng chung đường cong sRGB, chỉ khác primaries. Làm hoàn toàn
// bằng số nguyên: giải gamma bằng bảng tra nhúng sẵn (không gọi pow), nhân ma
// trận dấu phẩy tĩnh Q14, mã hoá lại bằng tìm nhị phân trên chính bảng đó.
constexpr uint16_t kSrgbToLinear[256] = {
    0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219,
    241, 264, 288, 313, 340, 367, 396, 427, 458, 491, 526, 562,
    599, 637, 677, 718, 761, 805, 851, 898, 947, 997, 1048, 1101,
    1156, 1212, 1270, 1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863,
    1937, 2013, 2090, 2170, 2250, 2333, 2418, 2504, 2592, 2681, 2773, 2866,
    2961, 3058, 3157, 3258, 3360, 3464, 3570, 3678, 3788, 3900, 4014, 4129,
    4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124, 5257, 5392, 5530, 5669,
    5810, 5953, 6099, 6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500,
    7666, 7834, 8004, 8177, 8352, 8528, 8708, 8889, 9072, 9258, 9445, 9635,
    9828, 10022, 10219, 10417, 10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090,
    12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909, 14146, 14387, 14629, 14874,
    15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481,
    21787, 22096, 22407, 22721, 23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325,
    25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094, 28452, 28813, 29176, 29542,
    29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138,
    39572, 40009, 40449, 40891, 41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534,
    45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359, 48850, 49344, 49841, 50341,
    50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221,
    63795, 64372, 64952, 65535,
};

// Ma trận tuyến tính Display P3 -> sRGB (D65), nhân 2^14. Mỗi hàng cộng = 16384.
constexpr int32_t kP3ToSrgb[3][3] = {
    {20069, -3685, 0}, {-689, 17073, 0}, {-322, -1288, 17994}};

uint8_t LinearToSrgb(int64_t linear) {
  if (linear <= 0) return 0;
  if (linear >= 65535) return 255;
  // Giá trị 8 bit lớn nhất mà trung điểm với giá trị liền trước <= linear.
  int lo = 0, hi = 255;
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (2 * linear >= static_cast<int64_t>(kSrgbToLinear[mid - 1]) + kSrgbToLinear[mid]) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return static_cast<uint8_t>(lo);
}

void ConvertP3ToSrgb(uint8_t *rgb, size_t pixels) {
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t *p = rgb + i * 3;
    const int64_t r = kSrgbToLinear[p[0]];
    const int64_t g = kSrgbToLinear[p[1]];
    const int64_t b = kSrgbToLinear[p[2]];
    for (int c = 0; c < 3; ++c) {
      const int64_t sum = kP3ToSrgb[c][0] * r + kP3ToSrgb[c][1] * g + kP3ToSrgb[c][2] * b;
      // Chia có làm tròn, không dịch bit số âm (hành vi tuỳ trình biên dịch).
      p[c] = LinearToSrgb(sum <= 0 ? 0 : (sum + 8192) / 16384);
    }
  }
}

// Xoay/lật theo EXIF. Với orientation 5..8 thì đổi chiều rộng/cao.
void Orient(const uint8_t *src, int32_t w, int32_t h, int orientation, uint8_t *dst) {
  const bool swap = orientation >= 5;
  const int32_t ow = swap ? h : w;
  const int32_t oh = swap ? w : h;
  for (int32_t dy = 0; dy < oh; ++dy) {
    for (int32_t dx = 0; dx < ow; ++dx) {
      int32_t sx, sy;
      switch (orientation) {
        case 2: sx = w - 1 - dx; sy = dy; break;
        case 3: sx = w - 1 - dx; sy = h - 1 - dy; break;
        case 4: sx = dx; sy = h - 1 - dy; break;
        case 5: sx = dy; sy = dx; break;
        case 6: sx = dy; sy = h - 1 - dx; break;
        case 7: sx = w - 1 - dy; sy = h - 1 - dx; break;
        case 8: sx = w - 1 - dy; sy = dx; break;
        default: sx = dx; sy = dy; break;
      }
      std::memcpy(dst + (static_cast<size_t>(dy) * ow + dx) * 3,
                  src + (static_cast<size_t>(sy) * w + sx) * 3, 3);
    }
  }
}

// Kích thước sau khi giới hạn cạnh dài (làm tròn nửa lên, số nguyên).
void ScaledSize(int32_t w, int32_t h, int32_t max_edge, int32_t *ow, int32_t *oh) {
  const int32_t longest = w > h ? w : h;
  if (longest <= max_edge) {
    *ow = w;
    *oh = h;
    return;
  }
  const int64_t rw = (2 * static_cast<int64_t>(w) * max_edge + longest) / (2 * longest);
  const int64_t rh = (2 * static_cast<int64_t>(h) * max_edge + longest) / (2 * longest);
  *ow = rw < 1 ? 1 : static_cast<int32_t>(rw);
  *oh = rh < 1 ? 1 : static_cast<int32_t>(rh);
}

// Ảnh đã giải mã -> RGB ở cạnh dài max_edge, đúng màu sRGB, đúng chiều.
struct Rendered {
  uint8_t *rgb = nullptr;
  int32_t width = 0;
  int32_t height = 0;
};

bool Render(const Decoded &image, const Metadata &meta, int32_t max_edge, Buffer *a, Buffer *b,
            Buffer *scratch, Rendered *out) {
  int32_t w, h;
  ScaledSize(image.width, image.height, max_edge, &w, &h);
  const size_t size = static_cast<size_t>(w) * h * 3;
  if (!a->Reserve(size)) return false;
  if (w == image.width && h == image.height) {
    CopyRgb(image.pixels, w, h, image.channels, a->data());
  } else if (!Resize(image.pixels, image.width, image.height, image.channels, a->data(), w, h,
                     scratch)) {
    return false;
  }
  if (meta.display_p3) ConvertP3ToSrgb(a->data(), static_cast<size_t>(w) * h);
  if (meta.orientation == 1) {
    *out = {a->data(), w, h};
    return true;
  }
  if (!b->Reserve(size)) return false;
  Orient(a->data(), w, h, meta.orientation, b->data());
  const bool swap = meta.orientation >= 5;
  *out = {b->data(), swap ? h : w, swap ? w : h};
  return true;
}

int32_t FingerprintBytes(const uint8_t *data, size_t length, uint8_t *out) {
  if (!out || (!data && length > 0)) return IHD_ERR_NULL_POINTER;
  const Format format = SniffFormat(data, length);
  Decoded image;
  int32_t source_w = 0, source_h = 0;
  const int32_t status = DecodeImage(format, data, length, IHD_DEFAULT_MAX_SOURCE_PIXELS, &image,
                                     &source_w, &source_h);
  if (status != IHD_OK) return status;
  const Metadata meta = ReadMetadata(format, data, length);
  Buffer a, b, scratch;
  Rendered rendered;
  if (!Render(image, meta, kFingerprintMaxEdge, &a, &b, &scratch, &rendered)) {
    return IHD_ERR_OUT_OF_MEMORY;
  }
  return ihd_fingerprint_rgba(rendered.rgb, rendered.width, rendered.height, rendered.width * 3, 3,
                              out);
}

struct JpegSink {
  Buffer buffer;
  size_t size = 0;
  size_t capacity = 0;
  bool failed = false;
};

void JpegWrite(void *context, void *data, int size) {
  auto *sink = static_cast<JpegSink *>(context);
  if (sink->failed || size <= 0) return;
  const size_t needed = sink->size + static_cast<size_t>(size);
  if (needed > sink->capacity) {
    size_t next = sink->capacity ? sink->capacity * 2 : 256 * 1024;
    while (next < needed) next *= 2;
    if (!sink->buffer.Reserve(next)) {
      sink->failed = true;
      return;
    }
    sink->capacity = next;
  }
  std::memcpy(sink->buffer.data() + sink->size, data, static_cast<size_t>(size));
  sink->size = needed;
}

void Process(const uint8_t *data, size_t length, int32_t max_bytes, int32_t max_edge,
             int32_t min_edge, int64_t max_source_pixels, ihd_result *r) {
  r->source_size = length;
  uint8_t digest[IHD_SHA256_SIZE];
  ihd_sha256(data, length, digest);
  ToHex(digest, IHD_SHA256_SIZE, r->source_sha256_hex);

  const Format format = SniffFormat(data, length);
  const Metadata meta = ReadMetadata(format, data, length);
  r->orientation = meta.orientation;
  r->color_converted = meta.display_p3 ? 1 : 0;
  std::memcpy(r->capture_time, meta.capture_time, sizeof(r->capture_time));
  std::memcpy(r->capture_offset, meta.capture_offset, sizeof(r->capture_offset));

  Decoded image;
  r->status = DecodeImage(format, data, length, max_source_pixels, &image, &r->source_width,
                          &r->source_height);
  if (r->status != IHD_OK) return;

  Buffer a, b, scratch;
  JpegSink sink;
  int32_t edge = max_edge;
  for (int reduction = 0; reduction <= kMaxEdgeReductions; ++reduction) {
    Rendered rendered;
    if (!Render(image, meta, edge, &a, &b, &scratch, &rendered)) {
      r->status = IHD_ERR_OUT_OF_MEMORY;
      return;
    }

    for (int quality : kQualitySteps) {
      sink.size = 0;
      if (!ihd_stb_write_jpeg(JpegWrite, &sink, rendered.width, rendered.height, rendered.rgb,
                              quality) ||
          sink.failed) {
        r->status = sink.failed ? IHD_ERR_OUT_OF_MEMORY : IHD_ERR_ENCODE_FAILED;
        return;
      }
      if (sink.size > static_cast<size_t>(max_bytes)) continue;

      // Dấu vân tay tính từ CHÍNH file JPEG đầu ra, qua cùng hàm mà client dùng
      // cho ảnh tải về từ server -> cùng byte thì cùng dấu vân tay tuyệt đối.
      uint8_t fingerprint[IHD_FINGERPRINT_SIZE];
      const int32_t fp_status = FingerprintBytes(sink.buffer.data(), sink.size, fingerprint);
      if (fp_status != IHD_OK) {
        r->status = fp_status;
        return;
      }
      ToHex(fingerprint, IHD_FINGERPRINT_SIZE, r->fingerprint_hex);
      ihd_sha256(sink.buffer.data(), sink.size, digest);
      ToHex(digest, IHD_SHA256_SIZE, r->output_sha256_hex);
      r->jpeg = sink.buffer.Release();
      r->jpeg_size = sink.size;
      r->width = rendered.width;
      r->height = rendered.height;
      r->quality = quality;
      r->status = IHD_OK;
      return;
    }

    // Hạ chất lượng hết đường thì hạ cạnh dài x0.8 rồi làm lại.
    const int32_t longest = rendered.width > rendered.height ? rendered.width : rendered.height;
    const int32_t next = (longest * 4 + 2) / 5;
    if (next < min_edge) break;
    edge = next;
  }
  r->status = IHD_ERR_TARGET_NOT_REACHED;
}

ihd_result *NewResult() {
  auto *r = static_cast<ihd_result *>(std::calloc(1, sizeof(ihd_result)));
  if (r) {
    r->status = IHD_ERR_OUT_OF_MEMORY;
    r->orientation = 1;
  }
  return r;
}

void Normalize(int32_t *max_bytes, int32_t *max_edge, int32_t *min_edge,
               int32_t *max_source_pixels) {
  if (*max_bytes <= 0) *max_bytes = IHD_DEFAULT_MAX_BYTES;
  if (*max_edge <= 0) *max_edge = IHD_DEFAULT_MAX_EDGE;
  if (*min_edge <= 0) *min_edge = IHD_DEFAULT_MIN_EDGE;
  if (*min_edge > *max_edge) *min_edge = *max_edge;
  if (*max_source_pixels <= 0) *max_source_pixels = IHD_DEFAULT_MAX_SOURCE_PIXELS;
}

// Đọc toàn bộ file. Đọc thiếu = file trên đám mây / thẻ nhớ chưa tải xong.
int32_t ReadWholeFile(const char *path, Buffer *bytes, size_t *size_out) {
  std::FILE *file = std::fopen(path, "rb");
  if (!file) return IHD_ERR_FILE_UNREADABLE;
  long size = -1;
  if (std::fseek(file, 0, SEEK_END) == 0) size = std::ftell(file);
  if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return IHD_ERR_FILE_UNREADABLE;
  }
  *size_out = static_cast<size_t>(size);
  if (size > IHD_MAX_INPUT_BYTES) {
    std::fclose(file);
    return IHD_ERR_FILE_TOO_LARGE;
  }
  if (!bytes->Reserve(size > 0 ? static_cast<size_t>(size) : 1)) {
    std::fclose(file);
    return IHD_ERR_OUT_OF_MEMORY;
  }
  const size_t read = std::fread(bytes->data(), 1, static_cast<size_t>(size), file);
  std::fclose(file);
  return read == static_cast<size_t>(size) ? IHD_OK : IHD_ERR_FILE_UNREADABLE;
}

}  // namespace

extern "C" {

ihd_result *ihd_process_bytes(const uint8_t *data, size_t length, int32_t max_bytes,
                              int32_t max_edge, int32_t min_edge,
                              int32_t max_source_pixels) {
  ihd_result *r = NewResult();
  if (!r) return nullptr;
  if (!data && length > 0) {
    r->status = IHD_ERR_NULL_POINTER;
    return r;
  }
  Normalize(&max_bytes, &max_edge, &min_edge, &max_source_pixels);
  Process(data, length, max_bytes, max_edge, min_edge, max_source_pixels, r);
  return r;
}

ihd_result *ihd_process_file(const char *path, int32_t max_bytes, int32_t max_edge,
                             int32_t min_edge, int32_t max_source_pixels) {
  ihd_result *r = NewResult();
  if (!r) return nullptr;
  if (!path) {
    r->status = IHD_ERR_NULL_POINTER;
    return r;
  }
  Buffer bytes;
  size_t size = 0;
  r->status = ReadWholeFile(path, &bytes, &size);
  r->source_size = size;
  if (r->status != IHD_OK) return r;
  Normalize(&max_bytes, &max_edge, &min_edge, &max_source_pixels);
  Process(bytes.data(), size, max_bytes, max_edge, min_edge, max_source_pixels, r);
  return r;
}

int32_t ihd_fingerprint_bytes(const uint8_t *data, size_t length, uint8_t *out) {
  return FingerprintBytes(data, length, out);
}

int32_t ihd_fingerprint_file(const char *path, uint8_t *out) {
  if (!path || !out) return IHD_ERR_NULL_POINTER;
  Buffer bytes;
  size_t size = 0;
  const int32_t status = ReadWholeFile(path, &bytes, &size);
  if (status != IHD_OK) return status;
  return FingerprintBytes(bytes.data(), size, out);
}

int32_t ihd_result_status(const ihd_result *r) { return r ? r->status : IHD_ERR_NULL_POINTER; }
const uint8_t *ihd_result_jpeg_data(const ihd_result *r) { return r ? r->jpeg : nullptr; }
size_t ihd_result_jpeg_size(const ihd_result *r) { return r ? r->jpeg_size : 0; }
int32_t ihd_result_width(const ihd_result *r) { return r ? r->width : 0; }
int32_t ihd_result_height(const ihd_result *r) { return r ? r->height : 0; }
int32_t ihd_result_quality(const ihd_result *r) { return r ? r->quality : 0; }
int32_t ihd_result_source_width(const ihd_result *r) { return r ? r->source_width : 0; }
int32_t ihd_result_source_height(const ihd_result *r) { return r ? r->source_height : 0; }
int32_t ihd_result_orientation(const ihd_result *r) { return r ? r->orientation : 1; }
int32_t ihd_result_color_converted(const ihd_result *r) { return r ? r->color_converted : 0; }
size_t ihd_result_source_size(const ihd_result *r) { return r ? r->source_size : 0; }
const char *ihd_result_fingerprint_hex(const ihd_result *r) { return r ? r->fingerprint_hex : ""; }
const char *ihd_result_source_sha256_hex(const ihd_result *r) { return r ? r->source_sha256_hex : ""; }
const char *ihd_result_output_sha256_hex(const ihd_result *r) { return r ? r->output_sha256_hex : ""; }
const char *ihd_result_capture_time(const ihd_result *r) { return r ? r->capture_time : ""; }
const char *ihd_result_capture_offset(const ihd_result *r) { return r ? r->capture_offset : ""; }

void ihd_result_free(ihd_result *r) {
  if (!r) return;
  std::free(r->jpeg);
  std::free(r);
}

const char *ihd_status_name(int32_t status) {
  switch (status) {
    case IHD_OK: return "ok";
    case IHD_ERR_NULL_POINTER: return "null-pointer";
    case IHD_ERR_INVALID_SIZE: return "invalid-size";
    case IHD_ERR_INVALID_CHANNELS: return "invalid-channels";
    case IHD_ERR_INVALID_STRIDE: return "invalid-stride";
    case IHD_ERR_INVALID_HEX: return "invalid-hex";
    case IHD_ERR_NOT_AN_IMAGE: return "not-an-image";
    case IHD_ERR_UNSUPPORTED_FORMAT: return "unsupported-format";
    case IHD_ERR_FILE_UNREADABLE: return "file-unreadable";
    case IHD_ERR_DECODE_FAILED: return "decode-failed";
    case IHD_ERR_TOO_MANY_PIXELS: return "too-many-pixels";
    case IHD_ERR_ENCODE_FAILED: return "encode-failed";
    case IHD_ERR_TARGET_NOT_REACHED: return "target-not-reached";
    case IHD_ERR_OUT_OF_MEMORY: return "out-of-memory";
    case IHD_ERR_FILE_TOO_LARGE: return "file-too-large";
    case IHD_ERR_FILE_TRUNCATED: return "file-truncated";
    default: return "unknown";
  }
}

}  // extern "C"
